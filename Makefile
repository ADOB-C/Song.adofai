CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lm

BIN     = adofai-audio
SRCS    = util.c chart.c pcm.c codec.c commands.c main.c
OBJS    = $(SRCS:.c=.o)
HDRS    = util.h chart.h pcm.h codec.h cli.h

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJS)

test: $(BIN)
	./$(BIN) self-test

install: $(BIN)
	install -m 0755 $(BIN) /usr/local/bin/$(BIN)

.PHONY: all clean test install
