/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/linkmachine.h>

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/vfs.h>

/* Scheduling. All GBAs run on one thread. Each frame is cut into slices of QUANTUM cycles;
 * in every slice the GBAs run in order 0..n-1 to the same cycle count. When a GBA starts
 * a serial transfer, its driver stops that GBA right after the register write, the GBAs
 * that have not reached that cycle are run up to it, and the transfer's data is latched
 * from every GBA as it is at that cycle. Completion is scheduled on every participating
 * GBA at start + transfer time. A GBA already past the start cycle (possible only when a
 * GBA other than GBA 0 drives a Normal-mode clock) contributes its current state and
 * receives its completion at once.
 *
 * The machine handles transfer completion itself (the driver's start callback returns
 * false), because the core's own completion event is not part of mGBA savestates; the
 * machine's savestate records every completion in flight.
 *
 * Savestates. mGBA savestates do not hold SIOMLT_SEND, and they restore SIOCNT and RCNT
 * through the register write path. The machine state therefore adds, per GBA, the live
 * SIOCNT, RCNT and SIOMLT_SEND values, the save memory contents, and the link's state. */

#define QUANTUM 2000             /* link scheduling slice in cycles */
#define SAVE_MAX 0x20000         /* largest GBA save memory (1 Mbit flash) */
#define STATE_MAGIC 0x4D4C4247   /* "GBLM" */
#define STATE_VERSION 1
#define HEADER_BYTES 16
#define LINK_BYTES (32 + GBA_LINK_MACHINE_MAX * 24)
#define MODE_NONE 0xFF
#define DRIVER_ID 0x434D4C47     /* "GLMC" */
#define TRACE_MAX 4096

struct GBALinkNodeState {
	uint16_t siocnt;    /* GBASIO.siocnt as the game reads SIOCNT (captured at save) */
	uint16_t rcnt;      /* GBASIO.rcnt (captured at save) */
	uint16_t sioMltSend;/* SIOMLT_SEND / SIODATA8 (captured at save) */
	uint8_t mode;       /* link mode of this GBA, MODE_NONE if not linked */
	uint8_t so;         /* SO line level in Normal mode */
	uint8_t pending;    /* a transfer completion is scheduled on this GBA */
	uint8_t xmode;      /* mode of that transfer */
	uint16_t recv[4];   /* SIOMULTI0-3 to deliver, or Normal data (recv[0] low, recv[1] high) */
	uint32_t doneAt;    /* this GBA's cycle count at completion */
};

struct GBALinkState {
	uint32_t now;       /* machine cycle count (low 32 bits) at the current frame boundary */
	uint8_t inflight;   /* the initiator's transfer has not completed */
	uint8_t initiator;
	uint8_t xmode;
	struct GBALinkMachineStats stats;
	struct GBALinkNodeState node[GBA_LINK_MACHINE_MAX];
};

struct GBALinkNode {
	struct GBASIODriver d;   /* first member: driver callbacks cast back to the node */
	struct GBALinkMachine* m;
	int id;
	struct mCore* core;
	struct GBA* gba;
	mColor* fb;
	unsigned width;
	unsigned height;
	struct mTimingEvent stop;
	struct mTimingEvent sync;
	struct mTimingEvent done;
	bool startRequested;     /* the core asked to start a transfer during this SIOCNT write */
};

struct GBALinkMachine {
	int n;
	bool linked;
	struct GBALinkNode node[GBA_LINK_MACHINE_MAX];
	struct GBALinkState st;
	bool deserializing;       /* ignore driver callbacks while mGBA loads a savestate */
	bool syncPending;         /* a transfer was started and waits for the other GBAs */
	uint32_t syncT;           /* the cycle at which it started */
	size_t coreStateSize;
	bool render;
	unsigned drawMask;
	struct GBALinkMachineTransfer* trace;
	size_t traceLen;
};

/* ---- helpers ------------------------------------------------------------------- */

static inline uint32_t _now(const struct GBALinkNode* nd) {
	return (uint32_t) mTimingCurrentTime(&nd->gba->timing);
}

static inline int32_t _until(uint32_t target, uint32_t t) {
	return (int32_t) (target - t);
}

static inline bool _isNormal(int mode) {
	return mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32;
}

static void _put16(uint8_t* p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}

static void _put32(uint8_t* p, uint32_t v) {
	_put16(p, v);
	_put16(p + 2, v >> 16);
}

static void _put64(uint8_t* p, uint64_t v) {
	_put32(p, v);
	_put32(p + 4, v >> 32);
}

static uint16_t _get16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

static uint32_t _get32(const uint8_t* p) {
	return _get16(p) | ((uint32_t) _get16(p + 2) << 16);
}

static uint64_t _get64(const uint8_t* p) {
	return _get32(p) | ((uint64_t) _get32(p + 4) << 32);
}

/* ---- line levels --------------------------------------------------------------- */

