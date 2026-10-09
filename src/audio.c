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

static struct hfn au[] = {
	R("_AudioQueueNewOutput", t_AudioQueueNewOutput),
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
