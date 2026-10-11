/* Copyright (c) 2013-2019 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/renderers/common.h>

#include <mgba/gba/interface.h>

int GBAVideoRendererCleanOAM(struct GBAObj* oam, struct GBAVideoRendererSprite* sprites, int offsetY) {
	return GBAVideoRendererCleanOAMMargins(oam, sprites, offsetY, NULL);
}

int GBAVideoRendererCleanOAMMargins(struct GBAObj* oam, struct GBAVideoRendererSprite* sprites, int offsetY, const struct GBAWidescreen* margins) {
	bool wide = margins && GBAWidescreenIsActive(margins);
	int i;
	int oamMax = 0;
	for (i = 0; i < 128; ++i) {
		struct GBAObj obj;
		LOAD_16LE(obj.a, 0, &oam[i].a);
		LOAD_16LE(obj.b, 0, &oam[i].b);
		LOAD_16LE(obj.c, 0, &oam[i].c);
		if (GBAObjAttributesAIsTransformed(obj.a) || !GBAObjAttributesAIsDisable(obj.a)) {
			int width = GBAVideoObjSizes[GBAObjAttributesAGetShape(obj.a) * 4 + GBAObjAttributesBGetSize(obj.b)][0];
			int height = GBAVideoObjSizes[GBAObjAttributesAGetShape(obj.a) * 4 + GBAObjAttributesBGetSize(obj.b)][1];
			int32_t x = (uint32_t) GBAObjAttributesBGetX(obj.b) << 23;
			x >>= 23;
			int cycles;
			// widescreen: sprites off the GBA's screen are kept when the margins show them
			bool offscreen = false;
			bool offscreenX = false;
			if (GBAObjAttributesAIsTransformed(obj.a)) {
				height <<= GBAObjAttributesAGetDoubleSize(obj.a);
				width <<= GBAObjAttributesAGetDoubleSize(obj.a);
				cycles = 8 + width * 2;
				if (x < 0) {
					cycles += x;
				}
			} else {
				cycles = width - 2;
				if (x < 0) {
					if (x + width < 0) {
						if (!wide) {
							continue;
						}
						offscreen = true;
						offscreenX = true;
					}
					cycles += x >> 1;
				}
			}
			if (GBAObjAttributesAGetY(obj.a) >= GBA_VIDEO_VERTICAL_PIXELS && GBAObjAttributesAGetY(obj.a) + height < VIDEO_VERTICAL_TOTAL_PIXELS) {
				if (!wide) {
					continue;
				}
				offscreen = true;
			}
			if (GBAObjAttributesBGetX(obj.b) >= GBA_VIDEO_HORIZONTAL_PIXELS && GBAObjAttributesBGetX(obj.b) + width < 512) {
				if (!wide) {
					continue;
				}
				offscreen = true;
				offscreenX = true;
			}
			int y = GBAObjAttributesAGetY(obj.a) + offsetY;
			if (y + height > 256) {
				y -= 256;
			} else if (wide && margins->top && y >= GBA_VIDEO_VERTICAL_PIXELS + margins->bottom && y + height > 256 - margins->top) {
				// below the bottom margin: wraps around into the top margin, as Y wraps at 256
				y -= 256;
			}
			if (offscreen) {
				int screenX = GBAVideoRendererSpriteX(GBAObjAttributesBGetX(obj.b), width, margins);
				if (screenX >= GBA_VIDEO_HORIZONTAL_PIXELS + margins->right || screenX + width <= -margins->left) {
					continue;
				}
				if (y >= GBA_VIDEO_VERTICAL_PIXELS + margins->bottom || y + height <= -margins->top) {
					continue;
				}
			}
			sprites[oamMax].y = y;
			sprites[oamMax].endY = y + height;
			sprites[oamMax].cycles = cycles;
			sprites[oamMax].obj = obj;
			sprites[oamMax].index = i;
			// the GBA does not fetch a sprite that is off its screen sideways, so it takes no
			// OBJ cycles; on the GBA's lines this keeps the sprites drawn exactly as before
			sprites[oamMax].marginOnly = offscreenX;
			++oamMax;
		}
	}
	return oamMax;
}
