/* Copyright (c) 2013-2019 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_RENDERER_COMMON_H
#define GBA_RENDERER_COMMON_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/video.h>
#include <mgba/internal/gba/widescreen.h>

struct GBAVideoRendererSprite {
	struct GBAObj obj;
	int16_t y;
	int16_t endY;
	int16_t cycles;
	int8_t index;
	// widescreen: drawn only in the margins; off the GBA's screen, so it costs no OBJ cycles
	bool marginOnly;
};

int GBAVideoRendererCleanOAM(struct GBAObj* oam, struct GBAVideoRendererSprite* sprites, int offsetY);
// As above, also keeping the sprites that widescreen margins show (NULL: none)
int GBAVideoRendererCleanOAMMargins(struct GBAObj* oam, struct GBAVideoRendererSprite* sprites, int offsetY, const struct GBAWidescreen* margins);
// A sprite's x on the screen (-256 to 255 on the GBA). Sprites that the GBA's screen does not
// show are placed where the margins show them: right of the screen first, else left.
static inline int GBAVideoRendererSpriteX(int x, int width, const struct GBAWidescreen* margins) {
	if (margins && (margins->left || margins->right) && x >= GBA_VIDEO_HORIZONTAL_PIXELS && x + width < 512) {
		return x < GBA_VIDEO_HORIZONTAL_PIXELS + margins->right ? x : x - 512;
	}
	return x >= 256 ? x - 512 : x;
}

CXX_GUARD_END

#endif