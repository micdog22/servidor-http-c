# Servidor HTTP em C

Um servidor web de arquivos estáticos escrito do zero em C, com sockets BSD e pthreads: HTTP/1.1 com keep-alive, IPv4 e IPv6, cache com ETag, pedidos parciais (Range) para vídeo, proteção contra travessia de diretório e log de acesso. Cada parte está explicada abaixo, porque o objetivo é aprender como um servidor de verdade funciona por dentro.

> Feito para estudo e desenvolvimento local. Não tem HTTPS e não passou por auditoria de segurança; para produção, use um servidor consolidado (nginx, Caddy, Apache).

## Recursos

- **Rede**: sockets BSD, IPv6 e IPv4 no mesmo socket (pilha dupla) quando o sistema permite, fila de conexões atendida por um conjunto fixo de threads (pthreads) e encerramento gracioso com Ctrl+C (SIGINT) ou SIGTERM.
- **HTTP/1.1**: GET e HEAD; outros métodos recebem 405 com `Allow`. Keep-alive com pipelining, limites (16 KB de cabeçalho, 100 cabeçalhos, 8 KB de endereço) e prazos (5 s ocioso, 10 s para o cabeçalho chegar, 10 s para o cliente ler a resposta).
- **Arquivos**: decodificação de `%XX`, normalização do caminho com bloqueio de `..` (inclusive disfarçado como `%2e%2e`), `realpath` para impedir fuga por link simbólico, arquivos ocultos protegidos, tabela de tipos MIME, `index.html` ou listagem da pasta com nomes escapados.
- **Cache**: `ETag` e `Last-Modified`, respondendo 304 para `If-None-Match` e `If-Modified-Since`.
- **Range**: um intervalo por pedido (206), 416 quando é impossível, `If-Range`. É o que permite pular para o meio de um vídeo no navegador.
- **Log** no Common Log Format, o mesmo do Apache e do nginx.
- Sem dependências além da biblioteca padrão e de POSIX. Roda em Linux e macOS; no Windows, use o WSL.

## Como usar

```
$ make
$ ./build/servidor-http -d meu-site
Servindo /srv/meu-site em http://localhost:8080/ (IPv6 e IPv4, 8 threads). Ctrl+C para encerrar.
```

Abra `http://localhost:8080/` no navegador. Opções:

```
uso: servidor-http [opções]

  -p, --porta N        porta TCP (padrão: 8080; 0 escolhe uma porta livre)
  -d, --pasta DIR      pasta a servir (padrão: a pasta atual)
  -b, --endereco IP    endereço de escuta, como 127.0.0.1 ou ::1
                       (padrão: todos, IPv6 e IPv4 no mesmo socket)
  -t, --threads N      threads de trabalho (padrão: 8)
      --log ARQ        grava o log de acesso no arquivo (padrão: saída padrão)
      --silencioso     não grava log de acesso
```

Sem `-b`, o servidor aceita conexões de outras máquinas da rede. Para ficar só no seu computador, use `-b 127.0.0.1`.

Uma resposta completa, vista pelo curl:

```
$ curl -i http://localhost:8080/estilo.css
HTTP/1.1 200 OK
Date: Fri, 09 Oct 2026 11:21:55 GMT
Server: servidor-http/1.0
Content-Type: text/css; charset=utf-8
Content-Length: 15
ETag: "364a346-f-6ac8ce52"
Last-Modified: Fri, 09 Oct 2026 11:21:54 GMT
Cache-Control: no-cache
Accept-Ranges: bytes
X-Content-Type-Options: nosniff
Connection: keep-alive
Keep-Alive: timeout=5, max=100

body{margin:0}
```

Pedindo só os 10 primeiros bytes de uma imagem:

```
$ curl -i -r 0-9 http://localhost:8080/fotos/praia.jpg
HTTP/1.1 206 Partial Content
Content-Type: image/jpeg
Content-Length: 10
Content-Range: bytes 0-9/2048
...
```

E uma tentativa de sair da pasta servida (o `--path-as-is` impede o próprio curl de "limpar" o caminho):

```
$ curl -i --path-as-is http://localhost:8080/../etc/passwd
HTTP/1.1 403 Forbidden
```

O log de acesso:

```
::1 - - [09/Oct/2026:11:21:55 +0000] "GET /estilo.css HTTP/1.1" 200 15
::1 - - [09/Oct/2026:11:21:55 +0000] "GET /fotos/praia.jpg HTTP/1.1" 206 10
::1 - - [09/Oct/2026:11:21:55 +0000] "GET /../etc/passwd HTTP/1.1" 403 216
```

## Como compilar

Com make (o binário fica em `build/`):

```sh
make
make test
make clean
```

Com CMake:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Precisa de um compilador C11 (clang ou gcc) e de um sistema POSIX. Tudo que depende do sistema operacional (sockets, threads, sinais) está em `src/main.c`, `src/server.c` e `src/pool.c`; o HTTP em si (`src/http.c`, `src/path.c`, `src/mime.c`) é C puro.

## Testes

```sh
make test
```

