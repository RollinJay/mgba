/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay.h>

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba-util/socket.h>
#include <mgba-util/vfs.h>

#include <gekkonet.h>

#include <getopt.h>
#include <inttypes.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* mgba-netplay-test: runs a netplay profile headless, for testing and measuring.
 *
 *   identity  print the profile's identity hash and exit
 *   synctest  one process plays every player under GekkoNet's stress session, which
 *             keeps rolling back and re-simulating, and compares checksums
 *   peer      one player per process over UDP; --peers lists the other players
 *   bench     per-frame costs: frames drawn, own GBA drawn, undrawn; save, load, checksum
 *
 * Every run first plays the profile's intro from power-on (the same inputs everywhere, so
 * every machine arrives in the same state). Session inputs are scripted: random moves and
 * buttons that are a function of (seed, player, input number), never START or SELECT. */

#define FRAME_NS ((int64_t) 1000000000 * GBA_LINK_MACHINE_FRAME_CYCLES / 16777216)

static void _nolog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

static struct mLogger _logger = { .log = _nolog };

static int64_t _nowNs(void) {
#ifdef _WIN32
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (int64_t) ((double) c.QuadPart * 1e9 / (double) f.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

static void _sleepNs(int64_t ns) {
	if (ns <= 0) {
		return;
	}
#ifdef _WIN32
	Sleep((DWORD) (ns / 1000000));
#else
	struct timespec ts = { ns / 1000000000, ns % 1000000000 };
	nanosleep(&ts, NULL);
#endif
}

static uint32_t _hash(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7FEB352D;
	x ^= x >> 15;
	x *= 0x846CA68B;
	x ^= x >> 16;
	return x;
}

/* scripted keys: a function of (seed, player, input number), held for 6 frames */
static uint16_t _bot(uint32_t seed, int player, int n) {
	static const uint16_t moves[] = { 0, 0x10, 0x20, 0x80, 0x40, 0x50, 0xA0 };
	static const uint16_t buttons[] = { 0, 0, 0x001, 0x002, 0x200, 0x100 };
	uint32_t h = _hash(seed * 7919 + player * 104729 + (uint32_t) (n / 6) * 2654435761u);
	return moves[h % 7] | buttons[(h >> 8) % 6];
}

struct Options {
	const char* mode;
	const char* profile;
	int player;
	int port;
	const char* peers;
	int frames;
	uint32_t seed;
	int delay;
	int latencyMs;
	int jitterMs;
	double loss;
	const char* trace;
	int check;
	int benchFrames;
	bool breakDeterminism;
	int watches;
	int watchGba[16];
	uint32_t watchAddr[16];
	int watchCount[16];
	const char* shots;
};

static void _usage(const char* arg0) {
	fprintf(stderr,
	        "usage: %s -m identity|synctest|peer|bench -p profile.ini [options]\n"
	        "  -f frames      session frames after the intro (default 1800)\n"
	        "  -s seed        scripted input seed (default 1)\n"
	        "  -c check       synctest: rollback distance (default 7)\n"
	        "  -P player      peer: local player number, from 1\n"
	        "  -o port        peer: local UDP port\n"
	        "  -r list        peer: other players, e.g. 2=127.0.0.1:7002,3=10.0.0.5:7003\n"
	        "  -d delay       peer: input delay in frames (default 1)\n"
	        "  -L ms -J ms    peer: added latency and jitter on outgoing packets\n"
	        "  -X fraction    peer: outgoing packet loss\n"
	        "  -t file        write 'frame checksum keys...' for every frame\n"
	        "  -b frames      bench: frames per measurement (default 600)\n"
	        "  -w gba:addr:n  print n halfwords at addr of GBA gba (from 1) after the intro\n"
	        "  -S prefix      after the intro, write each GBA's picture to prefix-gbaN.png\n"
	        "  -B             corrupt one byte of RAM after every state load, to check that\n"
	        "                 GekkoNet reports the desync (a negative control)\n",
	        arg0);
}

static bool _writeTrace(const char* path, const struct mNetplaySession* s, int frames) {
	FILE* f = fopen(path, "w");
	if (!f) {
		return false;
	}
	int i;
	for (i = 0; i <= frames && i < s->traceLength; ++i) {
		fprintf(f, "%d %08x", i, s->traceChecksum[i]);
		int p;
		for (p = 0; p < s->profile->players; ++p) {
			fprintf(f, " %03x", s->traceKeys[i * NETPLAY_MAX_PLAYERS + p]);
		}
		fputc('\n', f);
	}
	fclose(f);
	return true;
}

static int _countDesyncs(GekkoSession* gekko, bool verbose, int player) {
	int count = 0;
	int desyncs = 0;
	GekkoSessionEvent** events = gekko_session_events(gekko, &count);
	int i;
	for (i = 0; i < count; ++i) {
		GekkoSessionEvent* ev = events[i];
		switch (ev->type) {
		case GekkoDesyncDetected:
			++desyncs;
			fprintf(stderr, "[P%d] DESYNC at frame %d: local %08x remote %08x (player %d)\n", player, ev->data.desynced.frame,
			        ev->data.desynced.local_checksum, ev->data.desynced.remote_checksum, ev->data.desynced.remote_handle + 1);
			break;
		case GekkoPlayerConnected:
			if (verbose) {
				fprintf(stderr, "[P%d] player %d connected\n", player, ev->data.connected.handle + 1);
			}
			break;
		case GekkoPlayerDisconnected:
			fprintf(stderr, "[P%d] player %d disconnected\n", player, ev->data.disconnected.handle + 1);
			break;
		case GekkoSessionStarted:
			if (verbose) {
				fprintf(stderr, "[P%d] session started\n", player);
			}
			break;
		default:
			break;
		}
	}
	return desyncs;
}

/* Handles GekkoNet's events one at a time; with -B, a byte of GBA 1's work RAM is changed
 * after each load, which a working desync check must report. */
static void _handle(const struct Options* o, struct mNetplaySession* s, GekkoGameEvent** events, int count) {
	if (!o->breakDeterminism) {
		mNetplaySessionHandleEvents(s, events, count);
		return;
	}
	int i;
	for (i = 0; i < count; ++i) {
		mNetplaySessionHandleEvents(s, &events[i], 1);
		if (events[i]->type == GekkoLoadEvent) {
			struct mCore* core = GBALinkMachineCore(s->machine, 0);
			uint32_t addr = 0x02000000 + (s->stats.rollbacks * 4099) % 0x40000;
			core->busWrite8(core, addr, core->busRead8(core, addr) ^ 0x5A);
		}
	}
}

static void _summary(const struct Options* o, const struct mNetplayProfile* profile, struct GBALinkMachine* m,
                     const struct mNetplaySession* s, int introFrames, double introSec, double sessionSec, double simMs, int desyncs) {
	struct GBALinkMachineStats st;
	GBALinkMachineGetStats(m, &st);
	int frames = s->stats.frame + 1;
	printf("player %d mode %s profile %s: intro %d frames in %.1fs; session %d frames in %.1fs, rollbacks %d, "
	       "re-simulated frames %d, desyncs %d, avg frame+rollback cost %.2f ms, link transfers %" PRIu32
	       " (rejected %" PRIu32 ", late %" PRIu32 "), final checksum %08x\n",
	       o->player, o->mode, profile->name, introFrames, introSec, frames, sessionSec, s->stats.rollbacks,
	       s->stats.resimFrames, desyncs, frames ? simMs / frames : 0.0, st.transfers, st.rejected, st.late, s->checksum);
}

static int _runSynctest(const struct Options* o, const struct mNetplayProfile* profile, struct GBALinkMachine* m, int introFrames, double introSec) {
	GekkoSession* gekko = NULL;
	gekko_create(&gekko, GekkoStressSession);
	GekkoConfig cfg = {
		.num_players = (unsigned char) profile->players,
		.max_spectators = 0,
		.max_input_prediction_window = 8,
		.input_size = NETPLAY_INPUT_SIZE,
		.state_size = (unsigned) GBALinkMachineStateSize(m),
		.desync_detection = true,
		.check_distance = (unsigned) o->check,
	};
	gekko_start(gekko, &cfg);
	int p;
	for (p = 0; p < profile->players; ++p) {
		gekko_add_actor(gekko, GekkoLocalPlayer, NULL);
	}
	struct mNetplaySession s;
	mNetplaySessionInit(&s, m, profile, gekko);
	if (o->trace) {
		mNetplaySessionEnableTrace(&s, o->frames + 16);
	}
	int desyncs = 0;
	int n = 0;
	double simMs = 0;
	int64_t t0 = _nowNs();
	while (s.stats.frame + 1 < o->frames) {
		for (p = 0; p < profile->players; ++p) {
			uint8_t in[NETPLAY_INPUT_SIZE];
			mNetplayEncodeKeys(_bot(o->seed, p + 1, n), in);
			gekko_add_local_input(gekko, p, in);
		}
		++n;
		int64_t t = _nowNs();
		int count = 0;
		GekkoGameEvent** events = gekko_update_session(gekko, &count);
		_handle(o, &s, events, count);
		simMs += (_nowNs() - t) / 1e6;
		desyncs += _countDesyncs(gekko, false, 1);
	}
	double sessionSec = (_nowNs() - t0) / 1e9;
	_summary(o, profile, m, &s, introFrames, introSec, sessionSec, simMs, desyncs);
	if (o->trace && !_writeTrace(o->trace, &s, s.stats.frame)) {
		fprintf(stderr, "cannot write %s\n", o->trace);
	}
	mNetplaySessionDeinit(&s);
	gekko_destroy(&gekko);
	return desyncs ? 2 : 0;
}

static int _runPeer(const struct Options* o, const struct mNetplayProfile* profile, struct GBALinkMachine* m, int introFrames, double introSec) {
	if (o->player < 1 || o->player > profile->players) {
		fprintf(stderr, "player must be 1 to %d\n", profile->players);
		return 1;
	}
	char addrs[NETPLAY_MAX_PLAYERS][64] = { { 0 } };
	if (o->peers) {
		char list[512];
		strlcpy(list, o->peers, sizeof(list));
		char* item = list;
		while (item && *item) {
			char* comma = strchr(item, ',');
			if (comma) {
				*comma = '\0';
			}
			char* eq = strchr(item, '=');
			int num = eq ? atoi(item) : 0;
			if (!eq || num < 1 || num > NETPLAY_MAX_PLAYERS) {
				fprintf(stderr, "bad peer entry %s\n", item);
				return 1;
			}
			strlcpy(addrs[num - 1], eq + 1, sizeof(addrs[num - 1]));
			item = comma ? comma + 1 : NULL;
		}
	}
	char err[256];
	GekkoNetAdapter* adapter = mNetplayUDPAdapterCreate(o->port, o->latencyMs, o->jitterMs, o->loss, o->seed + o->player, err, sizeof(err));
	if (!adapter) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	GekkoSession* gekko = NULL;
	gekko_create(&gekko, GekkoGameSession);
	GekkoConfig cfg = {
		.num_players = (unsigned char) profile->players,
		.max_spectators = 0,
		.max_input_prediction_window = 8,
		.input_size = NETPLAY_INPUT_SIZE,
		.state_size = (unsigned) GBALinkMachineStateSize(m),
		.desync_detection = true,
	};
	gekko_start(gekko, &cfg);
	gekko_net_adapter_set(gekko, adapter);
	gekko_set_disconnect_timeout(gekko, 10000);
	int local = -1;
	int p;
	for (p = 0; p < profile->players; ++p) {
		if (p + 1 == o->player) {
			local = gekko_add_actor(gekko, GekkoLocalPlayer, NULL);
			gekko_set_local_delay(gekko, local, (unsigned char) o->delay);
		} else {
			if (!addrs[p][0]) {
				fprintf(stderr, "no address for player %d (use -r)\n", p + 1);
				return 1;
			}
			GekkoNetAddress addr = { addrs[p], (unsigned) strlen(addrs[p]) };
			gekko_add_actor(gekko, GekkoRemotePlayer, &addr);
		}
	}
	struct mNetplaySession s;
	mNetplaySessionInit(&s, m, profile, gekko);
	if (o->trace) {
		mNetplaySessionEnableTrace(&s, o->frames + 64);
	}
	int desyncs = 0;
	int n = 0;
	double simMs = 0;
	int64_t t0 = _nowNs();
	int64_t next = t0;
	while (s.stats.frame + 1 < o->frames) {
		gekko_network_poll(gekko);
		desyncs += _countDesyncs(gekko, false, o->player);
		int64_t now = _nowNs();
		if (now < next) {
			_sleepNs(next - now);
		}
		/* a peer ahead of the others waits a little longer each frame (GekkoNet's time sync) */
		float ahead = gekko_frames_ahead(gekko);
		next += ahead >= 1.0f ? FRAME_NS + FRAME_NS / 50 : FRAME_NS;
		if (_nowNs() - next > 4 * FRAME_NS) {
			next = _nowNs();   /* do not try to catch up after a stall */
		}
		uint8_t in[NETPLAY_INPUT_SIZE];
		mNetplayEncodeKeys(_bot(o->seed, o->player, n), in);
		gekko_add_local_input(gekko, local, in);
		++n;
		int64_t t = _nowNs();
		int count = 0;
		GekkoGameEvent** events = gekko_update_session(gekko, &count);
		_handle(o, &s, events, count);
		simMs += (_nowNs() - t) / 1e6;
	}
	/* keep serving the network briefly so the other peers can confirm the last frames */
	int64_t end = _nowNs() + 500000000;
	while (_nowNs() < end) {
		gekko_network_poll(gekko);
		desyncs += _countDesyncs(gekko, false, o->player);
		_sleepNs(5000000);
	}
	double sessionSec = (_nowNs() - t0) / 1e9;
	_summary(o, profile, m, &s, introFrames, introSec, sessionSec, simMs, desyncs);
	if (o->trace && !_writeTrace(o->trace, &s, s.stats.frame)) {
		fprintf(stderr, "cannot write %s\n", o->trace);
	}
	mNetplaySessionDeinit(&s);
	gekko_destroy(&gekko);
	mNetplayUDPAdapterDestroy();
	return desyncs ? 2 : 0;
}

static double _benchFrames(struct GBALinkMachine* m, const struct mNetplayProfile* profile, int frames, uint32_t seed, int* n) {
	int64_t t = _nowNs();
	int i;
	for (i = 0; i < frames; ++i) {
		uint16_t keys[NETPLAY_MAX_PLAYERS];
		int p;
		for (p = 0; p < profile->players; ++p) {
			keys[p] = _bot(seed, p + 1, *n);
		}
		++*n;
		mNetplayRunFrame(m, profile, keys);
	}
	return (_nowNs() - t) / 1e6 / frames;
}

static int _runBench(const struct Options* o, const struct mNetplayProfile* profile, struct GBALinkMachine* m) {
	int n = 0;
	int all = (1 << GBALinkMachineGBAs(m)) - 1;
	double drawn = _benchFrames(m, profile, o->benchFrames, o->seed, &n);
	GBALinkMachineSetDrawMask(m, 1);
	double own = _benchFrames(m, profile, o->benchFrames, o->seed, &n);
	GBALinkMachineSetDrawMask(m, all);
	GBALinkMachineSetRender(m, false);
	double undrawn = _benchFrames(m, profile, o->benchFrames, o->seed, &n);
	GBALinkMachineSetRender(m, true);
	size_t size = GBALinkMachineStateSize(m);
	void* state = malloc(size);
	int i;
	int64_t t = _nowNs();
	for (i = 0; i < o->benchFrames; ++i) {
		GBALinkMachineSaveState(m, state);
	}
	double save = (_nowNs() - t) / 1e6 / o->benchFrames;
	t = _nowNs();
	for (i = 0; i < o->benchFrames; ++i) {
		GBALinkMachineLoadState(m, state);
	}
	double load = (_nowNs() - t) / 1e6 / o->benchFrames;
	t = _nowNs();
	uint32_t sum = 0;
	for (i = 0; i < o->benchFrames; ++i) {
		sum = GBALinkMachineChecksum(m);
	}
	double checksum = (_nowNs() - t) / 1e6 / o->benchFrames;
	free(state);
	printf("bench profile %s gbas %d: frame %.2f ms, own GBA drawn %.2f ms, undrawn %.2f ms, save %.3f ms, load %.3f ms, "
	       "checksum %.3f ms (%08x), state %zu bytes\n",
	       profile->name, GBALinkMachineGBAs(m), drawn, own, undrawn, save, load, checksum, sum, size);
	return 0;
}

int main(int argc, char** argv) {
	struct Options o = {
		.mode = "synctest",
		.frames = 1800,
		.seed = 1,
		.delay = 1,
		.check = 7,
		.player = 1,
		.port = 7001,
		.benchFrames = 600,
	};
	int c;
	while ((c = getopt(argc, argv, "m:p:f:s:c:P:o:r:d:L:J:X:t:b:w:S:Bh")) != -1) {
		switch (c) {
		case 'm': o.mode = optarg; break;
		case 'p': o.profile = optarg; break;
		case 'f': o.frames = atoi(optarg); break;
		case 's': o.seed = (uint32_t) strtoul(optarg, NULL, 0); break;
		case 'c': o.check = atoi(optarg); break;
		case 'P': o.player = atoi(optarg); break;
		case 'o': o.port = atoi(optarg); break;
		case 'r': o.peers = optarg; break;
		case 'd': o.delay = atoi(optarg); break;
		case 'L': o.latencyMs = atoi(optarg); break;
		case 'J': o.jitterMs = atoi(optarg); break;
		case 'X': o.loss = atof(optarg); break;
		case 't': o.trace = optarg; break;
		case 'b': o.benchFrames = atoi(optarg); break;
		case 'B': o.breakDeterminism = true; break;
		case 'S': o.shots = optarg; break;
		case 'w':
			if (o.watches < 16 && sscanf(optarg, "%d:%" SCNi32 ":%d", &o.watchGba[o.watches], &o.watchAddr[o.watches], &o.watchCount[o.watches]) == 3) {
				++o.watches;
			}
			break;
		default:
			_usage(argv[0]);
			return 1;
		}
	}
	if (!o.profile) {
		_usage(argv[0]);
		return 1;
	}
	mLogSetDefaultLogger(&_logger);
	static struct mNetplayProfile profile;
	char err[512];
	if (!mNetplayProfileLoad(&profile, o.profile, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (strcmp(o.mode, "identity") == 0) {
		char id[41];
		if (!mNetplayProfileIdentity(&profile, id, err, sizeof(err))) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		printf("%s\n", id);
		return 0;
	}
	struct GBALinkMachine* m = mNetplayProfileOpen(&profile, err, sizeof(err));
	if (!m) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	int introFrames = mNetplayProfileIntroFrames(&profile);
	int64_t t0 = _nowNs();
	int f;
	for (f = 0; f < introFrames; ++f) {
		uint16_t keys[NETPLAY_MAX_PLAYERS];
		mNetplayProfileIntroKeys(&profile, f, keys);
		mNetplayRunFrame(m, &profile, keys);
	}
	double introSec = (_nowNs() - t0) / 1e9;
	int w;
	for (w = 0; w < o.watches; ++w) {
		int gba = o.watchGba[w] - 1;
		if (gba < 0 || gba >= GBALinkMachineGBAs(m)) {
			continue;
		}
		printf("after intro: GBA %d %08" PRIX32 ":", gba + 1, o.watchAddr[w]);
		int i;
		for (i = 0; i < o.watchCount[w]; ++i) {
			printf(" %04X", GBALinkMachineRead16(m, gba, o.watchAddr[w] + i * 2));
		}
		printf("\n");
	}
	if (o.shots) {
		/* the pictures at the end of the intro, to check where a profile's intro arrives */
		int gba;
		for (gba = 0; gba < GBALinkMachineGBAs(m); ++gba) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s-gba%d.png", o.shots, gba + 1);
			struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
			if (!vf || !mCoreTakeScreenshotVF(GBALinkMachineCore(m, gba), vf)) {
				fprintf(stderr, "%s: could not write the picture\n", path);
			}
			if (vf) {
				vf->close(vf);
			}
		}
	}
	int ret;
	if (strcmp(o.mode, "synctest") == 0) {
		ret = _runSynctest(&o, &profile, m, introFrames, introSec);
	} else if (strcmp(o.mode, "peer") == 0) {
		ret = _runPeer(&o, &profile, m, introFrames, introSec);
	} else if (strcmp(o.mode, "bench") == 0) {
		ret = _runBench(&o, &profile, m);
	} else {
		_usage(argv[0]);
		ret = 1;
	}
	GBALinkMachineClose(m);
	return ret;
}
