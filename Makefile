CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lm

BIN     = adofai-audio

all: $(BIN)

$(BIN): adofai_audio.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BIN)

test: $(BIN)
	./$(BIN) self-test

install: $(BIN)
	install -m 0755 $(BIN) /usr/local/bin/$(BIN)

.PHONY: all clean test install
