# 32-bit guest libraries, linked against a stub i386 libSystem.
CC = clang
CFLAGS = -target i386-apple-macos10.6 -O2 -fno-stack-protector -fno-builtin -Wall
LINK = $(CC) $(CFLAGS) -dynamiclib -nostdlib -Wl,-w

all: selftest.dylib libmac32rt.dylib

selftest.dylib: selftest.c libSystem.tbd
	$(LINK) $^ -o $@

libmac32rt.dylib: rt.c rtstring.c setjmp.s libSystem.tbd
	$(LINK) -install_name /usr/lib/libmac32rt.dylib $^ -o $@

clean:
	rm -f *.dylib