- `tests/test_http.c` (unitários, sem rede): analisador de requisições (válidas, incompletas, pipelining e cada tipo de erro), Connection e corpo, decodificação `%XX`, normalização de caminhos e travessia, confinamento na raiz, Range, MIME, escape de HTML, codificação de URL, datas nos três formatos da RFC (conferidas contra `gmtime` e `strftime` de 1970 a 2100), calendário em 1,6 milhão de dias e ETags.
- `tests/integracao.sh`: sobe o servidor numa porta livre servindo uma pasta temporária e confere com o `curl` os códigos 200, 404, 403, 304, 206, 416, 405 e 301, HEAD sem corpo, reaproveitamento da conexão (keep-alive), travessias (`/../etc/passwd`, `%2e%2e%2f`, link simbólico para fora), listagem escapada, 20 clientes simultâneos com `xargs -P 20`, requisições cruas pelo Python (400, 414, 431, 505, pipelining), o log, a pilha dupla (IPv4 e IPv6 no mesmo socket) e o encerramento com SIGINT.

Com sanitizers (o servidor de verdade roda instrumentado no teste de integração, e qualquer relatório reprova o teste):

```sh
make clean
make test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
          LDFLAGS="-fsanitize=address,undefined"
make clean
make test CFLAGS="-O1 -g -fsanitize=thread" LDFLAGS="-fsanitize=thread"
```

## Como funciona

### O ciclo de vida de um socket

Do lado do servidor, uma conexão TCP passa por estas chamadas:

```
socket()   cria o ponto de comunicação (AF_INET6, SOCK_STREAM)
bind()     prende ele a um endereço e porta (:: e 8080)
listen()   passa a aceitar conexões; o sistema faz o aperto de mão de 3 vias
accept()   devolve um socket NOVO para cada cliente; o original segue escutando
read()     lê a requisição
write()    manda a resposta
close()    encerra (ou volta ao read() se for keep-alive)
```

A **pilha dupla** vem de uma opção: um socket IPv6 com `IPV6_V6ONLY` desligado também recebe clientes IPv4, que aparecem com endereços como `::ffff:127.0.0.1`. O log mostra esses clientes como `127.0.0.1`.

O laço principal fica parado num `poll()` esperando o socket de escuta ou um pipe interno. Cada conexão aceita vai para uma **fila limitada**, de onde as threads de trabalho a retiram (o clássico produtor e consumidor, com um mutex e uma variável de condição). Se a fila enche, o servidor responde 503 na hora, em vez de deixar o cliente esperando sem resposta.

### Encerramento gracioso

Um tratador de sinal só pode chamar funções seguras para sinais. O de SIGINT faz duas coisas: liga uma flag atômica e escreve um byte num pipe. O byte acorda o `poll()` da thread principal (o truque do "self-pipe"), que para de aceitar conexões. As threads terminam a resposta em andamento, fecham as conexões ociosas e são esperadas com `pthread_join`. As threads de trabalho nascem com SIGINT bloqueado, para o sinal sempre cair na thread principal.

Escrever num socket que o cliente fechou gera SIGPIPE, que por padrão mata o processo. O servidor ignora esse sinal e trata o erro `EPIPE` que o `write()` devolve.

### O formato de uma mensagem HTTP

Uma requisição é texto: uma linha de pedido, cabeçalhos e uma linha em branco.

```
GET /fotos/praia.jpg HTTP/1.1\r\n       método, alvo e versão
Host: localhost:8080\r\n                obrigatório no HTTP/1.1
Range: bytes=0-9\r\n
\r\n                                    fim do cabeçalho
```

A resposta tem o mesmo formato, com uma linha de status (`HTTP/1.1 206 Partial Content`) e um corpo depois da linha em branco. O tamanho do corpo vem em `Content-Length`, e é assim que o cliente sabe onde termina uma resposta e começa a próxima na mesma conexão.

O analisador (`src/http.c`) só mexe no buffer quando a linha em branco já chegou, então dá para chamá-lo de novo a cada pedaço que o `read()` entrega. Ele segue a RFC 9112 à risca onde isso evita ataques: nome de cabeçalho com espaço antes do `:` é recusado (servidores e proxies que interpretam isso de jeitos diferentes abrem espaço para "request smuggling"), linhas dobradas (obsoletas) também, e HTTP/1.1 sem exatamente um `Host` recebe 400.

### Limites e prazos

Sem limites, qualquer um derruba um servidor: basta mandar um cabeçalho infinito, ou abrir mil conexões e não mandar nada (o ataque Slowloris). Por isso:

| Situação | Limite | Resposta |
|---|---|---|
| Cabeçalho maior que 16 KB ou com mais de 100 linhas | 16 KB / 100 | 431 |
| Endereço pedido maior que 8 KB | 8 KB | 414 |
| Cabeçalho que não termina de chegar | 10 s | 408 |
| Conexão ociosa entre requisições | 5 s | fecha |
| Cliente que não lê a resposta | 10 s (`SO_SNDTIMEO`) | fecha |
| Requisições numa mesma conexão | 100 | fecha |

