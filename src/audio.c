// Sound: AudioQueue output for the guest (the engine's CAudioDeviceAudioQueue),
// plus pass-through CoreAudio device queries.
//
// AudioQueueBuffer holds pointers, so each guest buffer is an i386 twin:
//   {u32 capacity; ptr data; u32 byteSize; ptr userData; u32 descCap;
//    ptr descs; u32 descCount}  (28 bytes) + 8 bytes holding the host buffer.
// Its sample memory is guest memory; on enqueue the bytes go to the host buffer.
// The queue's output callback and property listeners are host trampolines that
// call the guest functions (on AudioQueue's own thread).
#include "thunks.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dlfcn.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

enum { B_CAP = 0, B_DATA = 4, B_SIZE = 8, B_USER = 12, B_DCAP = 16, B_DESCS = 20, B_DCOUNT = 24, B_HOST = 28 };
#define W(g, off) (*(uint32_t *)G2H((g) + (off)))

struct gqueue { AudioQueueRef q; g32 cb, user, guest; };

static AudioQueueBufferRef host_buf(g32 gb)
{
	AudioQueueBufferRef h;
	memcpy(&h, G2H(gb + B_HOST), sizeof h);
	return h;
}

static void out_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef hb)
{
	(void)q;
	struct gqueue *gq = ud;
	g32 gb = (g32)(uintptr_t)hb->mUserData;
	static uint32_t n;
	n++;
	if (n % 2000 == 1 || n < 4) LOG("audio: output callback #%u enter (thread %p)\n", n, (void *)pthread_self());
	gcall(gq->cb, 3, gq->user, gq->guest, gb);
	if (n < 4) LOG("audio: output callback #%u done\n", n);
}

static void t_AudioQueueNewOutput(uint32_t *args, struct hres *r)
{
	// fmt, callback, userData, runLoop, runLoopMode, flags, *outAQ
	struct gqueue *gq = calloc(1, sizeof *gq);
	gq->cb = A(1);
	gq->user = A(2);
	OSStatus s = AudioQueueNewOutput(G2H(A(0)), out_cb, gq, A(3) ? hobj_unwrap(A(3)) : NULL,
					 A(4) ? hobj_unwrap(A(4)) : NULL, A(5), &gq->q);
	if (s) { free(gq); RET(s); return; }
	gq->guest = hobj_wrap(gq->q);
	*(g32 *)G2H(A(6)) = gq->guest;
	LOG("audio: AudioQueueNewOutput %.0f Hz runloop %#x mode %#x flags %#x -> %#x\n",
	    ((AudioStreamBasicDescription *)G2H(A(0)))->mSampleRate, A(3), A(4), A(5), gq->guest);
	RET(0);
}

static void t_AudioQueueAllocateBuffer(uint32_t *args, struct hres *r)
{
	AudioQueueBufferRef hb;
	OSStatus s = AudioQueueAllocateBuffer(hobj_unwrap(A(0)), A(1), &hb);
	if (s) { RET(s); return; }
	g32 gb = gcalloc(1, 36);
	W(gb, B_CAP) = A(1);
	W(gb, B_DATA) = gcalloc(1, A(1) ? A(1) : 16);
	memcpy(G2H(gb + B_HOST), &hb, sizeof hb);
	hb->mUserData = (void *)(uintptr_t)gb;
	*(g32 *)G2H(A(2)) = gb;
	RET(0);
}

static void t_AudioQueueFreeBuffer(uint32_t *args, struct hres *r)
{
	g32 gb = A(1);
	OSStatus s = AudioQueueFreeBuffer(hobj_unwrap(A(0)), host_buf(gb));
	gfree(W(gb, B_DATA));
	gfree(gb);
	RET(s);
}