static int _soLevel(struct GBALinkMachine* m, int k) {
	const struct GBALinkNodeState* s = &m->st.node[k];
	return _isNormal(s->mode) ? s->so : 1;
}

/* Normal mode SI: the previous GBA's SO; the first GBA's SI is grounded. */
static void _refreshSi(struct GBALinkMachine* m, int k) {
	if (k < 0 || k >= m->n || !_isNormal(m->st.node[k].mode)) {
		return;
	}
	int si = k == 0 ? 0 : _soLevel(m, k - 1);
	struct GBASIO* sio = &m->node[k].gba->sio;
	sio->siocnt = GBASIONormalSetSi(sio->siocnt, si);
}

static bool _allMulti(struct GBALinkMachine* m) {
	int k;
	for (k = 0; k < m->n; ++k) {
		if (m->st.node[k].mode != GBA_SIO_MULTI) {
			return false;
		}
	}
	return m->linked && m->n > 1;
}

/* Multiplayer SD line ("all ready"): high when every GBA is in multiplayer mode. */
static void _refreshReady(struct GBALinkMachine* m) {
	bool ready = _allMulti(m);
	int k;
	for (k = 0; k < m->n; ++k) {
		if (m->st.node[k].mode != GBA_SIO_MULTI) {
			continue;
		}
		struct GBASIO* sio = &m->node[k].gba->sio;
		sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, ready);
		sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, ready);
	}
}

/* ---- transfers ------------------------------------------------------------------- */

static void _trace(struct GBALinkMachine* m, uint32_t start, int mode, const uint32_t* data) {
	if (!m->trace) {
		return;
	}
	struct GBALinkMachineTransfer* t = &m->trace[m->traceLen++ % TRACE_MAX];
	t->frame = m->st.stats.frame;
	t->start = start;
	t->initiator = m->st.initiator;
	t->mode = mode;
	t->reserved = 0;
	int k;
	for (k = 0; k < GBA_LINK_MACHINE_MAX; ++k) {
		t->data[k] = k < m->n ? data[k] : 0;
	}
}

static void _scheduleDone(struct GBALinkMachine* m, int k, int mode, const uint16_t recv[4], uint32_t at) {
	struct GBALinkNode* nd = &m->node[k];
	struct GBALinkNodeState* s = &m->st.node[k];
	s->pending = 1;
	s->xmode = mode;
	memcpy(s->recv, recv, sizeof(s->recv));
	s->doneAt = at;
	int32_t dt = _until(at, _now(nd));
	if (dt < 0) {
		dt = 0;
		++m->st.stats.late;
	}
	mTimingDeschedule(&nd->gba->timing, &nd->done);
	mTimingSchedule(&nd->gba->timing, &nd->done, dt);
}

static void _startTransfer(struct GBALinkMachine* m, struct GBALinkNode* nd, int mode) {
	m->st.inflight = 1;
	m->st.initiator = nd->id;
	m->st.xmode = mode;
	m->syncPending = true;
	m->syncT = _now(nd);
	++m->st.stats.transfers;
	/* stop this GBA right after the current instruction */
	mTimingDeschedule(&nd->gba->timing, &nd->sync);
	mTimingSchedule(&nd->gba->timing, &nd->sync, 0);
}

