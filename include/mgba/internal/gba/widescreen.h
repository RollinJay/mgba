/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_WIDESCREEN_H
#define GBA_WIDESCREEN_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Widescreen for ROMs that ask for it (doc/widescreen.md): the software renderer draws
 * margins around the GBA's 240x160 picture, from the same registers, VRAM and OAM. Only
 * the picture grows; the emulated GBA, its timing included, is unchanged.
 *
 * A ROM asks with a declaration anywhere in the ROM, at a 4-byte-aligned offset:
 *
 *   0x00  "MGBAWIDE"   magic
 *   0x08  u16 version  1
 *   0x0A  u16 left     pixels left of the picture, a multiple of 8
 *   0x0C  u16 right    pixels right of the picture, a multiple of 8
 *   0x0E  u16 top      lines above the picture
 *   0x10  u16 bottom   lines below the picture
 *   0x12  u16 flags    0 (reserved)
 *
 * The whole picture is at most 512x256 pixels, the range of sprite coordinates. */

#define GBA_WIDESCREEN_MAX_WIDTH 512
#define GBA_WIDESCREEN_MAX_HEIGHT 256
#define GBA_WIDESCREEN_DECLARATION_SIZE 0x14

struct GBAWidescreen {
	uint16_t left;
	uint16_t right;
	uint16_t top;
	uint16_t bottom;
};

/* Whether margins fit the limits above (all zero is valid: no widescreen). */
bool GBAWidescreenIsValid(const struct GBAWidescreen*);
static inline bool GBAWidescreenIsActive(const struct GBAWidescreen* ws) {
	return ws->left || ws->right || ws->top || ws->bottom;
}

/* Looks for a valid declaration in a ROM; false (and zero margins) when there is none. */
bool GBAWidescreenFind(const void* rom, size_t size, struct GBAWidescreen* out);

CXX_GUARD_END

#endif
