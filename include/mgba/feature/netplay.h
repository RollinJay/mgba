/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_FEATURE_NETPLAY_H
#define M_FEATURE_NETPLAY_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio/linkmachine.h>

/* Rollback netplay for GBA games, built on a GBA link machine (every player's computer
 * runs every player's GBA and the cable between them) and the GekkoNet rollback library.
 *
 * A profile describes one netplay setup: the GBAs and their ROMs and saves, whether they
 * are linked, where each player's keys go, and optionally scripted keys from power-on. A
 * profile's identity is a hash of everything players must share. A session wraps a
 * GekkoNet session around a machine: it saves, loads and advances the machine when
 * GekkoNet asks. */

#define NETPLAY_MAX_PLAYERS 4
#define NETPLAY_MAX_INTRO 512
#define NETPLAY_INPUT_SIZE 2   /* bytes of input per player per frame: the 10 GBA keys */

struct GekkoSession;
struct GekkoGameEvent;
struct GekkoNetAdapter;

enum mNetplayInputType {
	NETPLAY_INPUT_KEYPAD,    /* the player's keys go to the GBA's keypad */
	NETPLAY_INPUT_MAILBOX,   /* the player's keys go to memory, for a game patched to read them */
};

enum mNetplayScreens {
	NETPLAY_SCREENS_OWN,     /* each player sees their own GBA; the others run undrawn */
	NETPLAY_SCREENS_ALL,     /* each player also sees the other players' GBAs */
};

struct mNetplayPlayer {
	int gba;
	enum mNetplayInputType input;
	uint32_t mailbox;        /* each frame, keys go to mailbox + keysOffset, then marker */
	uint32_t keysOffset;     /* (if non-zero) to mailbox + markerOffset */
	uint32_t markerOffset;
	uint16_t marker;
};

struct mNetplayIntroStep {
	int frames;
	uint16_t keys[NETPLAY_MAX_PLAYERS];
};

struct mNetplayProfile {
	char name[64];
	int gbas;
	char roms[GBA_LINK_MACHINE_MAX][PATH_MAX];
	char saves[GBA_LINK_MACHINE_MAX][PATH_MAX];   /* empty: no save file */
	bool linked;
	int64_t rtcEpochMs;
	enum mNetplayScreens screens;
	int players;
	struct mNetplayPlayer player[NETPLAY_MAX_PLAYERS];
	int introSteps;
	struct mNetplayIntroStep intro[NETPLAY_MAX_INTRO];
};

/* Reads a profile (an INI file; see doc/netplay.md). Paths may use ${VAR} environment
 * variables; relative paths start from the profile's folder. */
bool mNetplayProfileLoad(struct mNetplayProfile*, const char* path, char* err, size_t errlen);
int mNetplayProfileIntroFrames(const struct mNetplayProfile*);
void mNetplayProfileIntroKeys(const struct mNetplayProfile*, int frame, uint16_t* keys);
bool mNetplayParseKeys(const char* text, uint16_t* keys);   /* "UP+A" -> key bits */

/* SHA-1 (hex) over the emulator build, every ROM and save, the link, the clock start and
 * the player mapping. Peers must only be paired when their identities are equal. */
bool mNetplayProfileIdentity(const struct mNetplayProfile*, char identity[41], char* err, size_t errlen);

struct GBALinkMachine* mNetplayProfileOpen(const struct mNetplayProfile*, char* err, size_t errlen);

/* Runs one frame with each player's keys (players[i], active high), through the profile's
 * input mapping. */
void mNetplayRunFrame(struct GBALinkMachine*, const struct mNetplayProfile*, const uint16_t* players);

static inline void mNetplayEncodeKeys(uint16_t keys, uint8_t* out) {
	out[0] = keys;
	out[1] = keys >> 8;
}

static inline uint16_t mNetplayDecodeKeys(const uint8_t* in) {
	return (in[0] | (in[1] << 8)) & 0x3FF;
}

struct mNetplayStats {
	int frame;            /* last frame advanced on the current timeline */
	int rollbacks;        /* state loads requested by GekkoNet */
	int resimFrames;      /* frames re-simulated after loads */
	int desyncs;          /* checksum mismatches reported by GekkoNet */
};

struct mNetplaySession {
	struct GBALinkMachine* machine;
	const struct mNetplayProfile* profile;
	struct GekkoSession* gekko;
	struct mNetplayStats stats;
	uint32_t checksum;           /* checksum after the last advanced frame */
	/* optional per-frame trace of the current timeline: checksum and keys */
	uint32_t* traceChecksum;
	uint16_t* traceKeys;         /* NETPLAY_MAX_PLAYERS per frame */
	int traceLength;
	/* optional: called after every frame advanced; shown is false for frames that
	 * GekkoNet re-simulates or runs ahead, whose pictures and sound are not used */
	void (*frameCallback)(struct mNetplaySession*, bool shown, void* context);
	void* frameContext;
};

/* Sets up a session for a machine (in the state the session starts from) and a GekkoNet
 * session that is already started. */
void mNetplaySessionInit(struct mNetplaySession*, struct GBALinkMachine*, const struct mNetplayProfile*, struct GekkoSession*);

/* Handles the events from gekko_update_session(): saves, loads and advances the machine.
 * Re-simulated frames are not drawn. */
void mNetplaySessionHandleEvents(struct mNetplaySession*, struct GekkoGameEvent** events, int count);
void mNetplaySessionEnableTrace(struct mNetplaySession*, int maxFrames);
void mNetplaySessionDeinit(struct mNetplaySession*);

/* A UDP adapter for GekkoNet (addresses are "a.b.c.d:port" strings), with optional added
 * latency, jitter and loss on outgoing packets for tests. One adapter per process. */
struct GekkoNetAdapter* mNetplayUDPAdapterCreate(int port, int latencyMs, int jitterMs, double loss, uint32_t seed, char* err, size_t errlen);
void mNetplayUDPAdapterDestroy(void);

CXX_GUARD_END

#endif
