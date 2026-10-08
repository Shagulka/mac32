# Host: x86_64 (runs under Rosetta), pinned above 4 GB so the guest owns the low 4 GB.
CC = clang
CFLAGS = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
LDFLAGS = -arch x86_64 -Wl,-pagezero_size,0x1000 -Wl,-no_pie -Wl,-image_base,0x200000000 \
          -Wl,-segaddr,__RESERVE,0x1000 -Wl,-segprot,__RESERVE,rwx,---
SRC = src/main.c src/lowmem.c src/cpu.c src/thunks.c src/libc.c src/format.c \
      src/pthread.c src/cxx.c src/macho.c src/tramp.s src/invoke.s src/reserve.s
OBJ = $(patsubst src/%,build/%.o,$(SRC))

mac32: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@ 2>&1 | grep -v 'no_pie.*deprecated' || true

build/%.o: src/% src/mac32.h src/thunks.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build mac32
