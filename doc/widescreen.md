# Widescreen for ROM hacks

This fork of mGBA can show a picture larger than the Game Boy Advance's 240×160 screen, for ROM hacks that ask for it. The emulator draws *margins* around the GBA's picture: a band to the left, to the right, above and below, drawn from the same video state as the picture itself (the same background maps, tiles, sprites, palettes and video registers). A hack fills the margins the way it fills the screen: it writes more of its background maps and places sprites farther out.

Only the picture grows. The emulated GBA, including its CPU, memory, video timing and interrupts, stays as it is on the hardware. A ROM that does not ask for widescreen gets the same picture as in upstream mGBA, and in a hack that does, the 240×160 part of the picture is the same with widescreen on and off.

## Contents

| Path | What it is |
|---|---|
| `include/mgba/internal/gba/widescreen.h`, `src/gba/widescreen.c` | The declaration that a ROM carries, its limits, and the search for it in a ROM |
| `src/gba/renderers/video-software.c`, `src/gba/renderers/software-*.c`, `src/gba/renderers/common.c` | The software renderer, which draws the margins |
| `src/gba/core.c`, `include/mgba/gba/core.h` | The `gba.widescreen` option, the picture's size, and `GBACoreAllowWidescreen()`, with which a frontend accepts larger pictures |
| `src/gba/io.c` | Registers from which a game reads whether it has margins |
| `src/platform/qt/` | The setting in mGBA-qt, and picture buffers that follow the picture's size |
| `tools/widescreen/testrom/` | `wstest.gba`, a test program that covers every kind of layer in the margins, with its source and build script |

## Design

There are two ways to get a wider picture from a GBA game. One is to make the emulated GBA's screen larger. This fork once did that with a "Widescreen Test" build, which set the GBA's screen to 320×200. The screen size also sets the GBA's video timing, though: the vertical blank (the pause between frames, in which games do most of their work) started at line 200 instead of 160 and lasted 28 lines instead of 68, and interrupts and DMA that depend on the line moved with it. Every game then ran with timing that no GBA has, whether it used the extra space or not. Bitmap modes 3 and 4 changed their layout in video memory too, to rows of 320 pixels, so that mode 4's two pages overlapped and mode 3 reached past the end of video memory.

The other way, used here, keeps the GBA as it is and has the renderer draw more. The GBA still draws its 160 lines of 240 pixels, at the hardware's times. For each line, the renderer also draws the parts of that line that lie in the side margins, from the same state. The lines above and below the GBA's picture do not exist on a GBA, so the renderer draws them along with the GBA's first and last lines, from the state of those lines. Nothing that a game can observe changes, apart from registers that a game reads on purpose to learn about its margins.

Drawing the margins this way makes them a *continuation* of the GBA's picture: a background continues with the same scroll, a sprite at a position just off the screen appears in the margin there, and a window that reaches the screen's edge continues to the picture's edge. The section [What the margins show](#what-the-margins-show) gives the rules.

## Declaring widescreen in a ROM

A ROM asks for widescreen with a *declaration*: 20 bytes anywhere in the ROM, at an offset that is a multiple of 4.

| Offset | Size | Value |
|---|---|---|
| 0x00 | 8 bytes | `MGBAWIDE` (ASCII) |
| 0x08 | 16 bits | Version: 1 |
| 0x0A | 16 bits | Left margin in pixels, a multiple of 8 |
| 0x0C | 16 bits | Right margin in pixels, a multiple of 8 |
| 0x0E | 16 bits | Top margin in lines |
| 0x10 | 16 bits | Bottom margin in lines |
| 0x12 | 16 bits | Flags: 0 (reserved) |

Values are little-endian, as everything on the GBA. The whole picture, margins included, can be at most 512 pixels wide and 256 lines tall, which are the ranges of sprite coordinates. mGBA searches the ROM from the start and uses the first valid declaration; one with another version, flags other than 0, or margins outside the limits is ignored. The search runs after mGBA applies a patch (IPS, UPS or BPS), so a patch can add the declaration to a hack.

In GNU assembler syntax, a declaration for a 320×180 picture (16:9) is:

```asm
    .balign 4
    .ascii "MGBAWIDE"
    .hword 1          @ version
    .hword 40, 40     @ left, right
    .hword 10, 10     @ top, bottom
    .hword 0          @ flags
```

Some sizes:

| Picture | Left | Right | Top | Bottom | |
|---|---|---|---|---|---|
| 320×180 | 40 | 40 | 10 | 10 | 16:9 |
| 288×160 | 24 | 24 | 0 | 0 | About 16:9 (1.8:1), the GBA's height |
| 320×200 | 40 | 40 | 20 | 20 | The GBA's picture in the middle |
| 320×200 | 0 | 80 | 0 | 40 | The picture of the earlier Widescreen Test build, with the GBA's picture at the top left |

## Turning widescreen on

Two things must allow widescreen before a declaration takes effect:

