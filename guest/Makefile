# 32-bit guest libraries, linked against a stub i386 libSystem.
CC = clang
CFLAGS = -target i386-apple-macos10.6 -O2 -fno-stack-protector -fno-builtin -Wall
LINK = $(CC) $(CFLAGS) -dynamiclib -nostdlib -Wl,-w

all: selftest.dylib libmac32rt.dylib libmac32cxx.dylib extest.dylib

selftest.dylib: selftest.c libSystem.tbd
	$(LINK) $^ -o $@

libmac32rt.dylib: rt.c rtstring.c objcexc.c setjmp.s libSystem.tbd
	$(LINK) -install_name /usr/lib/libmac32rt.dylib $^ -o $@

clean:
	rm -f *.dylib

# LLVM libunwind + libc++abi for the guest (C++ exceptions, RTTI, dynamic_cast)
libmac32cxx.dylib: cxxrt/build.sh cxxrt/libSystem.tbd
	cxxrt/build.sh
	$(CC) -target i386-apple-macos10.7 -dynamiclib -nostdlib -Wl,-w -install_name /usr/lib/libmac32cxx.dylib \
	  cxxrt/obj/*.o cxxrt/libSystem.tbd -o $@

extest.dylib: extest.cpp libmac32cxx.dylib libSystem.tbd
	$(CC)++ -target i386-apple-macos10.7 -O1 -fno-stack-protector -fexceptions -frtti -nostdinc++ \
	  -I$(HOME)/Developer/mac32-deps/llvm-project/libcxxabi/include -dynamiclib -nostdlib -Wl,-w \
	  extest.cpp libmac32cxx.dylib cxxrt/libSystem.tbd -o $@
