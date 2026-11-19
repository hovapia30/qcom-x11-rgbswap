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

LIB         := libqcom_x11_tfp.so
RGBSWAP_LIB := libqcom_x11_rgbswap.so
TEST        := egl_rbtest
SRCS        := src/qcom_x11_tfp_trace.c src/qcom_x11_tfp_fix.c src/qcom_x11_rgbswap.c
RGBSWAP_SRC := src/qcom_x11_rgbswap.c

all: $(LIB) $(RGBSWAP_LIB) $(TEST)

$(LIB): $(SRCS)
	$(CC) $(CFLAGS) -shared -o $@ $(SRCS) -ldl -lX11 -lGL

$(RGBSWAP_LIB): $(RGBSWAP_SRC)
	$(CC) $(CFLAGS) -DQCOM_RGBSWAP_STANDALONE -shared -o $@ $(RGBSWAP_SRC) -ldl

$(TEST): test/egl_rbtest.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lEGL -lGLESv2

install: $(LIB) $(RGBSWAP_LIB)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0644 $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)
	install -m 0755 $(RGBSWAP_LIB) $(DESTDIR)$(LIBDIR)/$(RGBSWAP_LIB)

install-test: $(TEST)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TEST) $(DESTDIR)$(BINDIR)/$(TEST)

clean:
	rm -f $(LIB) libqcom_x11_rgbswap.so $(TEST)

.PHONY: all install install-test clean
