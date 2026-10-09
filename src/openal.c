// OpenAL for the game's voice chat: plain types, devices/contexts as handles.
#define OPENAL_DEPRECATED
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include "thunks.h"

static struct hfn al[] = {
	G("_alcOpenDevice", alcOpenDevice, "o:p"), G("_alcCloseDevice", alcCloseDevice, "b:o"),
	G("_alcCreateContext", alcCreateContext, "o:op"), G("_alcDestroyContext", alcDestroyContext, "v:o"),
	G("_alcMakeContextCurrent", alcMakeContextCurrent, "b:o"), G("_alcGetCurrentContext", alcGetCurrentContext, "o:"),
	G("_alcGetContextsDevice", alcGetContextsDevice, "o:o"), G("_alcSuspendContext", alcSuspendContext, "v:o"),
	G("_alcProcessContext", alcProcessContext, "v:o"), G("_alcGetError", alcGetError, "i:o"),
	G("_alcGetIntegerv", alcGetIntegerv, "v:oiip"), G("_alcGetString", alcGetString, "S:oi"),
	G("_alcCaptureOpenDevice", alcCaptureOpenDevice, "o:puii"), G("_alcCaptureCloseDevice", alcCaptureCloseDevice, "b:o"),
	G("_alcCaptureStart", alcCaptureStart, "v:o"), G("_alcCaptureStop", alcCaptureStop, "v:o"),
	G("_alcCaptureSamples", alcCaptureSamples, "v:opi"),
	G("_alGetError", alGetError, "i:"), G("_alGetString", alGetString, "S:i"),
	G("_alGenBuffers", alGenBuffers, "v:ip"), G("_alDeleteBuffers", alDeleteBuffers, "v:ip"),
	G("_alGenSources", alGenSources, "v:ip"), G("_alDeleteSources", alDeleteSources, "v:ip"),
	G("_alBufferData", alBufferData, "v:uipii"),
	G("_alGetSourcei", alGetSourcei, "v:uip"), G("_alSourcei", alSourcei, "v:uii"),
	G("_alSourcef", alSourcef, "v:uif"), G("_alSource3f", alSource3f, "v:uifff"),
	G("_alListener3f", alListener3f, "v:ifff"), G("_alListenerf", alListenerf, "v:if"),
	G("_alSourcePlay", alSourcePlay, "v:u"), G("_alSourceStop", alSourceStop, "v:u"),
	G("_alSourcePause", alSourcePause, "v:u"),
	G("_alSourceQueueBuffers", alSourceQueueBuffers, "v:uip"),
	G("_alSourceUnqueueBuffers", alSourceUnqueueBuffers, "v:uip"),
};

void thunks_openal_init(void) { REGISTER(al); }