static void t_AudioQueueEnqueueBuffer(uint32_t *args, struct hres *r)
{
	// aq, buffer, numPacketDescs, packetDescs (16-byte descs: same layout)
	g32 gb = A(1);
	AudioQueueBufferRef hb = host_buf(gb);
	uint32_t n = W(gb, B_SIZE);
	if (n > hb->mAudioDataBytesCapacity) n = hb->mAudioDataBytesCapacity;
	memcpy(hb->mAudioData, G2H(W(gb, B_DATA)), n);
	hb->mAudioDataByteSize = n;
	if (verbose) {		// with -v: report the level now and then (16-bit PCM)
		static uint32_t count;
		if (++count % 500 == 0) {
			int peak = 0;
			for (uint32_t i = 0; i + 1 < n; i += 2) {
				int v = abs(*(int16_t *)((char *)hb->mAudioData + i));
				if (v > peak) peak = v;
			}
			LOG("audio: buffer #%u, %u bytes, peak %d\n", count, n, peak);
		}
	}
	RET(AudioQueueEnqueueBuffer(hobj_unwrap(A(0)), hb, A(2), A(3) ? G2H(A(3)) : NULL));
}

static void t_AudioQueuePrime(uint32_t *args, struct hres *r)
{
	UInt32 done = 0;
	LOG("audio: AudioQueuePrime(%u frames)...\n", A(1));
	OSStatus s = AudioQueuePrime(hobj_unwrap(A(0)), A(1), A(2) ? &done : NULL);
	LOG("audio: AudioQueuePrime -> %d (%u)\n", (int)s, done);
	if (A(2)) *(uint32_t *)G2H(A(2)) = done;
	RET(s);
}

// property listeners: one trampoline record per (proc, userData)
struct listener { g32 proc, user, gaq; AudioQueuePropertyID id; struct listener *next; };
static struct listener *listeners;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static void listen_cb(void *ud, AudioQueueRef q, AudioQueuePropertyID id)
{
	(void)q;
	struct listener *l = ud;
	LOG("audio: property listener (%.4s) -> guest\n", (char *)&(uint32_t){ __builtin_bswap32(id) });
	gcall(l->proc, 3, l->user, l->gaq, id);
	LOG("audio: property listener done\n");
}
static void t_AudioQueueAddPropertyListener(uint32_t *args, struct hres *r)
{
	struct listener *l = calloc(1, sizeof *l);
	l->proc = A(2); l->user = A(3); l->gaq = A(0); l->id = A(1);
	OSStatus s = AudioQueueAddPropertyListener(hobj_unwrap(A(0)), A(1), listen_cb, l);
	LOG("audio: AddPropertyListener(%.4s) -> %d\n", (char *)&(uint32_t){ __builtin_bswap32(A(1)) }, (int)s);
	if (s) { free(l); RET(s); return; }
	pthread_mutex_lock(&lk);
	l->next = listeners;
	listeners = l;
	pthread_mutex_unlock(&lk);
	RET(0);
}
static void t_AudioQueueRemovePropertyListener(uint32_t *args, struct hres *r)
{
	pthread_mutex_lock(&lk);
	for (struct listener **p = &listeners; *p; p = &(*p)->next) {
		struct listener *l = *p;
		if (l->gaq == A(0) && l->id == A(1) && l->proc == A(2) && l->user == A(3)) {
			*p = l->next;
			pthread_mutex_unlock(&lk);
			RET(AudioQueueRemovePropertyListener(hobj_unwrap(A(0)), A(1), listen_cb, l));
			return;	// l leaks: the queue may still be calling it
		}
	}
	pthread_mutex_unlock(&lk);
	RET(0);
}

