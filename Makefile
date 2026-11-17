# Makefile for libqcom_x11_rgbswap
#
#   make            build the shim and the R/B test
#   make install    install the shim into $(PREFIX)/lib (default /usr/local)
#   make clean      remove build artifacts

CC      ?= cc
CFLAGS  ?= -O2 -fPIC -Wall -Wextra
PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
BINDIR  ?= $(PREFIX)/bin

LIB     := libqcom_x11_rgbswap.so
TEST    := egl_rbtest

all: $(LIB) $(TEST)

$(LIB): src/qcom_x11_rgbswap.c
	$(CC) $(CFLAGS) -shared -o $@ $< -ldl

$(TEST): test/egl_rbtest.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lEGL -lGLESv2

install: $(LIB)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0644 $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)

install-test: $(TEST)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TEST) $(DESTDIR)$(BINDIR)/$(TEST)

clean:
	rm -f $(LIB) $(TEST)

.PHONY: all install install-test clean
