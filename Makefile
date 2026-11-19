# Makefile for qcom-x11-rgbswap shims
#
#   make            build shims and the R/B test
#   make install    install shims into $(PREFIX)/lib (default /usr/local)
#   make clean      remove build artifacts

CC      ?= cc
CFLAGS  ?= -O2 -fPIC -Wall -Wextra
PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
BINDIR  ?= $(PREFIX)/bin

LIB_RGBSWAP  := libqcom_x11_rgbswap.so
LIB_TFP      := libqcom_x11_tfp.so
TEST         := egl_rbtest

all: $(LIB_RGBSWAP) $(LIB_TFP) $(TEST)

$(LIB_RGBSWAP): src/qcom_x11_rgbswap.c
	$(CC) $(CFLAGS) -shared -o $@ $< -ldl

$(LIB_TFP): src/qcom_x11_tfp_trace.c src/qcom_x11_tfp_fix.c
	$(CC) $(CFLAGS) -shared -o $@ src/qcom_x11_tfp_trace.c src/qcom_x11_tfp_fix.c -ldl -lX11 -lGL

$(TEST): test/egl_rbtest.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lEGL -lGLESv2

install: $(LIB_RGBSWAP) $(LIB_TFP)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0644 $(LIB_RGBSWAP) $(DESTDIR)$(LIBDIR)/$(LIB_RGBSWAP)
	install -m 0644 $(LIB_TFP) $(DESTDIR)$(LIBDIR)/$(LIB_TFP)

install-test: $(TEST)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TEST) $(DESTDIR)$(BINDIR)/$(TEST)

clean:
	rm -f $(LIB_RGBSWAP) $(LIB_TFP) $(TEST)

.PHONY: all install install-test clean
