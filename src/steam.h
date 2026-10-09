// Steam interface tables (generated: src/steam_ifaces.c) and the bridge (steam.c).
#pragma once
#include "thunks.h"

struct smethod {
	const char *name;
	const char *sig;	// mac32 signature, first arg 'o' = this
	int ver_arg;		// 1-based param holding the interface version string, 0 if none
};
struct siface {
	const char *cls, *version;
	int n;
	const struct smethod *m;
	int trusted;		// layout checked against libsteam_api's own wrappers
};
extern const struct siface steam_ifaces[];
