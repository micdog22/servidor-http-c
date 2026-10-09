# Makefile do servidor HTTP.
# Uso: make, make test, make clean. CC, CFLAGS e LDFLAGS podem ser trocados:
#   make test CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"
#   make test CFLAGS="-O1 -g -fsanitize=thread" LDFLAGS="-fsanitize=thread"

CFLAGS  ?= -O2 -g
LDFLAGS ?=
WARN    := -std=c11 -Wall -Wextra -Wpedantic
ALL_CFLAGS = $(WARN) -pthread -Isrc $(CFLAGS)

B := build
HEADERS := $(wildcard src/*.h)
CORE := $(B)/http.o $(B)/path.o $(B)/mime.o $(B)/strbuf.o
NET  := $(B)/server.o $(B)/pool.o

.PHONY: all test clean

all: $(B)/servidor-http

$(B):
	mkdir -p $@

$(B)/%.o: src/%.c $(HEADERS) | $(B)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(B)/servidor-http: $(B)/main.o $(NET) $(CORE)
	$(CC) $(ALL_CFLAGS) $^ $(LDFLAGS) -o $@

$(B)/test_http: tests/test_http.c tests/teste.h $(CORE)
	$(CC) $(ALL_CFLAGS) $< $(CORE) $(LDFLAGS) -o $@

test: all $(B)/test_http
	$(B)/test_http
	sh tests/integracao.sh $(B)

clean:
	rm -rf $(B)
