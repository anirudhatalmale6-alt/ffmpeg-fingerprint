# FFmpeg Fingerprint Plugin - Build System
#
# Targets:
#   make tools        - Build standalone tools (ts_fingerprint, sei_reader)
#   make all          - Build everything
#   make clean        - Clean build artifacts
#   make install      - Install to /usr/local/bin
#
# Dependencies:
#   - libzmq (libzmq3-dev on Ubuntu/Debian)
#   - pthread
#   - gcc/clang

CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS =

# Dependencies
ZMQ_CFLAGS = $(shell pkg-config --cflags libzmq 2>/dev/null)
ZMQ_LIBS = $(shell pkg-config --libs libzmq 2>/dev/null || echo "-lzmq")

PTHREAD_LIBS = -lpthread

SRCDIR = src
BUILDDIR = build
BINDIR = bin

# Standalone tools
TOOLS = $(BINDIR)/ts_fingerprint $(BINDIR)/sei_reader $(BINDIR)/ffmpeg_fingerprint

.PHONY: all tools clean install help

all: tools

tools: $(TOOLS)

$(BINDIR)/ts_fingerprint: $(SRCDIR)/ts_fingerprint.c $(SRCDIR)/stb_truetype.h | $(BINDIR)
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) -o $@ $< $(ZMQ_LIBS) $(PTHREAD_LIBS) -lm $(LDFLAGS)
	@echo "Built: $@"

$(BINDIR)/sei_reader: $(SRCDIR)/sei_reader.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

$(BINDIR)/ffmpeg_fingerprint: $(SRCDIR)/ffmpeg_fingerprint.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(BINDIR) $(BUILDDIR)

install: tools
	install -m 755 $(BINDIR)/ts_fingerprint /usr/local/bin/
	install -m 755 $(BINDIR)/sei_reader /usr/local/bin/
	install -m 755 $(BINDIR)/ffmpeg_fingerprint /usr/local/bin/
	@echo "Installed to /usr/local/bin/"

help:
	@echo "FFmpeg Fingerprint Plugin"
	@echo ""
	@echo "Targets:"
	@echo "  make tools     - Build standalone tools (ts_fingerprint, sei_reader, ffmpeg_fingerprint)"
	@echo "  make clean     - Clean build artifacts"
	@echo "  make install   - Install to /usr/local/bin"
	@echo ""
	@echo "For FFmpeg BSF integration, see build_ffmpeg.sh"
