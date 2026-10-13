/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_CORE_H
#define GBA_CORE_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct mCore;
struct mCore* GBACoreCreate(void);
/* Widescreen (doc/widescreen.md): lets a GBA core draw the margins that a ROM declares, when
 * the "gba.widescreen" option is on. Only for a frontend that sizes the picture from
 * currentVideoSize() after loading the ROM and follows its changes; off by default. Call it
 * after init(). */
void GBACoreAllowWidescreen(struct mCore* core, bool allow);
#ifndef MINIMAL_CORE
struct mCore* GBAVideoLogPlayerCreate(void);
#endif

CXX_GUARD_END

#endif