- **The frontend.** A frontend must size its picture from the core after loading the ROM, and follow changes of size. It says that it does with `GBACoreAllowWidescreen(core, true)` after creating the core. mGBA-qt and the netplay machine do; other frontends (SDL, libretro and the console ports) do not, and show the GBA's 240×160 in every case.
- **The option.** The configuration option `gba.widescreen` turns the feature on (1) or off (0); in the core it is off unless a frontend or the user sets it. mGBA-qt turns it on by default, under Settings, Emulation, "Enable widescreen in ROM hacks that declare it", and a change takes effect at the next frame. The netplay machine turns it on for every GBA ([netplay.md](netplay.md)), so that all players see the same margins: the margins depend only on the ROM, which a session's identity covers.

Margins need the software renderer. With mGBA-qt's OpenGL renderer (Settings, Enhancements, "Video renderer"), the picture stays 240×160.

## What the margins show

Positions below are GBA screen positions: x from 0 to 239 and y from 0 to 159 are the GBA's screen; a left margin of 40 pixels covers x from −40 to −1, a bottom margin of 20 lines covers y from 160 to 179.

**Text backgrounds** continue with the same scroll: the pixel at (x, y) shows the background map's pixel at (x + HOFS, y + VOFS), wrapping at the map's width and height. A map 256 pixels wide repeats within a picture wider than 256 pixels, so a hack that needs distinct content across a wide picture uses maps 512 pixels wide (64×32 or 64×64 tiles) and writes the map columns that the margins show.

**Affine backgrounds** continue their transformation: the pixel at (x, y) shows the map at the reference point plus PA·x + PB·y horizontally and PC·x + PD·y vertically, and outside the map either wraps around or shows nothing, as the background's wraparound bit says. **Bitmap modes** (3, 4 and 5) are affine backgrounds too, which end at the bitmap's edges: unless a game moves or scales the bitmap, the margins show what lies behind it, the backdrop and sprites. A bitmap keeps the GBA's layout in video memory, 240 pixels per row in modes 3 and 4 and 160 in mode 5, so a hack fills the margins with bitmap pixels only by scaling. With margins of 40 pixels on each side, for example, PA = 192 (0.75) and BG2X = 30 pixels spread the 240 pixels of a row over the picture's 320, at three quarters of the horizontal resolution.

**Sprites** off the GBA's screen appear in the margins:

| Attribute | In the right or bottom margin | In the left or top margin |
|---|---|---|
| X (9 bits) | 240 to 239 + right margin | 512 − left margin to 511, that is x − 512 |
| Y (8 bits) | 160 to 159 + bottom margin | 256 − top margin to 255, that is y − 256 |

As on the GBA, a sprite whose X plus width passes 512 wraps around to the left side, and one whose Y plus height passes 256 wraps around to the top. Sprites that the GBA does not fetch, because they are off its screen to the side, still use none of a line's sprite drawing time (OBJ cycles) when they appear in a margin, so the sprites that a busy line drops on the GBA's screen are the same with and without margins.

The usual way to hide a sprite, moving it just off the screen (to Y 160 or X 240), now shows it in a margin. A hack hides sprites with the disable bit instead (attribute 0, bit 9, for sprites without rotation and scaling), or moves them beyond the margins.

**Windows** that reach the left or right edge of the GBA's screen continue to the edge of the picture, including windows that wrap around from right to left. The lines above the picture take the windows' state from line 0, and the lines below from line 159.

**Mosaic** keeps the GBA's grid: the blocks line up with x = 0 and y = 0 of the GBA's screen and continue into the margins.

**Blending, priorities and the backdrop** work as on the GBA's screen, and forced blank whitens the whole picture.

**Lines above and below.** The GBA has no lines above or below its 160, so nothing happens on them: no horizontal blank, no VCOUNT match, no HBlank DMA. The margin lines above the picture are drawn with line 0, from line 0's state; the margin lines below are drawn after line 159, from line 159's state. A game that changes video registers during the frame, for example the scroll for a parallax effect or the affine parameters for a perspective floor, gets line 0's values in every line of the top margin and line 159's in every line of the bottom margin. Affine backgrounds step on through the margin lines by PB and PD per line, as they would on lines of their own. The side margins of each GBA line use that line's own values, so such effects continue sideways.

## Reading the margins from a game

A game finds out whether it has margins, and how large they are, by reading these registers:

| Address | Value |
|---|---|
| 0x04FFF790 | 0x5357 ("WS") in an mGBA build with this feature |
| 0x04FFF792 | Left margin in use, in pixels |
| 0x04FFF794 | Right margin in use |
| 0x04FFF796 | Top margin in use, in lines |
| 0x04FFF798 | Bottom margin in use |

The margins read 0 when widescreen is off, whether the user turned it off, the frontend does not allow it, or the OpenGL renderer draws. They can change while a game runs, when the user changes the setting, so a game reads them every frame, or at least whenever it lays out a scene. The registers are read with 16-bit or 32-bit loads; 8-bit loads return unrelated values, and writes are ignored. On a GBA or in another emulator these addresses return unrelated values, so a game also checks that the margins it reads are those it declared.