static int w_AudioQueueStart(AudioQueueRef q, const AudioTimeStamp *t)
{
	int rc = AudioQueueStart(q, t);
	LOG("audio: AudioQueueStart -> %d\n", rc);
	return rc;
}
static int w_AudioQueueStop(AudioQueueRef q, uint32_t imm)
{
	LOG("audio: AudioQueueStop(%u)...\n", imm & 0xff);
	int rc = AudioQueueStop(q, imm & 0xff);
	LOG("audio: AudioQueueStop -> %d\n", rc);
	return rc;
}
static int w_AudioQueuePause(AudioQueueRef q) { return AudioQueuePause(q); }
static int w_AudioQueueDispose(AudioQueueRef q, uint32_t imm) { return AudioQueueDispose(q, imm & 0xff); }
static int w_AudioQueueReset(AudioQueueRef q) { return AudioQueueReset(q); }
static int w_AudioQueueFlush(AudioQueueRef q) { return AudioQueueFlush(q); }
static int w_AudioQueueSetParameter(AudioQueueRef q, uint32_t id, float v) { return AudioQueueSetParameter(q, id, v); }
static int w_AudioQueueGetParameter(AudioQueueRef q, uint32_t id, float *v) { return AudioQueueGetParameter(q, id, v); }
static int w_AudioQueueGetProperty(AudioQueueRef q, uint32_t id, void *d, UInt32 *n) { return AudioQueueGetProperty(q, id, d, n); }
static int w_AudioQueueSetProperty(AudioQueueRef q, uint32_t id, const void *d, uint32_t n) { return AudioQueueSetProperty(q, id, d, n); }

// ---- device queries (deprecated calls looked up at runtime) -------------------------
static void *sym(const char *n)
{
	void *f = dlsym(RTLD_DEFAULT, n);
	if (!f) LOG("audio: %s not available on this macOS\n", n);
	return f;
}
static void t_AudioHardwareGetProperty(uint32_t *args, struct hres *r)
{
	static OSStatus (*f)(UInt32, UInt32 *, void *);
	if (!f) f = sym("AudioHardwareGetProperty");
	RET(f ? f(A(0), G2H(A(1)), G2H(A(2))) : kAudioHardwareUnspecifiedError);
}
static void t_AudioDeviceGetProperty(uint32_t *args, struct hres *r)
{
	// device, channel, isInput, propID, *size, data
	static OSStatus (*f)(UInt32, UInt32, Boolean, UInt32, UInt32 *, void *);
	if (!f) f = sym("AudioDeviceGetProperty");
	RET(f ? f(A(0), A(1), A(2) & 0xff, A(3), G2H(A(4)), G2H(A(5))) : kAudioHardwareUnspecifiedError);
}
static void t_AudioDeviceGetPropertyInfo(uint32_t *args, struct hres *r)
{
	static OSStatus (*f)(UInt32, UInt32, Boolean, UInt32, UInt32 *, Boolean *);
	if (!f) f = sym("AudioDeviceGetPropertyInfo");
	RET(f ? f(A(0), A(1), A(2) & 0xff, A(3), A(4) ? G2H(A(4)) : NULL, A(5) ? G2H(A(5)) : NULL) : kAudioHardwareUnspecifiedError);
}
static void t_AudioDeviceSetProperty(uint32_t *args, struct hres *r)
{
	// device, when, channel, isInput, propID, size, data
	static OSStatus (*f)(UInt32, const AudioTimeStamp *, UInt32, Boolean, UInt32, UInt32, const void *);
	if (!f) f = sym("AudioDeviceSetProperty");
	RET(f ? f(A(0), A(1) ? G2H(A(1)) : NULL, A(2), A(3) & 0xff, A(4), A(5), G2H(A(6))) : kAudioHardwareUnspecifiedError);
}
static void t_AudioObjectGetPropertyData(uint32_t *args, struct hres *r)
{
	// object, address (3 x u32), qualifierSize, qualifier, *size, data
	RET(AudioObjectGetPropertyData(A(0), G2H(A(1)), A(2), A(3) ? G2H(A(3)) : NULL, G2H(A(4)), G2H(A(5))));
}
// device-change notifications are not forwarded yet: accept and ignore
static void t_ok(uint32_t *args, struct hres *r) { (void)args; RET(0); }

