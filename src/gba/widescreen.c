/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/widescreen.h>

#include <mgba/gba/interface.h>

static const char _magic[8] = { 'M', 'G', 'B', 'A', 'W', 'I', 'D', 'E' };

bool GBAWidescreenIsValid(const struct GBAWidescreen* ws) {
	if ((ws->left & 7) || (ws->right & 7)) {
		return false;
	}
	if (ws->left + GBA_VIDEO_HORIZONTAL_PIXELS + ws->right > GBA_WIDESCREEN_MAX_WIDTH) {
		return false;
	}
	if (ws->top + GBA_VIDEO_VERTICAL_PIXELS + ws->bottom > GBA_WIDESCREEN_MAX_HEIGHT) {
		return false;
	}
	return true;
}

static uint16_t _read16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

bool GBAWidescreenFind(const void* rom, size_t size, struct GBAWidescreen* out) {
	memset(out, 0, sizeof(*out));
	if (!rom || size < GBA_WIDESCREEN_DECLARATION_SIZE) {
		return false;
	}
	const uint8_t* bytes = rom;
	size_t offset;
	for (offset = 0; offset + GBA_WIDESCREEN_DECLARATION_SIZE <= size; offset += 4) {
		if (bytes[offset] != 'M' || memcmp(&bytes[offset], _magic, sizeof(_magic)) != 0) {
			continue;
		}
		const uint8_t* d = &bytes[offset];
		if (_read16(&d[0x08]) != 1 || _read16(&d[0x12]) != 0) {
			continue;
		}
		struct GBAWidescreen ws = {
			.left = _read16(&d[0x0A]),
			.right = _read16(&d[0x0C]),
			.top = _read16(&d[0x0E]),
			.bottom = _read16(&d[0x10]),
		};
		if (!GBAWidescreenIsValid(&ws) || !GBAWidescreenIsActive(&ws)) {
			continue;
		}
		*out = ws;
		return true;
	}
	return false;
}