static void _latchMulti(struct GBALinkMachine* m, uint32_t start) {
	struct GBA* g0 = m->node[0].gba;
	uint32_t cycles = GBASIOTransferCycles(GBA_SIO_MULTI, g0->sio.siocnt, m->n - 1);
	uint16_t data[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
	uint32_t logged[GBA_LINK_MACHINE_MAX];
	int k;
	for (k = 0; k < m->n; ++k) {
		if (m->st.node[k].mode == GBA_SIO_MULTI) {
			data[k] = m->node[k].gba->memory.io[GBA_REG(SIOMLT_SEND)];
		}
		logged[k] = data[k];
	}
	_trace(m, start, GBA_SIO_MULTI, logged);
	for (k = 0; k < m->n; ++k) {
		if (m->st.node[k].mode != GBA_SIO_MULTI) {
			continue;
		}
		struct GBA* g = m->node[k].gba;
		g->memory.io[GBA_REG(SIOMULTI0)] = 0xFFFF;
		g->memory.io[GBA_REG(SIOMULTI1)] = 0xFFFF;
		g->memory.io[GBA_REG(SIOMULTI2)] = 0xFFFF;
		g->memory.io[GBA_REG(SIOMULTI3)] = 0xFFFF;
		if (k) {
			g->sio.siocnt = GBASIOMultiplayerFillBusy(g->sio.siocnt);
			g->sio.rcnt = GBASIORegisterRCNTClearSc(g->sio.rcnt);
		}
		_scheduleDone(m, k, GBA_SIO_MULTI, data, start + cycles);
	}
}

static void _latchNormal(struct GBALinkMachine* m, uint32_t start) {
	int i0 = m->st.initiator;
	int mode = m->st.xmode;
	bool wide = mode == GBA_SIO_NORMAL_32;
	uint32_t mask = wide ? 0xFFFFFFFFu : 0xFFu;
	uint32_t cycles = GBASIOTransferCycles(mode, m->node[i0].gba->sio.siocnt, 0);
	bool part[GBA_LINK_MACHINE_MAX];
	uint32_t out[GBA_LINK_MACHINE_MAX];
	int k;
	for (k = 0; k < m->n; ++k) {
		struct GBA* g = m->node[k].gba;
		uint16_t c = g->sio.siocnt;
		/* the clock driver, and every GBA armed with external clock in the same width */
		part[k] = k == i0 || (m->st.node[k].mode == mode && GBASIONormalIsStart(c) && !GBASIONormalIsSc(c));
		if (part[k]) {
			out[k] = wide ? (uint32_t) g->memory.io[GBA_REG(SIODATA32_LO)] | ((uint32_t) g->memory.io[GBA_REG(SIODATA32_HI)] << 16)
			              : (uint32_t) (g->memory.io[GBA_REG(SIODATA8)] & 0xFF);
		} else {
			out[k] = _soLevel(m, k) ? mask : 0;   /* a GBA not shifting holds its SO level */
		}
	}
	_trace(m, start, mode, out);
	for (k = 0; k < m->n; ++k) {
		if (!part[k]) {
			continue;
		}
		uint32_t in = (k == 0 ? 0 : out[k - 1]) & mask;
		uint16_t recv[4] = { (uint16_t) in, (uint16_t) (in >> 16), 0, 0 };
		_scheduleDone(m, k, mode, recv, start + cycles);
	}
}

static void _runUntil(struct GBALinkMachine* m, int i, uint32_t target);

/* A transfer was started: bring the GBAs that are behind up to its start, then latch. */
static void _linkSync(struct GBALinkMachine* m) {
	m->syncPending = false;
	uint32_t start = m->syncT;
	int k;
	for (k = 0; k < m->n; ++k) {
		if (k != m->st.initiator && _until(start, _now(&m->node[k])) > 0) {
			_runUntil(m, k, start);
		}
	}
	if (m->st.xmode == GBA_SIO_MULTI) {
		_latchMulti(m, start);
	} else {
		_latchNormal(m, start);
	}
}

static void _finish(struct GBALinkNode* nd, uint32_t cyclesLate) {
	struct GBALinkMachine* m = nd->m;
	struct GBALinkNodeState* s = &m->st.node[nd->id];
	if (!s->pending) {
		return;
	}
	s->pending = 0;
	if (nd->id == m->st.initiator) {
		m->st.inflight = 0;
	}
	struct GBASIO* sio = &nd->gba->sio;
	if (s->xmode == GBA_SIO_MULTI && sio->mode == GBA_SIO_MULTI) {
		GBASIOMultiplayerFinishTransfer(sio, s->recv, cyclesLate);
	} else if (_isNormal(s->xmode) && sio->mode == (enum GBASIOMode) s->xmode) {
		if (s->xmode == GBA_SIO_NORMAL_32) {
			GBASIONormal32FinishTransfer(sio, s->recv[0] | ((uint32_t) s->recv[1] << 16), cyclesLate);
		} else {
			GBASIONormal8FinishTransfer(sio, s->recv[0] & 0xFF, cyclesLate);
		}
		s->so = GBASIONormalGetIdleSo(sio->siocnt);   /* the new SO level is output at completion */
		_refreshSi(m, nd->id + 1);
	}
}

/* ---- SIO driver ---------------------------------------------------------------- */

static bool _drvInit(struct GBASIODriver* d) {
	UNUSED(d);
	return true;
}

static void _drvReset(struct GBASIODriver* d) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	struct GBALinkMachine* m = nd->m;
	struct GBALinkNodeState* s = &m->st.node[nd->id];
	if (m->deserializing) {
		return;
	}
	s->mode = MODE_NONE;
	s->so = 1;
	s->pending = 0;
	nd->startRequested = false;
	mTimingDeschedule(&nd->gba->timing, &nd->done);
}

static uint32_t _drvId(const struct GBASIODriver* d) {
	UNUSED(d);
	return DRIVER_ID;
}

static void _drvSetMode(struct GBASIODriver* d, enum GBASIOMode mode) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	struct GBALinkMachine* m = nd->m;
	if (m->deserializing) {
		return;
	}
	struct GBALinkNodeState* s = &m->st.node[nd->id];
	uint8_t newMode = (mode == GBA_SIO_MULTI || _isNormal(mode)) && m->linked ? (uint8_t) mode : MODE_NONE;
	if (s->mode == newMode) {
		return;
	}
	if (s->pending) {   /* leaving the mode cancels this GBA's part of a transfer */
		s->pending = 0;
		mTimingDeschedule(&nd->gba->timing, &nd->done);
		if (nd->id == m->st.initiator) {
			m->st.inflight = 0;
		}
	}
	s->mode = newMode;
	if (_isNormal(newMode)) {
		s->so = GBASIONormalGetIdleSo(d->p->siocnt);
		_refreshSi(m, nd->id);
	}
	_refreshReady(m);
	_refreshSi(m, nd->id + 1);
}