Quando o servidor fecha a conexão com dados ainda não lidos (por exemplo, depois de um 431), ele primeiro faz `shutdown()` da escrita e descarta o que chegar por até 1 segundo. Fechar direto faria o sistema mandar um RST, que pode apagar a resposta antes de o cliente lê-la.

### Travessia de diretório

O ataque mais clássico contra servidores de arquivos é pedir `GET /../../etc/passwd` e torcer para o servidor juntar o caminho com a pasta servida sem pensar. As variações tentam enganar o filtro: `%2e%2e%2f` (os mesmos `../` codificados), barras duplicadas, links simbólicos. A defesa tem camadas:

1. **Decodificar primeiro, conferir depois.** O `%2e%2e` vira `..` antes de qualquer verificação. Um `%00` é recusado, porque o byte nulo cortaria a string no meio de uma função de C.
2. **Recusar `..`.** Depois de decodificado, qualquer trecho igual a `..` gera 403. Não há tentativa de "resolver" o `..`, o que é mais simples e mais seguro.
3. **`realpath()` e confinamento.** O caminho final passa por `realpath()`, que resolve todos os links simbólicos, e o resultado precisa ser a raiz ou começar com `raiz/`. Só comparar o prefixo não basta: `/srv/site2` começa com `/srv/site`.
4. **Arquivos ocultos.** Trechos que começam com ponto (`.git`, `.env`) recebem 404, para não vazar configuração ou histórico.

Há uma janela de tempo entre a verificação e a abertura (TOCTOU): quem pode mexer nos arquivos da pasta servida poderia trocar um diretório por um link nesse intervalo. O `open()` com `O_NOFOLLOW` fecha a porta no último trecho; para um servidor de desenvolvimento, quem já escreve na pasta servida não precisa atacá-la.

### Keep-alive e pipelining

Abrir uma conexão TCP custa uma ida e volta inteira na rede. No HTTP/1.1 a conexão fica aberta por padrão (`Connection: keep-alive`), e o cliente manda a próxima requisição pelo mesmo socket. No HTTP/1.0 é o contrário: fecha, a não ser que o cliente peça.

O cliente pode até mandar várias requisições seguidas sem esperar as respostas (pipelining). Por isso o servidor nunca descarta o que sobrou no buffer depois de uma requisição: aquilo é o começo da próxima. Uma requisição com corpo (que este servidor não lê) faz a conexão fechar depois da resposta, senão o corpo seria confundido com a próxima requisição.

### Cache: ETag e 304

Cada arquivo ganha uma `ETag` derivada do inode, do tamanho e da data de modificação, e um `Last-Modified`. Na próxima visita o navegador pergunta "mudou?" mandando `If-None-Match: "a ETag"` (ou `If-Modified-Since: a data`). Se nada mudou, a resposta é `304 Not Modified`, sem corpo: só cabeçalhos. Quando os dois vêm juntos, `If-None-Match` tem prioridade, como manda a RFC 9110.

### Pedidos parciais (Range)

Para pular para o meio de um vídeo, o navegador pede só um pedaço do arquivo:

```
Range: bytes=1000000-          a partir do byte 1000000
Range: bytes=0-99              os bytes de 0 a 99 (inclusivo)
Range: bytes=-500              os últimos 500 bytes
```

A resposta é `206 Partial Content`, com `Content-Range: bytes 1000000-1048575/1048576` dizendo qual pedaço vai e qual o tamanho total. Um intervalo impossível (começando depois do fim) recebe `416 Range Not Satisfiable` com `Content-Range: bytes */1048576`. Vários intervalos de uma vez exigiriam uma resposta `multipart/byteranges`; este servidor devolve o arquivo inteiro nesse caso, o que a RFC permite. Com `If-Range`, o intervalo só vale se o arquivo ainda for o mesmo; senão vem o arquivo inteiro.

O envio usa `pread()` em blocos de 64 KB, então servir um vídeo de vários gigabytes não exige memória proporcional ao arquivo.

### Datas sem fuso horário

O HTTP usa datas em GMT no formato `Sun, 06 Nov 1994 08:49:37 GMT`. Em vez de depender de funções que variam entre sistemas (`timegm` não é padrão), `src/http.c` converte entre data e "dias desde 1970" com o algoritmo de Howard Hinnant para o calendário gregoriano. O teste confere a conversão em 1,6 milhão de dias seguidos e compara a formatação com a da biblioteca padrão.

### Log de acesso

Cada requisição vira uma linha no Common Log Format: endereço do cliente, identidade (sempre `-`), usuário (sempre `-`), data, a linha da requisição entre aspas, o status e os bytes de corpo enviados. Aspas, barras invertidas e bytes não imprimíveis do caminho são escapados (`\"`, `\x0A`), para ninguém conseguir forjar linhas no log. Várias threads escrevem no mesmo arquivo, então cada linha é escrita inteira sob um mutex.

## Contribuindo

Issues e pull requests são bem-vindos.

## Licença

MIT. Veja [LICENSE](LICENSE).