## Writing a hack for widescreen

1. Declare the margins.
2. Read the registers, and use the margins only when they read as declared, so that the hack still works on a GBA and with widescreen off.
3. Use backgrounds 512 pixels wide where the picture is wider than 256 pixels, and stream map columns as far as the margins reach on each side of the camera.
4. Keep objects active, and keep their sprites, out to the edges of the margins instead of the GBA's screen; hide unused sprites with the disable bit.
5. Lay out the heads-up display for the larger picture when it is there; with widescreen off, the GBA's 240×160 must still show everything that the game needs.
6. Expect mid-frame effects in the top and bottom margins to repeat line 0 and line 159.

The test program is a starting point: its source shows a declaration, the register check and sprites in each margin.

## The test program

`tools/widescreen/testrom/wstest.gba` declares margins of 40 pixels left and right and 20 lines above and below, a 320×200 picture, and shows eight scenes of 128 frames each, in a loop:

| Scene | What it shows |
|---|---|
| 0 | Two text backgrounds with scroll and alpha blending, and sprites inside, across and beyond each margin, some of them flipped |
| 1 | An affine background in mode 1, rotated, whose map ends inside the margins, and rotated double-size sprites |
| 2 | Bitmap mode 3 with sprites in the margins |
| 3 | Bitmap mode 5, scaled to the screen, with mosaic on the bitmap and on sprites |
| 4 | Windows: one that wraps around both edges in both directions, one that reaches the right edge, and an OBJ window |
| 5 | Mosaic on a text background and on sprites, under a grid without mosaic |
| 6 | More 64×64 sprites on one line than the GBA can draw, with sprites in the side margins |
| 7 | The registers: green when they read 0x5357 and the declared margins, blue when they read 0x5357 and no margins, red or yellow otherwise |

Scenes 0 to 6 read none of the registers, so their 240×160 part is the same with widescreen on and off. `tools/widescreen/testrom/build.sh` builds the ROM with the GNU assembler and linker for `arm-none-eabi`. The ROM has no Nintendo logo, so it runs in mGBA but not on a GBA.

## Results

These checks ran on Linux with GCC 13:

- **Without margins, nothing changes.** mGBA's renderer regression tests (the 26 GBA tests of the CInema suite) pass, as on the build without the feature. With the ROMs of *Mario Kart: Super Circuit* and *Super Street Fighter II Turbo Revival*, 20,000 frames of random keys gave the same picture in every frame as the build without the feature, as did the test program, apart from scene 7.
- **The GBA's part of the picture does not change with margins.** The same two games, with a declaration of 40/40/20/20 added to a copy of each ROM, gave the same 240×160 part in all 20,000 frames with widescreen on and off, over about 9,000 different pictures per game.
- **The margins show what the rules above say.** For scenes 0 to 5 of the test program, a separate model of these rules computed what the margins should show, and mGBA's pictures matched it in every pixel, apart from blended pixels, which matched within the rounding of the blend.
- **Changes while running.** Turning the option off and on while the test program runs gave, from the first frame after each change, the same pictures as runs started with that setting; mGBA-qt resized its window each time.
- **Netplay.** A two-GBA session with the test program on one GBA and the link test program on the other ran a 1,500-frame GekkoNet stress session without desyncs, with pictures of 320×200 and 240×160.
- **Speed.** With a 320×200 picture, the emulator took 6% more CPU time than with 240×160 in *Super Street Fighter II Turbo Revival* and 19% more in *Mario Kart: Super Circuit*; without a declaration, it took within 1.2% of the time of the build without the feature, which is within the variation between runs.

## Limitations

- **The software renderer only.** The OpenGL renderer draws the GBA's 240×160.
- **No timing of their own in the top and bottom margins.** Effects that change registers between lines show line 0's or line 159's state there.
- **Bitmaps no wider than the GBA's.** Modes 3 to 5 keep the GBA's rows of 240 (or 160) pixels; a wider picture shows them scaled or with margins around them.
- **One margin per sprite vertically.** A sprite tall enough to reach both the bottom margin and, wrapping around, the top margin shows only in the top margin. Only double-size affine sprites near the bottom of the screen are that tall.
- **Frontends.** Only frontends that call `GBACoreAllowWidescreen()` show margins: mGBA-qt and the netplay machine.
- **Video logs.** mGBA's video logs (`.mvl` files) hold no ROM, so a log of a widescreen hack plays back at 240×160.

## Sources

- GBATEK, the reference for the GBA's video hardware (backgrounds, sprites, windows, mosaic, OBJ cycles): https://problemkaputt.de/gbatek.htm
- mGBA (`src/gba/renderers/`, `src/gba/core.c`, the CInema tests in `cinema/gba/`): https://github.com/mgba-emu/mgba