static bool _drvHandlesMode(struct GBASIODriver* d, enum GBASIOMode mode) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	return nd->m->linked && (mode == GBA_SIO_MULTI || _isNormal(mode));
}

static int _drvConnectedDevices(struct GBASIODriver* d) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	return nd->m->linked ? nd->m->n - 1 : 0;
}

static int _drvDeviceId(struct GBASIODriver* d) {
	return ((struct GBALinkNode*) d)->id;
}

/* The core calls start() before writeSIOCNT() within the same register write, but only
 * writeSIOCNT() sees the value written, so the decision is taken there. */
static bool _drvStart(struct GBASIODriver* d) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	if (!nd->m->deserializing) {
		nd->startRequested = true;
	}
	return false;   /* completion is handled by the machine */
}

static uint16_t _drvWriteSIOCNT(struct GBASIODriver* d, uint16_t value) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	struct GBALinkMachine* m = nd->m;
	struct GBASIO* sio = d->p;
	struct GBALinkNodeState* s = &m->st.node[nd->id];
	bool start = nd->startRequested;
	nd->startRequested = false;
	if (m->deserializing) {
		return value;
	}
	switch (sio->mode) {
	case GBA_SIO_MULTI:
		if (nd->id != 0) {
			/* Busy is read-only for children: it shows the transfer in progress */
			value = (value & ~0x0080) | (sio->siocnt & 0x0080);
		} else if (start) {
			if (!m->st.inflight && _allMulti(m)) {
				_startTransfer(m, nd, GBA_SIO_MULTI);
			} else {
				value = GBASIOMultiplayerClearBusy(value);   /* the parent needs every GBA ready */
				++m->st.stats.rejected;
			}
		}
		value = GBASIOMultiplayerSetReady(value, _allMulti(m));
		break;
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
		if (!GBASIONormalIsStart(value)) {
			s->so = GBASIONormalGetIdleSo(value);   /* with Start set the old SO level is kept */
		}
		value = GBASIONormalSetSi(value, nd->id == 0 ? 0 : _soLevel(m, nd->id - 1));
		if (start && GBASIONormalIsSc(value)) {   /* internal clock: this GBA drives the transfer */
			if (!m->st.inflight) {
				_startTransfer(m, nd, sio->mode);
			} else {
				value = GBASIONormalClearStart(value);
				++m->st.stats.rejected;
			}
		}
		_refreshSi(m, nd->id + 1);
		break;
	default:
		break;
	}
	return value;
}

static uint16_t _drvWriteRCNT(struct GBASIODriver* d, uint16_t value) {
	UNUSED(d);
	return value;
}

static void _drvFinishMultiplayer(struct GBASIODriver* d, uint16_t data[4]) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	memcpy(data, nd->m->st.node[nd->id].recv, sizeof(uint16_t) * 4);
}

static uint8_t _drvFinishNormal8(struct GBASIODriver* d) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	return nd->m->st.node[nd->id].recv[0];
}

static uint32_t _drvFinishNormal32(struct GBASIODriver* d) {
	struct GBALinkNode* nd = (struct GBALinkNode*) d;
	const struct GBALinkNodeState* s = &nd->m->st.node[nd->id];
	return s->recv[0] | ((uint32_t) s->recv[1] << 16);
}

/* ---- events ---------------------------------------------------------------------- */

static void _stopCb(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	((struct GBALinkNode*) context)->gba->earlyExit = true;
}

static void _doneCb(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	_finish(context, cyclesLate);
}

/* ---- scheduler ---------------------------------------------------------------------- */

/* Runs GBA i until its cycle count reaches target, or until it starts a transfer. */
static void _runUntil(struct GBALinkMachine* m, int i, uint32_t target) {
	struct GBALinkNode* nd = &m->node[i];
	struct mTiming* timing = &nd->gba->timing;
	int32_t left = _until(target, _now(nd));
	if (left <= 0) {
		return;
	}
	mTimingDeschedule(timing, &nd->stop);
	mTimingSchedule(timing, &nd->stop, left);
	while (!m->syncPending && _until(target, _now(nd)) > 0) {
		nd->core->runLoop(nd->core);
	}
	mTimingDeschedule(timing, &nd->stop);
}

void GBALinkMachineRunFrame(struct GBALinkMachine* m, const uint16_t* keys) {
	int i;
	for (i = 0; i < m->n; ++i) {
		m->node[i].core->setKeys(m->node[i].core, keys ? (keys[i] & 0x3FF) : 0);
	}
	uint32_t end = m->st.now + GBA_LINK_MACHINE_FRAME_CYCLES;
	uint32_t slice = (m->linked && m->n > 1) ? QUANTUM : GBA_LINK_MACHINE_FRAME_CYCLES;
	uint32_t t = m->st.now;
	while (_until(end, t) > 0) {
		uint32_t q = t + slice;
		if (_until(end, q) < 0) {
			q = end;
		}
		for (i = 0; i < m->n; ++i) {
			_runUntil(m, i, q);
			while (m->syncPending) {
				_linkSync(m);
				_runUntil(m, i, q);
			}
		}
		t = q;
	}
	m->st.now = end;
	++m->st.stats.frame;
}