// ---- AudioUnit output (SDL 1.2's sound, ...) -----------------------------------
// Units come from the old Component Manager (FindNextComponent/OpenAComponent)
// or the AudioComponent API; both map onto AudioComponent. Units are guest
// handles. The render callback is a host trampoline: AudioBufferList entries
// are 12 bytes on i386 and 16 on x86_64, so each call fills an i386 copy whose
// sample memory is guest memory, calls the guest, and copies the samples back.
enum { MAXBUF = 8 };
struct gunit {
	AudioUnit u;
	g32 proc, refcon;		// guest AURenderCallback
	g32 flags, ts, abl;		// guest scratch: flags, AudioTimeStamp (64 bytes), buffer list
	g32 data[MAXBUF];
	uint32_t cap[MAXBUF];
	struct gunit *next;
};
static struct gunit *units;

static struct gunit *unit_of(AudioUnit u, int make)
{
	pthread_mutex_lock(&lk);
	struct gunit *g = units;
	while (g && g->u != u) g = g->next;
	if (!g && make) {
		g = calloc(1, sizeof *g);
		g->u = u;
		g->flags = gcalloc(1, 4);
		g->ts = gcalloc(1, 64);
		g->abl = gcalloc(1, 4 + 12 * MAXBUF);
		g->next = units;
		units = g;
	}
	pthread_mutex_unlock(&lk);
	return g;
}

static void unit_forget(AudioUnit u)
{
	pthread_mutex_lock(&lk);
	for (struct gunit **p = &units; *p; p = &(*p)->next)
		if ((*p)->u == u) {
			struct gunit *g = *p;
			*p = g->next;
			for (int i = 0; i < MAXBUF; i++) gfree(g->data[i]);
			gfree(g->flags), gfree(g->ts), gfree(g->abl);
			free(g);
			break;
		}
	pthread_mutex_unlock(&lk);
}

static OSStatus unit_render(void *ud, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
			    UInt32 frames, AudioBufferList *io)
{
	struct gunit *g = ud;
	uint32_t n = io ? io->mNumberBuffers : 0;
	if (n > MAXBUF) n = MAXBUF;
	uint32_t *gl = G2H(g->abl);
	gl[0] = n;
	for (uint32_t i = 0; i < n; i++) {
		AudioBuffer *b = &io->mBuffers[i];
		if (b->mDataByteSize > g->cap[i]) {
			gfree(g->data[i]);
			g->cap[i] = b->mDataByteSize * 2;
			g->data[i] = gcalloc(1, g->cap[i]);
		}
		gl[1 + i * 3] = b->mNumberChannels;
		gl[2 + i * 3] = b->mDataByteSize;
		gl[3 + i * 3] = g->data[i];
	}
	*(uint32_t *)G2H(g->flags) = flags ? *flags : 0;
	if (ts) memcpy(G2H(g->ts), ts, 64);
	OSStatus s = (OSStatus)gcall(g->proc, 6, g->refcon, g->flags, ts ? g->ts : 0, bus, frames, n ? g->abl : 0);
	if (flags) *flags = *(uint32_t *)G2H(g->flags);
	for (uint32_t i = 0; i < n; i++) {
		AudioBuffer *b = &io->mBuffers[i];
		uint32_t size = gl[2 + i * 3];
		if (size > b->mDataByteSize) size = b->mDataByteSize;
		g32 src = gl[3 + i * 3];
		if (src && b->mData) memcpy(b->mData, G2H(src), size);
		b->mDataByteSize = size;
	}
	return s;
}

static void t_FindNextComponent(uint32_t *args, struct hres *r)
{
	// (Component previous, ComponentDescription *): the description has the
	// same five UInt32s as AudioComponentDescription
	AudioComponent c = AudioComponentFindNext(A(0) ? hobj_unwrap(A(0)) : NULL, G2H(A(1)));
	RET(c ? hobj_wrap(c) : 0);
}
static void t_OpenAComponent(uint32_t *args, struct hres *r)
{
	AudioComponentInstance u = NULL;
	OSStatus s = A(0) ? AudioComponentInstanceNew(hobj_unwrap(A(0)), &u) : -50;
	*(g32 *)G2H(A(1)) = s ? 0 : hobj_wrap(u);
	RET(s);
}
static void t_OpenComponent(uint32_t *args, struct hres *r)
{
	AudioComponentInstance u = NULL;
	OSStatus s = A(0) ? AudioComponentInstanceNew(hobj_unwrap(A(0)), &u) : -50;
	RET(s ? 0 : hobj_wrap(u));
}
static void t_CloseComponent(uint32_t *args, struct hres *r)
{
	if (!A(0)) { RET(-50); return; }
	AudioUnit u = hobj_unwrap(A(0));
	AudioOutputUnitStop(u);
	OSStatus s = AudioComponentInstanceDispose(u);
	unit_forget(u);
	RET(s);
}

