# Host: x86_64 (runs under Rosetta), pinned above 4 GB so the guest owns the low 4 GB.
CC = clang
CFLAGS = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
         -DGL_SILENCE_DEPRECATION -Wno-missing-field-initializers
LDFLAGS = -arch x86_64 -liconv -lz -framework Foundation -framework AppKit -framework Carbon \
          -framework IOKit -framework OpenGL -framework ApplicationServices -framework ImageIO -framework CFNetwork -framework SystemConfiguration -framework CoreText -Wl,-pagezero_size,0x1000 -Wl,-no_pie -Wl,-image_base,0x200000000 \
          -Wl,-segaddr,__RESERVE,0x1000 -Wl,-segprot,__RESERVE,rwx,---
SRC = src/main.c src/lowmem.c src/cpu.c src/thunks.c src/libc.c src/format.c \
      src/pthread.c src/cxx.c src/libc2.c src/objc.m src/apple.m src/gl_thunks.c src/gl_special.c src/libc3.c src/ats.m src/steam.c src/steam_ifaces.c src/zlib.c src/macho.c src/tramp.s src/invoke.s src/reserve.s
OBJ = $(patsubst src/%,build/%.o,$(SRC))

all: mac32 guest app

guest:
	$(MAKE) -C guest

mac32: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@ 2>&1 | grep -v 'no_pie.*deprecated' || true

build/%.o: src/% src/mac32.h src/thunks.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build mac32
	$(MAKE) -C guest clean

.PHONY: all guest clean

# An app bundle around the same binary, so the game window has an app identity
# (Dock, window tools). Launch through Mac32.app/Contents/MacOS/mac32.
APP = Mac32.app
app: mac32 guest
	mkdir -p $(APP)/Contents/MacOS/guest
	cp mac32 $(APP)/Contents/MacOS/mac32
	cp guest/libmac32rt.dylib $(APP)/Contents/MacOS/guest/
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '<key>CFBundleIdentifier</key><string>com.khesin.mac32</string>' \
	  '<key>CFBundleName</key><string>mac32</string>' \
	  '<key>CFBundleExecutable</key><string>mac32</string>' \
	  '<key>CFBundlePackageType</key><string>APPL</string>' \
	  '<key>NSHighResolutionCapable</key><false/>' \
	  '</dict></plist>' > $(APP)/Contents/Info.plist
	codesign -s - --force $(APP) 2>/dev/null || true
.PHONY: app