/* ---- setup ------------------------------------------------------------------------------ */

static bool _openNode(struct GBALinkMachine* m, int i, const struct GBALinkMachineConfig* cfg, char* err, size_t errlen) {
	struct GBALinkNode* nd = &m->node[i];
	nd->m = m;
	nd->id = i;
	struct mCore* core = mCoreFind(cfg->roms[i]);
	if (!core || core->platform(core) != mPLATFORM_GBA) {
		snprintf(err, errlen, "%s: not a GBA ROM", cfg->roms[i]);
		if (core) {
			core->deinit(core);
		}
		return false;
	}
	if (!core->init(core)) {
		snprintf(err, errlen, "%s: core init failed", cfg->roms[i]);
		return false;
	}
	nd->core = core;
	mCoreInitConfig(core, NULL);
	/* ROM hacks that declare widescreen margins get them (doc/widescreen.md): the margins
	 * come from the ROM and change only the picture, so every peer has the same */
	GBACoreAllowWidescreen(core, true);
	mCoreConfigSetIntValue(&core->config, "gba.widescreen", 1);
	if (!mCoreLoadFile(core, cfg->roms[i])) {
		snprintf(err, errlen, "%s: could not load ROM", cfg->roms[i]);
		return false;
	}
	unsigned w, h;
	core->currentVideoSize(core, &w, &h);   /* after loading: the ROM can declare margins */
	nd->fb = calloc((size_t) w * h, sizeof(mColor));
	nd->width = w;
	nd->height = h;
	core->setVideoBuffer(core, nd->fb, w);
	/* always an in-memory save, so nothing next to the ROM is picked up */
	core->loadSave(core, VFileMemChunk(cfg->saves[i], cfg->saves[i] ? cfg->saveSizes[i] : 0));
	nd->gba = core->board;

	nd->stop = (struct mTimingEvent) { .context = nd, .callback = _stopCb, .name = "GBA link machine stop", .priority = 0xFF };
	nd->sync = (struct mTimingEvent) { .context = nd, .callback = _stopCb, .name = "GBA link machine sync", .priority = 0 };
	nd->done = (struct mTimingEvent) { .context = nd, .callback = _doneCb, .name = "GBA link machine transfer", .priority = 0x80 };
	nd->d.init = _drvInit;
	nd->d.reset = _drvReset;
	nd->d.driverId = _drvId;
	nd->d.setMode = _drvSetMode;
	nd->d.handlesMode = _drvHandlesMode;
	nd->d.connectedDevices = _drvConnectedDevices;
	nd->d.deviceId = _drvDeviceId;
	nd->d.writeSIOCNT = _drvWriteSIOCNT;
	nd->d.writeRCNT = _drvWriteRCNT;
	nd->d.start = _drvStart;
	nd->d.finishMultiplayer = _drvFinishMultiplayer;
	nd->d.finishNormal8 = _drvFinishNormal8;
	nd->d.finishNormal32 = _drvFinishNormal32;
	core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &nd->d);
	core->reset(core);
	core->rtc.override = RTC_FAKE_EPOCH;   /* clock advances with emulated time only */
	core->rtc.value = cfg->rtcEpochMs;
	return true;
}

struct GBALinkMachine* GBALinkMachineOpen(const struct GBALinkMachineConfig* cfg, char* err, size_t errlen) {
	if (cfg->gbas < 1 || cfg->gbas > GBA_LINK_MACHINE_MAX) {
		snprintf(err, errlen, "need 1 to %d GBAs", GBA_LINK_MACHINE_MAX);
		return NULL;
	}
	if (cfg->linked && cfg->gbas < 2) {
		snprintf(err, errlen, "a link needs at least 2 GBAs");
		return NULL;
	}
	struct GBALinkMachine* m = calloc(1, sizeof(*m));
	m->n = cfg->gbas;
	m->linked = cfg->linked;
	m->render = true;
	m->drawMask = (1u << m->n) - 1;
	int i;
	for (i = 0; i < GBA_LINK_MACHINE_MAX; ++i) {   /* no GBA is in a link mode yet */
		m->st.node[i].mode = MODE_NONE;
		m->st.node[i].so = 1;
	}
	for (i = 0; i < m->n; ++i) {
		if (!_openNode(m, i, cfg, err, errlen)) {
			GBALinkMachineClose(m);
			return NULL;
		}
	}
	memset(&m->st, 0, sizeof(m->st));
	for (i = 0; i < GBA_LINK_MACHINE_MAX; ++i) {
		m->st.node[i].mode = MODE_NONE;
		m->st.node[i].so = 1;
	}
	for (i = 0; i < m->n; ++i) {   /* modes the GBAs entered while resetting */
		struct GBASIO* sio = &m->node[i].gba->sio;
		if (_drvHandlesMode(&m->node[i].d, sio->mode)) {
			m->st.node[i].mode = sio->mode;
		}
	}
	m->coreStateSize = m->node[0].core->stateSize(m->node[0].core);

