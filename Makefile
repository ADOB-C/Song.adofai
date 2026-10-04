CC      ?= cc
CFLAGS  ?= -O2
# -iquote: quoted includes see src/include (e.g. "zstd.h" = ours) while
# <zstd.h> from #include <...> still resolves to the real libzstd header.
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -iquote src/include -Ithird_party
LDLIBS   = -lm

# liblzma (xz) / libzstd — override *_CFLAGS/*_LIBS for custom installs
UNAME_S  := $(shell uname -s)
LZMA_CFLAGS ?=
LZMA_LIBS   ?= -llzma
ZSTD_CFLAGS ?=
ZSTD_LIBS   ?= -lzstd
ifeq ($(UNAME_S),Darwin)
  ifneq (,$(wildcard /opt/homebrew/include/lzma.h))
    LZMA_CFLAGS = -I/opt/homebrew/include
    LZMA_LIBS   = -L/opt/homebrew/lib -llzma
  endif
  ifneq (,$(wildcard /opt/homebrew/include/zstd.h))
    ZSTD_CFLAGS = -I/opt/homebrew/include
    ZSTD_LIBS   = -L/opt/homebrew/lib -lzstd
  endif
  AUDIO_LIBS = -framework CoreAudio -framework CoreFoundation \
               -framework AudioToolbox -framework AudioUnit
endif
CFLAGS  += $(LZMA_CFLAGS) $(ZSTD_CFLAGS)
LDLIBS  += $(LZMA_LIBS) $(ZSTD_LIBS) $(AUDIO_LIBS)

BIN      = build/adofai-music
SRCS     = src/util.c src/chart.c src/pcm.c src/codec.c src/xz.c src/zstd.c \
           src/vorbis.c src/commands.c src/bench.c src/play.c src/main.c
HDRS     = src/include/util.h src/include/chart.h src/include/pcm.h src/include/codec.h \
           src/include/xz.h src/include/zstd.h src/include/ogg.h src/include/cli.h \
           third_party/miniaudio.h third_party/stb_vorbis.c
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
	./$(BIN) -self-test

install: $(BIN)
	install -m 0755 $(BIN) /usr/local/bin/adofai-music

.PHONY: all clean test install
