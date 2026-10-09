// Classic Sound Manager channels (Bink's audio) on top of AudioQueue.
//
// Commands go into a per-channel FIFO: bufferCmd queues a sound header's
// samples, callBackCmd calls the channel's 32-bit callBack routine once
// everything queued before it has been played, quietCmd/flushCmd empty the
// queue. An AudioQueue pulls PCM from the FIFO.
//
// Carbon structs are 2-byte packed:
//   SndCommand  {u16 cmd; s16 param1; s32 param2}                  8 bytes
//   SndChannel  {next@0 firstMod@4 callBack@8 userInfo@12 wait@16
//                cmdInProgress@20 flags@28 qLength@30 qHead@32 qTail@34
//                queue[128]@36}                                    1060 bytes
//   SoundHeader (encode at +20): stdSH 0x00 8-bit unsigned mono, samples at +22;
//   extSH 0xFF numChannels@4, numFrames@22, sampleSize@48, samples at +64;
//   cmpSH 0xFE like extSH but format@40 ('twos' big-endian / 'sowt' little)
//   and sampleSize@62.
#include "thunks.h"
#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

enum { nullCmd = 0, quietCmd = 3, flushCmd = 4, reInitCmd = 5, pauseCmd = 11, resumeCmd = 12, callBackCmd = 13, volumeCmd = 46,
       soundCmd = 80, bufferCmd = 81 };

struct item { int kind; int16_t *pcm; uint32_t frames, pos; uint16_t cmd; int16_t p1; int32_t p2; struct item *next; };

struct chan {
	g32 guest;			// guest SndChannel
	g32 callback;			// SndCallBackUPP
	pthread_mutex_t lk;
	struct item *head, *tail;
	AudioQueueRef q;
	double rate;
	int channels;
	float volume;
	int busy;
};

static dispatch_queue_t cbq;		// guest callbacks run here, in order
#define NCHAN 16
static struct chan *chans[NCHAN];

static struct chan *find(g32 g)
{
	for (int i = 0; i < NCHAN; i++) if (chans[i] && chans[i]->guest == g) return chans[i];
	return NULL;
}

static void run_callback(struct chan *c, uint16_t cmd, int16_t p1, int32_t p2)
{
	if (!c->callback) return;
	g32 gc = c->guest, fn = c->callback;
	dispatch_async(cbq, ^{
		g32 sc = gmalloc(8);
		uint8_t *s = G2H(sc);
		memcpy(s, &cmd, 2); memcpy(s + 2, &p1, 2); memcpy(s + 4, &p2, 4);
		gcall(fn, 2, gc, sc);
		gfree(sc);
	});
}

