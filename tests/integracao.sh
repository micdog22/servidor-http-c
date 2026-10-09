#!/bin/sh
# Teste de integração: sobe o servidor numa porta livre servindo uma pasta
# temporária e confere as respostas com o curl (e com sockets crus via
# Python para as requisições que o curl não consegue mandar).
# Uso: sh tests/integracao.sh <pasta do build>

B=${1:-build}
SERVIDOR="$B/servidor-http"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/servidor-http.XXXXXX") || exit 1
PID=""
cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

total=0
falhas=0
ok() { total=$((total + 1)); }
falha() { total=$((total + 1)); falhas=$((falhas + 1)); echo "FALHOU: $*"; }
confere() { # confere <descrição> <esperado> <obtido>
    if [ "$2" = "$3" ]; then ok; else falha "$1: esperava '$2', veio '$3'"; fi
}

if ! command -v curl > /dev/null 2>&1; then
    echo "curl não encontrado: o teste de integração precisa dele"
    exit 1
fi

# ---------------------------------------------------------------------------
# A pasta servida
# ---------------------------------------------------------------------------
SITE="$TMP/site"
mkdir -p "$SITE/sub/interna" "$TMP/fora"
printf '<!doctype html><title>Início</title><h1>Olá!</h1>\n' > "$SITE/index.html"
printf 'Olá, mundo!\n' > "$SITE/hello.txt"
printf 'x' > "$SITE/sub/<script>&\"aspas\".txt"
printf 'segredo\n' > "$SITE/.segredo"
printf 'nao deveria sair\n' > "$TMP/fora/segredo.txt"
ln -s "$TMP/fora" "$SITE/fora"              # link para fora da raiz
ln -s hello.txt "$SITE/atalho.txt"         # link para dentro: pode
head -c 1048576 /dev/urandom > "$SITE/video.bin"
TAMANHO=1048576