static void t_AudioUnitSetProperty(uint32_t *args, struct hres *r)
{
	// unit, id, scope, element, data, size
	AudioUnit u = hobj_unwrap(A(0));
	if (A(1) == kAudioUnitProperty_SetRenderCallback && A(4)) {
		struct gunit *g = unit_of(u, 1);
		uint32_t *cb = G2H(A(4));
		g->proc = cb[0];
		g->refcon = cb[1];
		AURenderCallbackStruct h = { g->proc ? unit_render : NULL, g };
		LOG("audio: render callback %#x on unit %#x\n", g->proc, A(0));
		RET(AudioUnitSetProperty(u, A(1), A(2), A(3), &h, sizeof h));
		return;
	}
	RET(AudioUnitSetProperty(u, A(1), A(2), A(3), A(4) ? G2H(A(4)) : NULL, A(5)));
}
static void t_AudioUnitGetProperty(uint32_t *args, struct hres *r)
{
	// unit, id, scope, element, data, *size (scalars and AudioStreamBasicDescription
	// have the same layout on both sides)
	RET(AudioUnitGetProperty(hobj_unwrap(A(0)), A(1), A(2), A(3), A(4) ? G2H(A(4)) : NULL, A(5) ? G2H(A(5)) : NULL));
}
static void t_AudioUnitGetPropertyInfo(uint32_t *args, struct hres *r)
{
	Boolean w = 0;
	OSStatus s = AudioUnitGetPropertyInfo(hobj_unwrap(A(0)), A(1), A(2), A(3), A(4) ? G2H(A(4)) : NULL, &w);
	if (A(5)) *(uint8_t *)G2H(A(5)) = w;
	RET(s);
}
static OSStatus w_AudioUnitInitialize(AudioUnit u) { return AudioUnitInitialize(u); }
static OSStatus w_AudioUnitUninitialize(AudioUnit u) { return AudioUnitUninitialize(u); }
static OSStatus w_AudioUnitReset(AudioUnit u, uint32_t scope, uint32_t el) { return AudioUnitReset(u, scope, el); }
static OSStatus w_AudioOutputUnitStart(AudioUnit u) { LOG("audio: AudioOutputUnitStart\n"); return AudioOutputUnitStart(u); }
static OSStatus w_AudioOutputUnitStop(AudioUnit u) { return AudioOutputUnitStop(u); }
static OSStatus w_AudioUnitSetParameter(AudioUnit u, uint32_t id, uint32_t scope, uint32_t el, float v, uint32_t off)
{
	return AudioUnitSetParameter(u, id, scope, el, v, off);
}
static OSStatus w_AudioUnitGetParameter(AudioUnit u, uint32_t id, uint32_t scope, uint32_t el, float *v)
{
	return AudioUnitGetParameter(u, id, scope, el, v);
}
static void t_AudioComponentInstanceNew(uint32_t *args, struct hres *r)
{
	AudioComponentInstance u = NULL;
	OSStatus s = AudioComponentInstanceNew(hobj_unwrap(A(0)), &u);
	*(g32 *)G2H(A(1)) = s ? 0 : hobj_wrap(u);
	RET(s);
}

