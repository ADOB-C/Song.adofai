CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc/include
LDLIBS   = -lm

# liblzma (xz) — override LZMA_CFLAGS/LZMA_LIBS for custom installs
UNAME_S  := $(shell uname -s)
LZMA_CFLAGS ?=
LZMA_LIBS   ?= -llzma
ifeq ($(UNAME_S),Darwin)
  ifneq (,$(wildcard /opt/homebrew/include/lzma.h))
    LZMA_CFLAGS = -I/opt/homebrew/include
    LZMA_LIBS   = -L/opt/homebrew/lib -llzma
  endif
endif
CFLAGS  += $(LZMA_CFLAGS)
LDLIBS  += $(LZMA_LIBS)

BIN      = build/adofai-audio
SRCS     = src/util.c src/chart.c src/pcm.c src/codec.c src/xz.c src/commands.c src/main.c
HDRS     = src/include/util.h src/include/chart.h src/include/pcm.h src/include/codec.h \
           src/include/xz.h src/include/cli.h
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
