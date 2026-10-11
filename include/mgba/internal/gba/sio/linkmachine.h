/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LINKMACHINE_H
#define GBA_SIO_LINKMACHINE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/interface.h>

/* A link machine is 1 to 4 GBAs, optionally joined by an emulated link cable, run on one
 * thread in a fixed order so that the same inputs from the same state always produce the
 * same state. A machine savestate covers every GBA, the cable and the transfers on it.
 * It is built for rollback netplay, where every player's computer runs every player's
 * GBA: only the players' keys travel over the network.
 *
 * The cable is the official multiplay cable with GBA 0 at its small plug. Multiplayer
 * mode: GBA 0 is the parent, the others take IDs 1 to 3 in order, and only the parent
 * starts transfers, while every GBA is in multiplayer mode. Normal mode: data shifts one
 * way along the cable; each GBA receives the previous GBA's outgoing bits, and GBA 0
 * receives zero (its SI line is grounded). UART, JOY bus and general-purpose modes are
 * not connected. */

#define GBA_LINK_MACHINE_MAX 4
#define GBA_LINK_MACHINE_FRAME_CYCLES 280896

struct mCore;
struct GBALinkMachine;

struct GBALinkMachineStats {
	uint64_t frame;      /* machine frames run */
	uint32_t transfers;  /* link transfers started */
	uint32_t rejected;   /* transfer starts refused (link busy, or not every GBA ready) */
	uint32_t late;       /* deliveries to a GBA already past the completion time */
};

struct GBALinkMachineConfig {
	int gbas;
	const char* roms[GBA_LINK_MACHINE_MAX];   /* ROM file paths */
	const void* saves[GBA_LINK_MACHINE_MAX];  /* initial save memory, or NULL */
	size_t saveSizes[GBA_LINK_MACHINE_MAX];
	bool linked;
	int64_t rtcEpochMs;                       /* start time of cartridge clocks */
};

/* Diagnostics: one link transfer (not part of the machine state). */
struct GBALinkMachineTransfer {
	uint64_t frame;
	uint32_t start;      /* GBA 0's cycle count at the start */
	uint8_t initiator;
	uint8_t mode;        /* enum GBASIOMode */
	uint16_t reserved;
	uint32_t data[GBA_LINK_MACHINE_MAX];
};

struct GBALinkMachine* GBALinkMachineOpen(const struct GBALinkMachineConfig*, char* err, size_t errlen);
void GBALinkMachineClose(struct GBALinkMachine*);

/* Runs one frame (GBA_LINK_MACHINE_FRAME_CYCLES of every GBA, aligned with the start of
 * GBA 0's vertical blank) with keys[i] held on GBA i (active high, GBA keypad order). */
void GBALinkMachineRunFrame(struct GBALinkMachine*, const uint16_t* keys);

size_t GBALinkMachineStateSize(const struct GBALinkMachine*);
void GBALinkMachineSaveState(struct GBALinkMachine*, void* buffer);
bool GBALinkMachineLoadState(struct GBALinkMachine*, const void* buffer);

/* A 32-bit hash of every GBA's work RAM and the link state: equal on peers in sync. */
uint32_t GBALinkMachineChecksum(struct GBALinkMachine*);

int GBALinkMachineGBAs(const struct GBALinkMachine*);
struct mCore* GBALinkMachineCore(struct GBALinkMachine*, int gba);
/* A GBA's latest picture, rows packed, of the size GBALinkMachineVideoSize() gives: 240x160,
 * or larger for a ROM that declares widescreen margins (doc/widescreen.md). */
const mColor* GBALinkMachineFramebuffer(const struct GBALinkMachine*, int gba);
void GBALinkMachineVideoSize(const struct GBALinkMachine*, int gba, unsigned* width, unsigned* height);
uint16_t GBALinkMachineRead16(struct GBALinkMachine*, int gba, uint32_t address);
void GBALinkMachineWrite16(struct GBALinkMachine*, int gba, uint32_t address, uint16_t value);
void GBALinkMachineGetStats(const struct GBALinkMachine*, struct GBALinkMachineStats*);

/* Drawing has no effect on emulation. A client draws only the GBAs it shows (bit i of
 * mask: GBA i; all at first) and turns drawing off while re-simulating after a rollback.
 * The first frame run after a GBA starts being drawn again is drawn in full. */
void GBALinkMachineSetRender(struct GBALinkMachine*, bool enable);
void GBALinkMachineSetDrawMask(struct GBALinkMachine*, unsigned mask);

void GBALinkMachineTraceEnable(struct GBALinkMachine*, bool enable);
size_t GBALinkMachineTraceRead(struct GBALinkMachine*, struct GBALinkMachineTransfer* out, size_t max);

CXX_GUARD_END

#endif