// ---- AUGraph (DOSBox's MIDI goes to the DLS synth through one) ---------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void t_NewAUGraph(uint32_t *args, struct hres *r)
{
	AUGraph gr = NULL;
	OSStatus s = NewAUGraph(&gr);
	*(g32 *)G2H(A(0)) = s ? 0 : hobj_wrap(gr);
	RET(s);
}
static OSStatus w_AUGraphNewNode(AUGraph gr, const struct ComponentDescription *d, uint32_t n, const void *data, AUNode *out)
{
	return AUGraphNewNode(gr, d, n, data, out);
}
static OSStatus w_AUGraphAddNode(AUGraph gr, const AudioComponentDescription *d, AUNode *out) { return AUGraphAddNode(gr, d, out); }
static void t_AUGraphGetNodeInfo(uint32_t *args, struct hres *r)
{
	// graph, node, *desc, *classDataSize, **classData, *unit
	AudioUnit u = NULL;
	UInt32 n = 0;
	OSStatus s = AUGraphGetNodeInfo(hobj_unwrap(A(0)), (AUNode)A(1), A(2) ? G2H(A(2)) : NULL, &n, NULL, A(5) ? &u : NULL);
	if (A(3)) *(uint32_t *)G2H(A(3)) = 0;
	if (A(4)) *(uint32_t *)G2H(A(4)) = 0;
	if (A(5)) *(g32 *)G2H(A(5)) = u ? hobj_wrap(u) : 0;
	RET(s);
}
static void t_AUGraphNodeInfo(uint32_t *args, struct hres *r)
{
	// graph, node, *desc, *unit
	AudioUnit u = NULL;
	OSStatus s = AUGraphNodeInfo(hobj_unwrap(A(0)), (AUNode)A(1), A(2) ? G2H(A(2)) : NULL, A(3) ? &u : NULL);
	if (A(3)) *(g32 *)G2H(A(3)) = u ? hobj_wrap(u) : 0;
	RET(s);
}
static OSStatus w_AUGraphConnectNodeInput(AUGraph gr, AUNode a, uint32_t ao, AUNode b, uint32_t bi)
{
	return AUGraphConnectNodeInput(gr, a, ao, b, bi);
}
static OSStatus w_AUGraphOpen(AUGraph gr) { return AUGraphOpen(gr); }
static OSStatus w_AUGraphInitialize(AUGraph gr) { return AUGraphInitialize(gr); }
static OSStatus w_AUGraphUninitialize(AUGraph gr) { return AUGraphUninitialize(gr); }
static OSStatus w_AUGraphStart(AUGraph gr) { return AUGraphStart(gr); }
static OSStatus w_AUGraphStop(AUGraph gr) { return AUGraphStop(gr); }
static OSStatus w_AUGraphClose(AUGraph gr) { return AUGraphClose(gr); }
static OSStatus w_DisposeAUGraph(AUGraph gr) { return DisposeAUGraph(gr); }
static OSStatus w_MusicDeviceMIDIEvent(AudioUnit u, uint32_t st, uint32_t d1, uint32_t d2, uint32_t off)
{
	return MusicDeviceMIDIEvent(u, st, d1, d2, off);
}
static OSStatus w_MusicDeviceSysEx(AudioUnit u, const UInt8 *p, uint32_t n) { return MusicDeviceSysEx(u, p, n); }
#pragma clang diagnostic pop

