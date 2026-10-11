# Host: x86_64 (runs under Rosetta), pinned above 4 GB so the guest owns the low 4 GB.
CC = clang
CFLAGS = -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
         -DGL_SILENCE_DEPRECATION -Wno-missing-field-initializers
LDFLAGS = -arch x86_64 -liconv -lz -framework Foundation -framework AppKit -framework Carbon \
          -framework IOKit -framework OpenGL -framework ApplicationServices -framework ImageIO -framework CFNetwork -framework SystemConfiguration -framework CoreText -framework AudioToolbox -framework CoreAudio -framework OpenAL -framework CoreVideo -framework UniformTypeIdentifiers -framework Carbon -Wl,-pagezero_size,0x1000 -Wl,-no_pie -Wl,-image_base,0x200000000 \
          -Wl,-segaddr,__RESERVE,0x1000 -Wl,-segprot,__RESERVE,rwx,---
SRC = src/main.c src/lowmem.c src/cpu.c src/thunks.c src/libc.c src/format.c \
      src/pthread.c src/cxx.c src/libc2.c src/objc.m src/apple.m src/apple2.m src/legacy.m src/cg_thunks.c src/cf_thunks.c src/carbon_thunks.c src/patches.c src/srcfont.m src/launcher.m src/gl_thunks.c src/gl_special.c src/libc3.c src/ats.m src/steam.c src/steam_ifaces.c src/zlib.c src/audio.c src/sndmgr.c src/openal.c src/macho.c $(wildcard patches/*.c) src/tramp.s src/invoke.s src/reserve.s
OBJ = $(patsubst patches/%,build/patches/%.o,$(patsubst src/%,build/%.o,$(SRC)))

all: mac32 guest app

guest:
	$(MAKE) -C guest

mac32: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@ 2>&1 | grep -v 'no_pie.*deprecated' || true

build/%.o: src/% src/mac32.h src/thunks.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/patches/%.o: patches/% src/patches.h src/mac32.h src/thunks.h | build
	@mkdir -p build/patches
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build build/patches

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
	cp guest/libmac32rt.dylib guest/libmac32cxx.dylib guest/selftest.dylib guest/extest.dylib $(APP)/Contents/MacOS/guest/
	mkdir -p $(APP)/Contents/Resources
	cp bin/mac32 $(APP)/Contents/Resources/mac32
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '<key>CFBundleIdentifier</key><string>com.khesin.mac32</string>' \
	  '<key>CFBundleName</key><string>mac32</string>' \
	  '<key>CFBundleExecutable</key><string>mac32</string>' \
	  '<key>CFBundlePackageType</key><string>APPL</string>' \
	  '<key>NSHighResolutionCapable</key><false/>' \
	  '<key>CFBundleDisplayName</key><string>mac32</string>' \
	  '<key>CFBundleShortVersionString</key><string>0.1</string>' \
	  '<key>CFBundleDocumentTypes</key><array><dict>' \
	  '<key>CFBundleTypeName</key><string>32-bit Mac app</string>' \
	  '<key>CFBundleTypeRole</key><string>Viewer</string>' \
	  '<key>LSHandlerRank</key><string>Alternate</string>' \
	  '<key>LSItemContentTypes</key><array><string>com.apple.application-bundle</string>' \
	  '<string>public.unix-executable</string></array></dict></array>' \
	  '</dict></plist>' > $(APP)/Contents/Info.plist
	codesign -s - --force $(APP) 2>/dev/null || true
.PHONY: app

# mac32 on your PATH: links bin/mac32 into $(PREFIX)/bin (the engine stays here)
PREFIX ?= $(HOME)/.local
install: app
	mkdir -p $(PREFIX)/bin
	ln -sf $(CURDIR)/bin/mac32 $(PREFIX)/bin/mac32
	@echo "installed $(PREFIX)/bin/mac32 (make sure $(PREFIX)/bin is on your PATH)"
.PHONY: install