	/* Align machine frames with VBlank: GBA 0 runs to the end of its first video frame,
	 * the others to the same cycle. Every later frame is exactly one frame long. */
	struct GBA* g0 = m->node[0].gba;
	uint32_t fc = g0->video.frameCounter;
	while (g0->video.frameCounter == fc) {
		m->node[0].core->runLoop(m->node[0].core);
		while (m->syncPending) {
			_linkSync(m);
		}
	}
	uint32_t t0 = _now(&m->node[0]);
	for (i = 1; i < m->n; ++i) {
		_runUntil(m, i, t0);
		while (m->syncPending) {
			_linkSync(m);
			_runUntil(m, i, t0);
		}
	}
	m->st.now = t0;
	return m;
}

void GBALinkMachineClose(struct GBALinkMachine* m) {
	if (!m) {
		return;
	}
	int i;
	for (i = 0; i < m->n; ++i) {
		struct GBALinkNode* nd = &m->node[i];
		if (nd->core) {
			nd->core->setPeripheral(nd->core, mPERIPH_GBA_LINK_PORT, NULL);
			mCoreConfigDeinit(&nd->core->config);
			nd->core->deinit(nd->core);
		}
		free(nd->fb);
	}
	free(m->trace);
	free(m);
}

/* ---- state ------------------------------------------------------------------------------ */

static void _captureLive(struct GBALinkMachine* m) {
	int k;
	for (k = 0; k < m->n; ++k) {
		struct GBA* g = m->node[k].gba;
		m->st.node[k].siocnt = g->sio.siocnt;
		m->st.node[k].rcnt = g->sio.rcnt;
		m->st.node[k].sioMltSend = g->memory.io[GBA_REG(SIOMLT_SEND)];
	}
}

static void _linkSerialize(struct GBALinkMachine* m, uint8_t* p) {
	_captureLive(m);
	memset(p, 0, LINK_BYTES);
	_put32(p, m->st.now);
	p[4] = m->st.inflight;
	p[5] = m->st.initiator;
	p[6] = m->st.xmode;
	_put64(p + 8, m->st.stats.frame);
	_put32(p + 16, m->st.stats.transfers);
	_put32(p + 20, m->st.stats.rejected);
	_put32(p + 24, m->st.stats.late);
	int k;
	for (k = 0; k < GBA_LINK_MACHINE_MAX; ++k) {
		const struct GBALinkNodeState* s = &m->st.node[k];
		uint8_t* q = p + 32 + k * 24;
		_put16(q, s->siocnt);
		_put16(q + 2, s->rcnt);
		_put16(q + 4, s->sioMltSend);
		q[6] = s->mode;
		q[7] = s->so;
		q[8] = s->pending;
		q[9] = s->xmode;
		_put16(q + 10, s->recv[0]);
		_put16(q + 12, s->recv[1]);
		_put16(q + 14, s->recv[2]);
		_put16(q + 16, s->recv[3]);
		_put32(q + 18, s->doneAt);
	}
}

static void _linkDeserialize(struct GBALinkMachine* m, const uint8_t* p) {
	m->st.now = _get32(p);
	m->st.inflight = p[4];
	m->st.initiator = p[5];
	m->st.xmode = p[6];
	m->st.stats.frame = _get64(p + 8);
	m->st.stats.transfers = _get32(p + 16);
	m->st.stats.rejected = _get32(p + 20);
	m->st.stats.late = _get32(p + 24);
	int k;
	for (k = 0; k < GBA_LINK_MACHINE_MAX; ++k) {
		struct GBALinkNodeState* s = &m->st.node[k];
		const uint8_t* q = p + 32 + k * 24;
		s->siocnt = _get16(q);
		s->rcnt = _get16(q + 2);
		s->sioMltSend = _get16(q + 4);
		s->mode = q[6];
		s->so = q[7];
		s->pending = q[8];
		s->xmode = q[9];
		s->recv[0] = _get16(q + 10);
		s->recv[1] = _get16(q + 12);
		s->recv[2] = _get16(q + 14);
		s->recv[3] = _get16(q + 16);
		s->doneAt = _get32(q + 18);
	}
}

size_t GBALinkMachineStateSize(const struct GBALinkMachine* m) {
	return HEADER_BYTES + (size_t) m->n * (m->coreStateSize + 4 + SAVE_MAX) + LINK_BYTES;
}

