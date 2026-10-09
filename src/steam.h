// Steam interface tables (generated: src/steam_ifaces.c) and the bridge (steam.c).
#pragma once
#include "thunks.h"

struct smethod {
	const char *name;
	const char *flat;	// SteamAPI_<Interface>_<Method> wrapper used to find the slot
	const char *sig;	// mac32 signature, first arg 'o' = this
	int ver_arg;		// 1-based param holding the interface version string, 0 if none
	int slot;		// fixed vtable slot, or -1: probe the flat wrapper at runtime
};
struct siface {
	const char *cls, *version;
	int n;
	const struct smethod *m;	// JSON order; slots are found at runtime
};
extern const struct siface steam_ifaces[];