// pull PCM for the AudioQueue; fire callBackCmds as they are reached
static void render(void *ud, AudioQueueRef q, AudioQueueBufferRef b)
{
	struct chan *c = ud;
	int16_t *out = b->mAudioData;
	uint32_t want = b->mAudioDataBytesCapacity / (2 * c->channels), got = 0;
	pthread_mutex_lock(&c->lk);
	while (got < want && c->head) {
		struct item *it = c->head;
		if (it->kind == bufferCmd) {
			uint32_t n = it->frames - it->pos;
			if (n > want - got) n = want - got;
			memcpy(out + got * c->channels, it->pcm + it->pos * c->channels, n * 2 * c->channels);
			it->pos += n;
			got += n;
			if (it->pos < it->frames) break;
		} else if (it->kind == callBackCmd) {
			run_callback(c, it->cmd, it->p1, it->p2);
		}
		c->head = it->next;
		if (!c->head) c->tail = NULL;
		free(it->pcm);
		free(it);
	}
	c->busy = c->head != NULL;
	pthread_mutex_unlock(&c->lk);
	memset(out + got * c->channels, 0, (want - got) * 2 * c->channels);	// silence when starved
	b->mAudioDataByteSize = want * 2 * c->channels;
	AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void start_queue(struct chan *c, double rate, int channels)
{
	if (c->q && c->rate == rate && c->channels == channels) return;
	if (c->q) AudioQueueDispose(c->q, true);
	c->rate = rate;
	c->channels = channels;
	AudioStreamBasicDescription f = {
		.mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
		.mBytesPerPacket = 2 * (uint32_t)channels, .mFramesPerPacket = 1,
		.mBytesPerFrame = 2 * (uint32_t)channels, .mChannelsPerFrame = (uint32_t)channels, .mBitsPerChannel = 16,
	};
	if (AudioQueueNewOutput(&f, render, c, NULL, NULL, 0, &c->q)) { c->q = NULL; return; }
	AudioQueueSetParameter(c->q, kAudioQueueParam_Volume, c->volume);
	uint32_t frames = (uint32_t)(rate / 50);		// 20 ms buffers
	for (int i = 0; i < 3; i++) {
		AudioQueueBufferRef b;
		if (AudioQueueAllocateBuffer(c->q, frames * 2 * (uint32_t)channels, &b)) continue;
		render(c, c->q, b);
	}
	AudioQueueStart(c->q, NULL);
}

static double fixed_rate(uint32_t f) { return f / 65536.0; }

// 16-bit samples: classic Mac data is big-endian, but Intel builds may hand us
// native order; pick the order whose waveform is smoother
static void to_native16(int16_t *s, uint32_t n, int known_big)
{
	if (known_big == 0) return;
	if (known_big < 0) {
		static int said;
		uint64_t d_le = 0, d_be = 0;
		for (uint32_t i = 1; i < n && i < 4096; i++) {
			d_le += (uint64_t)abs(s[i] - s[i - 1]);
			d_be += (uint64_t)abs((int16_t)__builtin_bswap16((uint16_t)s[i]) - (int16_t)__builtin_bswap16((uint16_t)s[i - 1]));
		}
		if (said++ < 3) LOG("sound manager: 16-bit order looks %s\n", d_le <= d_be ? "native" : "big-endian");
		if (d_le <= d_be) return;
	}
	for (uint32_t i = 0; i < n; i++) s[i] = (int16_t)__builtin_bswap16((uint16_t)s[i]);
}

static struct item *decode(g32 h, struct chan *c)
{
	uint8_t *p = G2H(h);
	uint8_t encode = p[20];
	uint32_t samplePtr, rate;
	memcpy(&samplePtr, p, 4);
	memcpy(&rate, p + 8, 4);
	struct item *it = calloc(1, sizeof *it);
	it->kind = bufferCmd;
	if (encode == 0x00) {			// stdSH: 8-bit unsigned mono
		uint32_t len;
		memcpy(&len, p + 4, 4);
		uint8_t *src = samplePtr ? G2H(samplePtr) : p + 22;
		it->frames = len;
		it->pcm = malloc(len * 2 * (size_t)(c->channels ? c->channels : 1));
		start_queue(c, fixed_rate(rate), 1);
		for (uint32_t i = 0; i < len; i++) it->pcm[i] = (int16_t)((src[i] - 128) << 8);
	} else {				// extSH / cmpSH
		uint32_t nch, frames;
		uint16_t bits;
		memcpy(&nch, p + 4, 4);
		memcpy(&frames, p + 22, 4);
		memcpy(&bits, p + (encode == 0xFE ? 62 : 48), 2);
		uint8_t *src = samplePtr ? G2H(samplePtr) : p + 64;
		int big = -1;
		if (encode == 0xFE) {
			uint32_t fmt;
			memcpy(&fmt, p + 40, 4);
			if (fmt == 'twos') big = 1; else if (fmt == 'sowt') big = 0;
		}
		if (nch < 1 || nch > 8) nch = 2;
		start_queue(c, fixed_rate(rate), (int)nch);
		it->frames = frames;
		it->pcm = malloc((size_t)frames * nch * 2);
		static int shown;
		if (shown++ < 3) LOG("sound manager: header %#x rate %.0f ch %u bits %u frames %u order %s\n",
				     encode, fixed_rate(rate), nch, bits, frames, big == 1 ? "big" : big == 0 ? "little" : "auto");
		if (bits == 8) {
			for (uint32_t i = 0; i < frames * nch; i++) it->pcm[i] = (int16_t)((src[i] - 128) << 8);
		} else {
			memcpy(it->pcm, src, (size_t)frames * nch * 2);
			to_native16(it->pcm, frames * nch, big);
		}
	}
	return it;
}

static void push(struct chan *c, struct item *it)
{
	pthread_mutex_lock(&c->lk);
	if (c->tail) c->tail->next = it; else c->head = it;
	c->tail = it;
	c->busy = 1;
	pthread_mutex_unlock(&c->lk);
}

static void flush(struct chan *c)
{
	pthread_mutex_lock(&c->lk);
	for (struct item *it = c->head, *n; it; it = n) { n = it->next; free(it->pcm); free(it); }
	c->head = c->tail = NULL;
	c->busy = 0;
	pthread_mutex_unlock(&c->lk);
}

static int command(struct chan *c, g32 cmdp, int immediate)
{
	uint8_t *s = G2H(cmdp);
	uint16_t cmd;
	int16_t p1;
	int32_t p2;
	memcpy(&cmd, s, 2); memcpy(&p1, s + 2, 2); memcpy(&p2, s + 4, 4);
	cmd &= 0x7fff;			// dataPointerFlag
	switch (cmd) {
	case bufferCmd: case soundCmd:
		if (p2) push(c, decode((g32)p2, c));
		return 0;
	case callBackCmd:
		if (immediate) run_callback(c, cmd, p1, p2);
		else {
			struct item *it = calloc(1, sizeof *it);
			it->kind = callBackCmd; it->cmd = cmd; it->p1 = p1; it->p2 = p2;
			pthread_mutex_lock(&c->lk);
			int empty = c->head == NULL;
			pthread_mutex_unlock(&c->lk);
			if (empty && !c->q) { run_callback(c, cmd, p1, p2); free(it); }	// nothing will play it
			else push(c, it);
		}
		return 0;
	case quietCmd: case flushCmd:
		flush(c);
		return 0;
	case volumeCmd: {
		// param2: right volume in high word, left in low word; 0x100 = full
		float v = ((p2 & 0xffff) + ((uint32_t)p2 >> 16)) / 512.0f;
		c->volume = v > 1 ? 1 : v;
		if (c->q) AudioQueueSetParameter(c->q, kAudioQueueParam_Volume, c->volume);
		return 0;
	}
	case pauseCmd:
		if (c->q) AudioQueuePause(c->q);
		return 0;
	case resumeCmd:
		if (c->q) AudioQueueStart(c->q, NULL);
		return 0;
	case nullCmd: case reInitCmd:
		return 0;
	default:
		LOG("sound manager: command %u not supported\n", cmd);
		return 0;
	}
}

static void t_SndNewChannel(uint32_t *args, struct hres *r)
{
	// SndChannelPtr *chan, short synth, long init, SndCallBackUPP userRoutine
	if (!cbq) cbq = dispatch_queue_create("mac32.soundmanager", DISPATCH_QUEUE_SERIAL);
	g32 *pp = G2H(A(0));
	g32 g = *pp ? *pp : gcalloc(1, 1060);
	*pp = g;
	struct chan *c = calloc(1, sizeof *c);
	pthread_mutex_init(&c->lk, NULL);
	c->guest = g;
	c->callback = A(3);
	c->volume = 1;
	*(g32 *)G2H(g + 8) = A(3);
	for (int i = 0; i < NCHAN; i++) if (!chans[i]) { chans[i] = c; RET(0); return; }
	RET(-201);	// notEnoughHardwareErr
}
static void t_SndDisposeChannel(uint32_t *args, struct hres *r)
{
	struct chan *c = find(A(0));
	if (c) {
		flush(c);
		if (c->q) AudioQueueDispose(c->q, true);
		for (int i = 0; i < NCHAN; i++) if (chans[i] == c) chans[i] = NULL;
		// c leaks: a callback may still be in flight
	}
	RET(0);
}
static void t_SndDoCommand(uint32_t *args, struct hres *r)
{
	struct chan *c = find(A(0));
	RET(c ? command(c, A(1), 0) : -205);	// badChannel
}
static void t_SndDoImmediate(uint32_t *args, struct hres *r)
{
	struct chan *c = find(A(0));
	RET(c ? command(c, A(1), 1) : -205);
}
static void t_SndChannelStatus(uint32_t *args, struct hres *r)
{
	// chan, short length, SCStatus *: scStartTime@0 scEndTime@4 scCurrentTime@8
	// scChannelBusy@12 (Boolean) scChannelDisposed@13 scChannelPaused@14 ...
	struct chan *c = find(A(0));
	if (!c) { RET(-205); return; }
	uint8_t *st = G2H(A(2));
	memset(st, 0, (int16_t)A(1) > 0 ? (int16_t)A(1) : 24);
	st[12] = (uint8_t)c->busy;
	RET(0);
}
static void t_upp(uint32_t *args, struct hres *r) { RET(A(0)); }
static void t_noop(uint32_t *args, struct hres *r) { (void)args; RET(0); }

static struct hfn snd[] = {
	R("_SndNewChannel", t_SndNewChannel), R("_SndDisposeChannel", t_SndDisposeChannel),
	R("_SndDoCommand", t_SndDoCommand), R("_SndDoImmediate", t_SndDoImmediate),
	R("_SndChannelStatus", t_SndChannelStatus),
	R("_NewSndCallBackUPP", t_upp), R("_DisposeSndCallBackUPP", t_noop),
};

void thunks_sndmgr_init(void) { REGISTER(snd); }