void GBALinkMachineSaveState(struct GBALinkMachine* m, void* buffer) {
	uint8_t* p = buffer;
	_put32(p, STATE_MAGIC);
	_put32(p + 4, STATE_VERSION);
	_put32(p + 8, m->n);
	_put32(p + 12, (uint32_t) m->coreStateSize);
	p += HEADER_BYTES;
	int k;
	for (k = 0; k < m->n; ++k) {
		struct GBALinkNode* nd = &m->node[k];
		nd->core->saveState(nd->core, p);
		p += m->coreStateSize;
		struct GBASavedata* sd = &nd->gba->memory.savedata;
		size_t size = sd->data ? GBASavedataSize(sd) : 0;
		if (size > SAVE_MAX) {
			size = SAVE_MAX;
		}
		_put32(p, (uint32_t) size);
		if (size) {
			memcpy(p + 4, sd->data, size);
		}
		p += 4 + SAVE_MAX;
	}
	_linkSerialize(m, p);
}

bool GBALinkMachineLoadState(struct GBALinkMachine* m, const void* buffer) {
	const uint8_t* p = buffer;
	if (_get32(p) != STATE_MAGIC || _get32(p + 4) != STATE_VERSION || (int) _get32(p + 8) != m->n ||
	    _get32(p + 12) != m->coreStateSize) {
		return false;
	}
	p += HEADER_BYTES;
	bool ok = true;
	int k;
	m->deserializing = true;
	for (k = 0; k < m->n; ++k) {
		struct GBALinkNode* nd = &m->node[k];
		if (!nd->core->loadState(nd->core, p)) {
			ok = false;
		}
		p += m->coreStateSize;
		struct GBASavedata* sd = &nd->gba->memory.savedata;
		size_t size = _get32(p);
		if (size && sd->data && size <= GBASavedataSize(sd)) {
			memcpy(sd->data, p + 4, size);
		}
		p += 4 + SAVE_MAX;
	}
	m->deserializing = false;
	_linkDeserialize(m, p);
	m->syncPending = false;
	for (k = 0; k < m->n; ++k) {
		struct GBALinkNode* nd = &m->node[k];
		const struct GBALinkNodeState* s = &m->st.node[k];
		struct GBA* g = nd->gba;
		/* loading cleared every scheduled event, so ours are re-created from the state */
		mTimingDeschedule(&g->timing, &nd->stop);
		mTimingDeschedule(&g->timing, &nd->sync);
		mTimingDeschedule(&g->timing, &nd->done);
		nd->startRequested = false;
		g->sio.siocnt = s->siocnt;
		g->sio.rcnt = s->rcnt;
		g->memory.io[GBA_REG(SIOMLT_SEND)] = s->sioMltSend;
		if (s->pending) {
			int32_t dt = _until(s->doneAt, _now(nd));
			mTimingSchedule(&g->timing, &nd->done, dt > 0 ? dt : 0);
		}
	}
	return ok;
}

/* The checksum is a 64-bit hash with the structure of xxHash64: four independent lanes over
 * 32-byte stripes read as little-endian words, folded to 32 bits at the end. It only has to
 * be identical on every peer and fast, because GekkoNet asks for it with every saved frame,
 * re-simulated frames included; CRC32 (zlib) took about four times as long. */
#define HASH_P1 0x9E3779B185EBCA87ULL
#define HASH_P2 0xC2B2AE3D27D4EB4FULL
#define HASH_P3 0x165667B19E3779F9ULL
#define HASH_P4 0x85EBCA77C2B2AE63ULL

struct GBALinkHash {
	uint64_t v[4];
	uint64_t total;
};

static inline uint64_t _linkHashRotl64(uint64_t x, int r) {
	return (x << r) | (x >> (64 - r));
}

static inline uint64_t _hashRound(uint64_t acc, uint64_t in) {
	acc += in * HASH_P2;
	acc = _linkHashRotl64(acc, 31);
	return acc * HASH_P1;
}

static void _hashInit(struct GBALinkHash* h) {
	h->v[0] = HASH_P1 + HASH_P2;
	h->v[1] = HASH_P2;
	h->v[2] = 0;
	h->v[3] = 0 - HASH_P1;
	h->total = 0;
}

/* data must be 8-byte aligned; a partial last stripe is hashed as if padded with zeros */
static void _hashUpdate(struct GBALinkHash* h, const void* data, size_t size) {
	const uint8_t* p = data;
	uint64_t a = h->v[0], b = h->v[1], c = h->v[2], d = h->v[3];
	size_t i;
	for (i = 0; i + 32 <= size; i += 32) {
		uint64_t w0, w1, w2, w3;
		LOAD_64LE(w0, i, p);
		LOAD_64LE(w1, i + 8, p);
		LOAD_64LE(w2, i + 16, p);
		LOAD_64LE(w3, i + 24, p);
		a = _hashRound(a, w0);
		b = _hashRound(b, w1);
		c = _hashRound(c, w2);
		d = _hashRound(d, w3);
	}
	if (i < size) {
		uint64_t tail[4] = { 0, 0, 0, 0 };
		memcpy(tail, p + i, size - i);
		uint64_t w0, w1, w2, w3;
		LOAD_64LE(w0, 0, tail);
		LOAD_64LE(w1, 8, tail);
		LOAD_64LE(w2, 16, tail);
		LOAD_64LE(w3, 24, tail);
		a = _hashRound(a, w0);
		b = _hashRound(b, w1);
		c = _hashRound(c, w2);
		d = _hashRound(d, w3);
	}
	h->v[0] = a;
	h->v[1] = b;
	h->v[2] = c;
	h->v[3] = d;
	h->total += size;
}