# ---------------------------------------------------------------------------
# Sobe o servidor e descobre a porta
# ---------------------------------------------------------------------------
sobe() { # sobe <argumentos extras...>
    rm -f "$TMP/stderr.log"
    "$SERVIDOR" -p 0 -d "$SITE" -t 4 --log "$TMP/acesso.log" "$@" 2> "$TMP/stderr.log" &
    PID=$!
    i=0
    while [ $i -lt 50 ]; do
        PORTA=$(sed -n 's/.*:\([0-9][0-9]*\)\/ .*/\1/p' "$TMP/stderr.log" 2>/dev/null)
        [ -n "$PORTA" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    echo "o servidor não subiu:"
    cat "$TMP/stderr.log"
    exit 1
}

encerra() { # manda SIGINT e confere o encerramento gracioso
    kill -INT "$PID"
    i=0
    while kill -0 "$PID" 2>/dev/null && [ $i -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    if kill -0 "$PID" 2>/dev/null; then
        falha "o servidor não encerrou com SIGINT"
        kill -9 "$PID"
    else
        wait "$PID"
        confere "código de saída depois do SIGINT" 0 $?
        if grep -q "Encerrado." "$TMP/stderr.log"; then ok; else falha "mensagem de encerramento"; fi
        # Quando compilado com sanitizers, nenhum relatório pode aparecer.
        if grep -Eq "runtime error|AddressSanitizer|ThreadSanitizer|LeakSanitizer" "$TMP/stderr.log"; then
            falha "relatório de sanitizer"
            cat "$TMP/stderr.log"
        else
            ok
        fi
    fi
    PID=""
}

sobe -b 127.0.0.1
U="http://127.0.0.1:$PORTA"
C="curl -s --max-time 10"

codigo() { $C -o /dev/null -w '%{http_code}' "$@"; }

# 200, 404 e conteúdo
confere "GET /" 200 "$(codigo "$U/")"
if $C "$U/" | grep -q '<h1>Olá!</h1>'; then ok; else falha "index.html na raiz"; fi
confere "GET /hello.txt" "Olá, mundo!" "$($C "$U/hello.txt")"
confere "Content-Type de .txt" "text/plain; charset=utf-8" "$($C -o /dev/null -w '%{content_type}' "$U/hello.txt")"
confere "link simbólico dentro da raiz" 200 "$(codigo "$U/atalho.txt")"
confere "GET /nao-existe" 404 "$(codigo "$U/nao-existe")"
confere "arquivo oculto" 404 "$(codigo "$U/.segredo")"

# HEAD: mesmos cabeçalhos, nenhum byte de corpo
$C -I -o "$TMP/head.txt" -w '%{size_download}' "$U/hello.txt" > "$TMP/head-size.txt"
confere "HEAD sem corpo" 0 "$(cat "$TMP/head-size.txt")"
if grep -qi '^Content-Length: 13' "$TMP/head.txt"; then ok; else falha "HEAD com Content-Length"; fi

# ETag e Last-Modified: a segunda visita recebe 304
$C -D "$TMP/h1.txt" -o /dev/null "$U/hello.txt"
ETAG=$(grep -i '^ETag:' "$TMP/h1.txt" | cut -d' ' -f2 | tr -d '\r')
LASTMOD=$(grep -i '^Last-Modified:' "$TMP/h1.txt" | cut -d' ' -f2- | tr -d '\r')
confere "If-None-Match" 304 "$(codigo -H "If-None-Match: $ETAG" "$U/hello.txt")"
confere "If-None-Match diferente" 200 "$(codigo -H 'If-None-Match: "outra"' "$U/hello.txt")"
confere "If-Modified-Since" 304 "$(codigo -H "If-Modified-Since: $LASTMOD" "$U/hello.txt")"
confere "If-Modified-Since antigo" 200 "$(codigo -H 'If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT' "$U/hello.txt")"

# Range: 206 com o pedaço certo, 416 fora do arquivo
$C -r 0-99 -D "$TMP/r.txt" -o "$TMP/parte.bin" -w '%{http_code}' "$U/video.bin" > "$TMP/r-code.txt"
confere "Range 0-99" 206 "$(cat "$TMP/r-code.txt")"
if grep -qi "^Content-Range: bytes 0-99/$TAMANHO" "$TMP/r.txt"; then ok; else falha "Content-Range"; fi
head -c 100 "$SITE/video.bin" > "$TMP/esperado.bin"
if cmp -s "$TMP/parte.bin" "$TMP/esperado.bin"; then ok; else falha "bytes do intervalo"; fi
$C -H 'Range: bytes=-10' -o "$TMP/fim.bin" "$U/video.bin"
tail -c 10 "$SITE/video.bin" > "$TMP/esperado.bin"
if cmp -s "$TMP/fim.bin" "$TMP/esperado.bin"; then ok; else falha "Range de sufixo"; fi
$C -H 'Range: bytes=2000000-' -D "$TMP/r416.txt" -o /dev/null "$U/video.bin"
if grep -q '416' "$TMP/r416.txt" && grep -qi "^Content-Range: bytes \*/$TAMANHO" "$TMP/r416.txt"; then ok; else falha "416"; fi
$C -o "$TMP/inteiro.bin" "$U/video.bin"
if cmp -s "$TMP/inteiro.bin" "$SITE/video.bin"; then ok; else falha "arquivo de 1 MB inteiro"; fi

# Métodos
$C -X POST -d 'a=1' -D "$TMP/post.txt" -o /dev/null "$U/"
if grep -q '405' "$TMP/post.txt" && grep -qi '^Allow: GET, HEAD' "$TMP/post.txt"; then ok; else falha "405 com Allow"; fi
confere "DELETE" 405 "$(codigo -X DELETE "$U/hello.txt")"

# Travessia de diretório
confere "/../etc/passwd" 403 "$(codigo --path-as-is "$U/../etc/passwd")"
confere "%2e%2e%2f" 403 "$(codigo "$U/%2e%2e%2fetc%2fpasswd")"
confere "/sub/%2e%2e/%2e%2e/" 403 "$(codigo "$U/sub/%2e%2e/%2e%2e/etc/passwd")"
confere "link simbólico para fora" 403 "$(codigo "$U/fora/segredo.txt")"
confere "%00 no caminho" 400 "$(codigo "$U/hello.txt%00.png")"

# Pastas
$C -D "$TMP/dir.txt" -o /dev/null "$U/sub"
if grep -q '301' "$TMP/dir.txt" && grep -qi '^Location: /sub/' "$TMP/dir.txt"; then ok; else falha "301 para /sub/"; fi
$C "$U/sub/" > "$TMP/lista.html"
if grep -q '>&lt;script&gt;&amp;&quot;aspas&quot;.txt<' "$TMP/lista.html" && ! grep -q '<script>' "$TMP/lista.html" &&
   grep -q 'href="%3Cscript%3E%26%22aspas%22.txt"' "$TMP/lista.html"; then
    ok
else
    falha "listagem escapada"
fi
if grep -q 'href="interna/"' "$TMP/lista.html"; then ok; else falha "subpasta na listagem"; fi

# Keep-alive: a segunda transferência reaproveita a conexão (0 conexões novas)
confere "keep-alive" "1 0" "$($C -o /dev/null -o /dev/null -w '%{num_connects} ' "$U/hello.txt" "$U/hello.txt" | sed 's/ $//')"

# 20 clientes ao mesmo tempo
seq 1 100 | xargs -P 20 -I{} curl -s --max-time 10 -o /dev/null -w '%{http_code}\n' "$U/hello.txt?{}" > "$TMP/paralelo.txt"
confere "100 requisições com 20 em paralelo" 100 "$(grep -c '^200$' "$TMP/paralelo.txt")"

# Requisições cruas, que o curl não manda
if command -v python3 > /dev/null 2>&1; then
    python3 - "$PORTA" > "$TMP/cru.txt" 2>&1 <<'EOF'
import socket, sys
port = int(sys.argv[1])

def raw(data):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    try:
        s.sendall(data)
    except OSError:
        pass
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except OSError:
        pass
    s.close()
    return out

checks = [
    ("HTTP/1.1 sem Host", raw(b"GET / HTTP/1.1\r\n\r\n"), b"HTTP/1.1 400 "),
    ("HTTP/2.0", raw(b"GET / HTTP/2.0\r\nHost: x\r\n\r\n"), b"HTTP/1.1 505 "),
    ("lixo", raw(b"isto nao e http\r\n\r\n"), b"HTTP/1.1 400 "),
    ("cabecalho de 20 KB", raw(b"GET / HTTP/1.1\r\nHost: x\r\nX-A: " + b"a" * 20000 + b"\r\n\r\n"), b"HTTP/1.1 431 "),
    ("URI de 9 KB", raw(b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n"), b"HTTP/1.1 414 "),
    ("HTTP/1.0 sem Host", raw(b"GET /hello.txt HTTP/1.0\r\n\r\n"), b"HTTP/1.1 200 "),
]
bad = 0
for name, got, prefix in checks:
    if not got.startswith(prefix):
        print("falhou:", name, got[:60])
        bad += 1
pipe = raw(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
if pipe.count(b"HTTP/1.1 200 OK") != 2 or pipe.count("Olá, mundo!".encode()) != 2:
    print("falhou: pipelining", pipe[:80])
    bad += 1
close = raw(b"GET /hello.txt HTTP/1.0\r\n\r\n")
if b"Connection: close" not in close:
    print("falhou: HTTP/1.0 deveria fechar")
    bad += 1
sys.exit(1 if bad else 0)
EOF
    if [ $? -eq 0 ]; then ok; else falha "requisições cruas"; cat "$TMP/cru.txt"; fi
else
    echo "python3 não encontrado: pulando as requisições cruas"
fi

# Log de acesso no Common Log Format
if grep -Eq '^127\.0\.0\.1 - - \[[0-9]{2}/[A-Z][a-z]{2}/[0-9]{4}:[0-9:]{8} \+0000\] "GET /hello.txt HTTP/1.1" 200 13$' "$TMP/acesso.log"; then
    ok
else
    falha "linha do log de acesso"
fi
if grep -q '"GET /%2e%2e%2fetc%2fpasswd HTTP/1.1" 403 ' "$TMP/acesso.log"; then ok; else falha "tentativa de travessia no log"; fi

encerra

# Pilha dupla: sem -b, o mesmo socket atende IPv4 e IPv6.
sobe
if grep -q "IPv6 e IPv4" "$TMP/stderr.log"; then
    confere "IPv4 no socket de pilha dupla" 200 "$(codigo "http://127.0.0.1:$PORTA/hello.txt")"
    confere "IPv6 no socket de pilha dupla" 200 "$(codigo -g "http://[::1]:$PORTA/hello.txt")"
else
    echo "sistema sem IPv6: pulando o teste de pilha dupla"
fi
encerra

echo "integracao.sh: $total verificações, $falhas falhas"
[ "$falhas" -eq 0 ]
