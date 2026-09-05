CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc
LDLIBS   = -lm

BIN      = build/adofai-audio
SRCS     = src/util.c src/chart.c src/pcm.c src/codec.c src/commands.c src/main.c
HDRS     = src/util.h src/chart.h src/pcm.h src/codec.h src/cli.h
OBJS     = $(SRCS:src/%.c=build/obj/%.o)

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

build/obj/%.o: src/%.c $(HDRS)
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build

test: $(BIN)
	./$(BIN) self-test

install: $(BIN)
	install -m 0755 $(BIN) /usr/local/bin/adofai-audio

.PHONY: all clean test install