static uint32_t _hashFinish(const struct GBALinkHash* h) {
	uint64_t r = _linkHashRotl64(h->v[0], 1) + _linkHashRotl64(h->v[1], 7) + _linkHashRotl64(h->v[2], 12) + _linkHashRotl64(h->v[3], 18);
	int k;
	for (k = 0; k < 4; ++k) {
		r ^= _hashRound(0, h->v[k]);
		r = r * HASH_P1 + HASH_P4;
	}
	r += h->total;
	r ^= r >> 33;
	r *= HASH_P2;
	r ^= r >> 29;
	r *= HASH_P3;
	r ^= r >> 32;
	return (uint32_t) (r ^ (r >> 32));
}

uint32_t GBALinkMachineChecksum(struct GBALinkMachine* m) {
	struct GBALinkHash h;
	_hashInit(&h);
	int k;
	for (k = 0; k < m->n; ++k) {
		struct GBA* g = m->node[k].gba;
		_hashUpdate(&h, g->memory.wram, GBA_SIZE_EWRAM);
		_hashUpdate(&h, g->memory.iwram, GBA_SIZE_IWRAM);
	}
	uint64_t link[(LINK_BYTES + 7) / 8];
	_linkSerialize(m, (uint8_t*) link);
	_hashUpdate(&h, link, LINK_BYTES);
	return _hashFinish(&h);
}

/* ---- accessors ----------------------------------------------------------------------- */

int GBALinkMachineGBAs(const struct GBALinkMachine* m) {
	return m->n;
}

struct mCore* GBALinkMachineCore(struct GBALinkMachine* m, int gba) {
	return m->node[gba].core;
}

const mColor* GBALinkMachineFramebuffer(const struct GBALinkMachine* m, int gba) {
	return m->node[gba].fb;
}

void GBALinkMachineVideoSize(const struct GBALinkMachine* m, int gba, unsigned* width, unsigned* height) {
	*width = m->node[gba].width;
	*height = m->node[gba].height;
}

uint16_t GBALinkMachineRead16(struct GBALinkMachine* m, int gba, uint32_t address) {
	struct mCore* core = m->node[gba].core;
	if ((address >> 24) == 0x04 && (address & 0x00FFFFFF) < GBA_REG_MAX) {
		return GBAIORead(m->node[gba].gba, address & 0x3FE);   /* the value the game reads */
	}
	return core->rawRead16(core, address, -1);
}

void GBALinkMachineWrite16(struct GBALinkMachine* m, int gba, uint32_t address, uint16_t value) {
	struct mCore* core = m->node[gba].core;
	core->busWrite16(core, address, value);
}

void GBALinkMachineGetStats(const struct GBALinkMachine* m, struct GBALinkMachineStats* out) {
	*out = m->st.stats;
}

/* mGBA draws a frame only while frameskipCounter <= 0; the counter is host state (not in
 * savestates) and drawing has no effect on emulation, so skipping it is safe. */
static void _applyDrawing(struct GBALinkMachine* m) {
	int i;
	for (i = 0; i < m->n; ++i) {
		bool on = m->render && ((m->drawMask >> i) & 1);
		m->node[i].gba->video.frameskipCounter = on ? 0 : 0x3FFFFFFF;
	}
}

void GBALinkMachineSetRender(struct GBALinkMachine* m, bool enable) {
	m->render = enable;
	_applyDrawing(m);
}

void GBALinkMachineSetDrawMask(struct GBALinkMachine* m, unsigned mask) {
	m->drawMask = mask & ((1u << m->n) - 1);
	_applyDrawing(m);
}

void GBALinkMachineTraceEnable(struct GBALinkMachine* m, bool enable) {
	if (enable && !m->trace) {
		m->trace = calloc(TRACE_MAX, sizeof(*m->trace));
	} else if (!enable && m->trace) {
		free(m->trace);
		m->trace = NULL;
	}
	m->traceLen = 0;
}

size_t GBALinkMachineTraceRead(struct GBALinkMachine* m, struct GBALinkMachineTransfer* out, size_t max) {
	if (!m->trace) {
		return 0;
	}
	size_t n = m->traceLen < TRACE_MAX ? m->traceLen : TRACE_MAX;
	size_t first = m->traceLen - n;
	size_t i;
	for (i = 0; i < n && i < max; ++i) {
		out[i] = m->trace[(first + i) % TRACE_MAX];
	}
	m->traceLen = 0;
	return i;
}