static struct hfn au[] = {
	R("_AudioQueueNewOutput", t_AudioQueueNewOutput),
	R("_FindNextComponent", t_FindNextComponent), R("_OpenAComponent", t_OpenAComponent),
	R("_OpenComponent", t_OpenComponent), R("_CloseComponent", t_CloseComponent),
	R("_AudioComponentFindNext", t_FindNextComponent), R("_AudioComponentInstanceNew", t_AudioComponentInstanceNew),
	R("_AudioComponentInstanceDispose", t_CloseComponent),
	R("_AudioUnitSetProperty", t_AudioUnitSetProperty), R("_AudioUnitGetProperty", t_AudioUnitGetProperty),
	R("_AudioUnitGetPropertyInfo", t_AudioUnitGetPropertyInfo),
	G("_AudioUnitInitialize", w_AudioUnitInitialize, "i:o"), G("_AudioUnitUninitialize", w_AudioUnitUninitialize, "i:o"),
	G("_AudioUnitReset", w_AudioUnitReset, "i:ouu"),
	G("_AudioOutputUnitStart", w_AudioOutputUnitStart, "i:o"), G("_AudioOutputUnitStop", w_AudioOutputUnitStop, "i:o"),
	G("_AudioUnitSetParameter", w_AudioUnitSetParameter, "i:ouuufu"),
	G("_AudioUnitGetParameter", w_AudioUnitGetParameter, "i:ouuup"),
	R("_NewAUGraph", t_NewAUGraph), G("_AUGraphNewNode", w_AUGraphNewNode, "i:opupp"),
	G("_AUGraphAddNode", w_AUGraphAddNode, "i:opp"),
	R("_AUGraphGetNodeInfo", t_AUGraphGetNodeInfo), R("_AUGraphNodeInfo", t_AUGraphNodeInfo),
	G("_AUGraphConnectNodeInput", w_AUGraphConnectNodeInput, "i:oiuiu"),
	G("_AUGraphOpen", w_AUGraphOpen, "i:o"), G("_AUGraphInitialize", w_AUGraphInitialize, "i:o"),
	G("_AUGraphUninitialize", w_AUGraphUninitialize, "i:o"), G("_AUGraphStart", w_AUGraphStart, "i:o"),
	G("_AUGraphStop", w_AUGraphStop, "i:o"), G("_AUGraphClose", w_AUGraphClose, "i:o"),
	G("_DisposeAUGraph", w_DisposeAUGraph, "i:o"),
	G("_MusicDeviceMIDIEvent", w_MusicDeviceMIDIEvent, "i:ouuuu"), G("_MusicDeviceSysEx", w_MusicDeviceSysEx, "i:opu"),
	R("_AudioQueueAllocateBuffer", t_AudioQueueAllocateBuffer),
	R("_AudioQueueFreeBuffer", t_AudioQueueFreeBuffer),
	R("_AudioQueueEnqueueBuffer", t_AudioQueueEnqueueBuffer),
	R("_AudioQueuePrime", t_AudioQueuePrime),
	R("_AudioQueueAddPropertyListener", t_AudioQueueAddPropertyListener),
	R("_AudioQueueRemovePropertyListener", t_AudioQueueRemovePropertyListener),
	G("_AudioQueueStart", w_AudioQueueStart, "i:op"),
	G("_AudioQueueStop", w_AudioQueueStop, "i:ou"),
	G("_AudioQueuePause", w_AudioQueuePause, "i:o"),
	G("_AudioQueueDispose", w_AudioQueueDispose, "i:ou"),
	G("_AudioQueueReset", w_AudioQueueReset, "i:o"),
	G("_AudioQueueFlush", w_AudioQueueFlush, "i:o"),
	G("_AudioQueueSetParameter", w_AudioQueueSetParameter, "i:ouf"),
	G("_AudioQueueGetParameter", w_AudioQueueGetParameter, "i:oup"),
	G("_AudioQueueGetProperty", w_AudioQueueGetProperty, "i:oupp"),
	G("_AudioQueueSetProperty", w_AudioQueueSetProperty, "i:oupu"),
	R("_AudioHardwareGetProperty", t_AudioHardwareGetProperty),
	R("_AudioDeviceGetProperty", t_AudioDeviceGetProperty),
	R("_AudioDeviceGetPropertyInfo", t_AudioDeviceGetPropertyInfo),
	R("_AudioDeviceSetProperty", t_AudioDeviceSetProperty),
	R("_AudioObjectGetPropertyData", t_AudioObjectGetPropertyData),
	R("_AudioObjectAddPropertyListener", t_ok), R("_AudioObjectRemovePropertyListener", t_ok),
};

void thunks_audio_init(void) { REGISTER(au); }
