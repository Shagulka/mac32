// Fixes for bugs in games' own 32-bit code, applied in memory when a library
// loads. One file per game library in patches/; each names the exact build
// (SHA-256 of its i386 code) it was written for and steps aside for any other.
#pragma once
#include "thunks.h"

struct image;
struct guest_patch {
	const char *library;	// file name, e.g. "vguimatsurface.dylib"
	const char *sha256;	// of the library's i386 slice: the one build this fixes
	const char *game;	// who ships that build
	const char *what;	// one line for the log
	int (*apply)(struct image *img);	// nonzero when applied
};

// replace the function named fn (local symbols too) with a jump to host code
int patch_jump(struct image *img, const char *fn, struct hfn *h);
