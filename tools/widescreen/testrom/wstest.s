@ wstest.s - widescreen test program for mGBA (doc/widescreen.md).
@
@ An emulator test program: it boots in mGBA but not on hardware (no Nintendo logo).
@ It declares margins of 40 pixels left and right and 20 lines above and below, for a
@ 320x200 picture, and shows eight scenes in turn, 128 frames each (scene n starts at
@ frame 128n; at frame 128n + 64 it has settled):
@   0  text backgrounds with alpha blending, and sprites inside, across and beyond
@      each margin, some flipped
@   1  an affine background (mode 1) and affine double-size sprites
@   2  bitmap mode 3 and sprites
@   3  bitmap mode 5 scaled to the screen, with mosaic
@   4  windows that wrap around or touch the screen edges, and an OBJ window
@   5  mosaic on a text background and on sprites
@   6  the OBJ cycle limit, with sprites in the side margins
@   7  the widescreen registers: the whole picture is green when they read 0x5357 and
@      the declared margins, blue when they read 0x5357 and no margins (widescreen
@      off), yellow when a 32-bit read disagrees with 16-bit reads, red otherwise
@ Scenes 0 to 6 do not read the widescreen registers, so their 240x160 area must be the
@ same with widescreen on and off. Build with build.sh (GNU binutils for arm-none-eabi).
    .syntax unified
    .cpu arm7tdmi
    .arm
    .section .text, "ax"
    .global _start

    .equ IO,              0x04000000
    .equ PALETTE,         0x05000000
    .equ VRAM,            0x06000000
    .equ OBJ_VRAM_BITMAP, 0x06014000    @ sprite tile 512: the first one bitmap modes leave
    .equ OAM,             0x07000000
    .equ WIDESCREEN_REGS, 0x04FFF790
    .equ STATE,           0x03000100    @ +0 frame, +4 scene

    .equ SPRITE16_TILE,   512           @ a 16x16 sprite: tiles 512-515
    .equ GRID_TILE,       516           @ 64 bordered tiles, for sprites up to 64x64: 516-579
    .equ TILES4_SIZE,     3 * 32        @ background tiles (below)
    .equ TILES8_SIZE,     17 * 64

_start:
    b     main                      @ 0x00 (mGBA checks for the 0xEA branch opcode)
    .space 156                      @ 0x04: logo (left empty)
    .ascii "WIDESCREEN  "           @ 0xA0: title
    .ascii "WIDE"                   @ 0xAC: game code
    .ascii "00"                     @ 0xB0: maker code
    .byte 0x96                      @ 0xB2: fixed value
    .byte 0, 0                      @ 0xB3: unit code, device type
    .space 7
    .byte 0, 0                      @ 0xBC: version, complement (unchecked by mGBA)
    .space 2

@ 0xC0: the widescreen declaration. It can be anywhere in the ROM, 4-byte aligned.
widescreen_declaration:
    .ascii "MGBAWIDE"
    .hword 1                        @ version
    .hword 40, 40                   @ left, right: multiples of 8
    .hword 20, 20                   @ top, bottom
    .hword 0                        @ flags

main:
    mov   r0, #IO
    mov   r1, #0x80
    strh  r1, [r0]                  @ DISPCNT: forced blank
    ldr   r1, =0x4317
    add   r2, r0, #0x200
    strh  r1, [r2, #4]              @ WAITCNT: faster ROM access, prefetch
    ldr   r0, =OBJ_VRAM_BITMAP
    ldr   r1, =sprite16
    mov   r2, #128
    bl    copy32                    @ the 16x16 sprite
    ldr   r4, =OBJ_VRAM_BITMAP + (GRID_TILE - SPRITE16_TILE) * 32
    mov   r5, #64
1:  mov   r0, r4
    ldr   r1, =tiles4 + 32          @ the bordered tile
    mov   r2, #32
    bl    copy32                    @ grid tiles for the larger sprites
    add   r4, r4, #32
    subs  r5, r5, #1
    bne   1b
    ldr   r3, =STATE
    mov   r1, #0
    str   r1, [r3]                  @ frame
    mvn   r1, #0
    str   r1, [r3, #4]              @ scene: none yet

main_loop:
    bl    wait_vblank
    ldr   r3, =STATE
    ldr   r1, [r3]
    add   r1, r1, #1
    str   r1, [r3]
    mov   r1, r1, lsr #7
    and   r1, r1, #7                @ scene = frame / 128 % 8
    ldr   r2, [r3, #4]
    cmp   r1, r2
    beq   main_loop
    str   r1, [r3, #4]
    bl    reset_video
    ldr   r3, =STATE
    ldr   r1, [r3, #4]
    ldr   r2, =scenes
    ldr   r2, [r2, r1, lsl #2]
    mov   lr, pc
    bx    r2                        @ returns to the next instruction
    b     main_loop

@ Waits for the start of the next VBlank (line 160). Clobbers r0, r1.
wait_vblank:
    mov   r0, #IO
1:  ldrh  r1, [r0, #6]              @ VCOUNT
    cmp   r1, #160
    beq   1b
2:  ldrh  r1, [r0, #6]
    cmp   r1, #160
    bne   2b
    bx    lr

@ Copies r2 bytes (a multiple of 4) from r1 to r0. Clobbers r0-r3.
copy32:
    ldr   r3, [r1], #4
    str   r3, [r0], #4
    subs  r2, r2, #4
    bgt   copy32
    bx    lr

@ Fills r2 bytes (a multiple of 4) at r0 with the word r1. Clobbers r0, r2.
fill32:
    str   r1, [r0], #4
    subs  r2, r2, #4
    bgt   fill32
    bx    lr

@ Forced blank; video registers back to their defaults; palettes and background tiles
@ reloaded (bitmap modes overwrite the tiles); every sprite hidden. Clobbers r0-r3.
reset_video:
    push  {r4, lr}
    mov   r4, #IO
    mov   r1, #0x80
    strh  r1, [r4]                  @ DISPCNT: forced blank while the scene is set up
    add   r0, r4, #0x08
    mov   r1, #0
    mov   r2, #0x50
    bl    fill32                    @ BG0CNT to BLDY: 0
    mov   r1, #0x100
    strh  r1, [r4, #0x20]           @ BG2PA and BG2PD: identity
    strh  r1, [r4, #0x26]
    strh  r1, [r4, #0x30]           @ BG3PA and BG3PD: identity
    strh  r1, [r4, #0x36]
    ldr   r0, =PALETTE
    ldr   r1, =palette
    mov   r2, #0x200
    bl    copy32                    @ background palette
    ldr   r0, =PALETTE + 0x200
    ldr   r1, =palette
    mov   r2, #0x200
    bl    copy32                    @ sprite palette
    ldr   r0, =VRAM
    ldr   r1, =tiles4
    ldr   r2, =TILES4_SIZE
    bl    copy32                    @ 4bpp tiles in character block 0
    ldr   r0, =VRAM + 0x4000
    ldr   r1, =tiles8
    ldr   r2, =TILES8_SIZE
    bl    copy32                    @ 8bpp tiles in character block 1
    ldr   r0, =OAM
    mov   r1, #0x200                @ attribute 0: hidden; attribute 1: 0
    mov   r2, #0                    @ attribute 2 and the affine parameter: 0
    mov   r3, #128
1:  str   r1, [r0], #4
    str   r2, [r0], #4
    subs  r3, r3, #1
    bne   1b
    pop   {r4, pc}

@ A text background map of r1 (32 or 64) by 32 tiles at r0, of tile r2 in palette
@ (x + 2y) % 16 for tile column x and row y. Clobbers r0-r3.
make_grid_map:
    push  {r4-r7, lr}
    mov   r4, #0                    @ y
1:  mov   r5, #0                    @ x
2:  add   r6, r5, r4, lsl #1
    and   r6, r6, #15
    orr   r6, r2, r6, lsl #12       @ the map entry
    and   r7, r5, #31
    add   r7, r7, r4, lsl #5
    mov   r7, r7, lsl #1            @ offset in its 32x32 block
    tst   r5, #32
    addne r7, r7, #0x800            @ columns 32-63: the second block
    strh  r6, [r0, r7]
    add   r5, r5, #1
    cmp   r5, r1
    blt   2b
    add   r4, r4, #1
    cmp   r4, #32
    blt   1b
    pop   {r4-r7, pc}

@ An affine background map of 32x32 tiles at r0: 8bpp tile 1 + (x + y) % 16 for tile
@ column x and row y. Clobbers r0-r3.
make_affine_map:
    push  {r4-r6, lr}
    mov   r4, #0                    @ y
1:  mov   r5, #0                    @ x, even: VRAM takes 16-bit writes, two entries
2:  add   r6, r5, r4
    and   r6, r6, #15
    add   r6, r6, #1                @ entry x
    add   r3, r5, r4
    add   r3, r3, #1
    and   r3, r3, #15
    add   r3, r3, #1                @ entry x + 1
    orr   r6, r6, r3, lsl #8
    add   r3, r5, r4, lsl #5
    strh  r6, [r0, r3]
    add   r5, r5, #2
    cmp   r5, #32
    blt   2b
    add   r4, r4, #1
    cmp   r4, #32
    blt   1b
    pop   {r4-r6, pc}

@ A 16-bit bitmap of r1 by r2 pixels at r0: color r3 | (x / 8) << r4 | (y / 8) << r5,
@ with a white border. Clobbers r0-r3.
fill_bitmap:
    push  {r4-r11, lr}
    mov   r8, r0                    @ row
    mov   r9, #0                    @ y
1:  mov   r7, r9, lsr #3
    orr   r7, r3, r7, lsl r5        @ the row's color
    mov   r10, #0                   @ x
2:  mov   r6, r10, lsr #3
    orr   r6, r7, r6, lsl r4
    mov   r11, r10, lsl #1
    strh  r6, [r8, r11]
    add   r10, r10, #1
    cmp   r10, r1
    blt   2b
    add   r8, r8, r1, lsl #1
    add   r9, r9, #1
    cmp   r9, r2
    blt   1b
    ldr   r6, =0x7FFF
    sub   r11, r2, #1
    mul   r11, r1, r11
    add   r11, r0, r11, lsl #1      @ the last row
    mov   r10, #0
3:  mov   r12, r10, lsl #1
    strh  r6, [r0, r12]             @ top border
    strh  r6, [r11, r12]            @ bottom border
    add   r10, r10, #1
    cmp   r10, r1
    blt   3b
    mov   r8, r0
    sub   r12, r1, #1
    mov   r12, r12, lsl #1          @ offset of a row's last pixel
    mov   r9, #0
4:  strh  r6, [r8]                  @ left border
    strh  r6, [r8, r12]             @ right border
    add   r8, r8, r1, lsl #1
    add   r9, r9, #1
    cmp   r9, r2
    blt   4b
    pop   {r4-r11, pc}

@ Copies sprite attributes 0-2 from the list at r1 (halfword triples, ending with
@ 0xFFFF) to OAM from sprite 0, leaving the affine parameters alone. Clobbers r0-r3.
load_sprites:
    ldr   r0, =OAM
    ldr   r3, =0xFFFF
1:  ldrh  r2, [r1], #2
    cmp   r2, r3
    bxeq  lr
    strh  r2, [r0]
    ldrh  r2, [r1], #2
    strh  r2, [r0, #2]
    ldrh  r2, [r1], #2
    strh  r2, [r0, #4]
    add   r0, r0, #8
    b     1b

    .ltorg

@ Scene 0: text backgrounds and alpha blending; sprites in the margins
scene_text:
    push  {r4, lr}
    ldr   r0, =VRAM + 24 * 0x800
    mov   r1, #64
    mov   r2, #1
    bl    make_grid_map             @ BG0: 512x256 pixels of bordered tiles
    ldr   r0, =VRAM + 26 * 0x800
    mov   r1, #0
    mov   r2, #0x800
    bl    fill32                    @ BG1: 256x256 pixels, transparent...
    ldr   r0, =VRAM + 26 * 0x800
    ldr   r1, =0xF002               @ ...but for hollow tiles along row 0 and column 0,
    mov   r2, #0                    @ the seams where the map wraps around
1:  mov   r3, r2, lsl #1
    strh  r1, [r0, r3]
    mov   r3, r2, lsl #6
    strh  r1, [r0, r3]
    add   r2, r2, #1
    cmp   r2, #32
    blt   1b
    mov   r4, #IO
    ldr   r1, =(1 << 14) | (24 << 8) | 1
    strh  r1, [r4, #0x08]           @ BG0CNT: 64x32 tiles, map at 24, priority 1
    ldr   r1, =(26 << 8)
    strh  r1, [r4, #0x0A]           @ BG1CNT: 32x32 tiles, map at 26, priority 0
    mov   r1, #12
    strh  r1, [r4, #0x10]           @ BG0HOFS
    mov   r1, #5
    strh  r1, [r4, #0x12]           @ BG0VOFS
    mov   r1, #3
    strh  r1, [r4, #0x14]           @ BG1HOFS
    mov   r1, #250
    strh  r1, [r4, #0x16]           @ BG1VOFS
    ldr   r1, =0x2142
    strh  r1, [r4, #0x50]           @ BLDCNT: BG1 blended over BG0 and the backdrop
    ldr   r1, =0x0808
    strh  r1, [r4, #0x52]           @ BLDALPHA: half and half
    ldr   r1, =sprites_text
    bl    load_sprites
    ldr   r1, =0x1340
    strh  r1, [r4]                  @ DISPCNT: mode 0, 1D sprites, BG0, BG1, OBJ
    pop   {r4, pc}

@ Scene 1: an affine background, rotated, its map ending inside the margins; affine
@ double-size sprites
scene_affine:
    push  {r4, lr}
    ldr   r0, =VRAM + 24 * 0x800
    mov   r1, #64
    mov   r2, #1
    bl    make_grid_map             @ BG0: the grid, behind
    ldr   r0, =VRAM + 27 * 0x800
    bl    make_affine_map
    mov   r4, #IO
    ldr   r1, =(1 << 14) | (24 << 8) | 2
    strh  r1, [r4, #0x08]           @ BG0CNT: priority 2
    ldr   r1, =(1 << 14) | (27 << 8) | (1 << 2)
    strh  r1, [r4, #0x0C]           @ BG2CNT: 256x256, map at 27, tiles at 1, no wraparound
    ldr   r1, =222
    strh  r1, [r4, #0x20]           @ BG2PA: rotated by 30 degrees
    ldr   r1, =-128
    strh  r1, [r4, #0x22]           @ BG2PB
    mov   r1, #128
    strh  r1, [r4, #0x24]           @ BG2PC
    ldr   r1, =222
    strh  r1, [r4, #0x26]           @ BG2PD
    ldr   r1, =16368
    str   r1, [r4, #0x28]           @ BG2X: the map's center at the screen's center
    ldr   r1, =-352
    str   r1, [r4, #0x2C]           @ BG2Y
    ldr   r1, =sprites_affine
    bl    load_sprites
    ldr   r0, =OAM
    mov   r1, #181                  @ affine parameters 0: rotated by 45 degrees
    strh  r1, [r0, #0x06]           @ PA
    strh  r1, [r0, #0x0E]           @ PB
    ldr   r1, =-181
    strh  r1, [r0, #0x16]           @ PC
    mov   r1, #181
    strh  r1, [r0, #0x1E]           @ PD
    ldr   r1, =0x1541
    strh  r1, [r4]                  @ DISPCNT: mode 1, 1D sprites, BG0, BG2, OBJ
    pop   {r4, pc}

@ Scene 2: bitmap mode 3; the margins show the backdrop and sprites
scene_mode3:
    push  {r4-r5, lr}
    ldr   r0, =VRAM
    mov   r1, #240
    mov   r2, #160
    ldr   r3, =(12 << 10)           @ blue 12, red from x, green from y
    mov   r4, #0
    mov   r5, #5
    bl    fill_bitmap
    ldr   r1, =sprites_bitmap
    bl    load_sprites
    mov   r0, #IO
    ldr   r1, =0x1443
    strh  r1, [r0]                  @ DISPCNT: mode 3, 1D sprites, BG2, OBJ
    pop   {r4-r5, pc}

@ Scene 3: bitmap mode 5, 160x128 scaled to 240x160, with mosaic
scene_mode5:
    push  {r4-r5, lr}
    ldr   r0, =VRAM
    mov   r1, #160
    mov   r2, #128
    mov   r3, #8                    @ red 8, blue from x, green from y
    mov   r4, #10
    mov   r5, #5
    bl    fill_bitmap
    mov   r0, #IO
    mov   r1, #171
    strh  r1, [r0, #0x20]           @ BG2PA: 160 pixels across 240
    mov   r1, #205
    strh  r1, [r0, #0x26]           @ BG2PD: 128 lines across 160
    ldr   r1, =0x1112
    strh  r1, [r0, #0x4C]           @ MOSAIC: background 3x2, sprites 2x2
    mov   r1, #0x40
    strh  r1, [r0, #0x0C]           @ BG2CNT: mosaic
    ldr   r1, =sprites_mode5
    bl    load_sprites
    mov   r0, #IO
    ldr   r1, =0x1445
    strh  r1, [r0]                  @ DISPCNT: mode 5, 1D sprites, BG2, OBJ
    pop   {r4-r5, pc}

@ Scene 4: windows. Window 0 wraps around both screen edges in both directions, window 1
@ touches the right edge only; inside them the grid, outside plain white; an OBJ window.
scene_windows:
    push  {r4, lr}
    ldr   r0, =VRAM + 24 * 0x800
    mov   r1, #64
    mov   r2, #1
    bl    make_grid_map             @ BG0: the grid
    ldr   r0, =VRAM + 26 * 0x800
    ldr   r1, =0xF001F001
    mov   r2, #0x800
    bl    fill32                    @ BG1: white tiles (palette 15)
    mov   r4, #IO
    ldr   r1, =(1 << 14) | (24 << 8) | 1
    strh  r1, [r4, #0x08]           @ BG0CNT
    ldr   r1, =(26 << 8) | 1
    strh  r1, [r4, #0x0A]           @ BG1CNT
    ldr   r1, =(200 << 8) | 40
    strh  r1, [r4, #0x40]           @ WIN0H: x 200-239 and 0-39
    ldr   r1, =(100 << 8) | 240
    strh  r1, [r4, #0x42]           @ WIN1H: x 100-239
    ldr   r1, =(140 << 8) | 20
    strh  r1, [r4, #0x44]           @ WIN0V: y 140-159 and 0-19
    ldr   r1, =(60 << 8) | 120
    strh  r1, [r4, #0x46]           @ WIN1V: y 60-119
    ldr   r1, =0x3111
    strh  r1, [r4, #0x48]           @ WININ: window 0 BG0, OBJ; window 1 BG0, OBJ, effects
    ldr   r1, =0x0112
    strh  r1, [r4, #0x4A]           @ WINOUT: outside BG1, OBJ; OBJ window BG0
    mov   r1, #0x81
    strh  r1, [r4, #0x50]           @ BLDCNT: brighten BG0
    mov   r1, #8
    strh  r1, [r4, #0x54]           @ BLDY
    ldr   r1, =sprites_windows
    bl    load_sprites
    ldr   r1, =0xF340
    strh  r1, [r4]                  @ DISPCNT: mode 0, 1D sprites, BG0, BG1, OBJ, all windows
    pop   {r4, pc}

@ Scene 5: mosaic on a text background (5x3) and on sprites (4x2), under a grid of
@ hollow tiles without mosaic
scene_mosaic:
    push  {r4, lr}
    ldr   r0, =VRAM + 24 * 0x800
    mov   r1, #64
    mov   r2, #1
    bl    make_grid_map             @ BG0: the grid, with mosaic
    ldr   r0, =VRAM + 26 * 0x800
    ldr   r1, =0xF002F002
    mov   r2, #0x800
    bl    fill32                    @ BG1: hollow tiles everywhere
    mov   r4, #IO
    ldr   r1, =(1 << 14) | (24 << 8) | 0x40 | 1
    strh  r1, [r4, #0x08]           @ BG0CNT: mosaic, priority 1
    ldr   r1, =(26 << 8)
    strh  r1, [r4, #0x0A]           @ BG1CNT: priority 0
    mov   r1, #3
    strh  r1, [r4, #0x10]           @ BG0HOFS
    mov   r1, #2
    strh  r1, [r4, #0x12]           @ BG0VOFS
    ldr   r1, =0x1324
    strh  r1, [r4, #0x4C]           @ MOSAIC: background 5x3, sprites 4x2
    ldr   r1, =sprites_mosaic
    bl    load_sprites
    ldr   r1, =0x1340
    strh  r1, [r4]                  @ DISPCNT: mode 0, 1D sprites, BG0, BG1, OBJ
    pop   {r4, pc}

@ Scene 6: more 64x64 sprites on lines 40-103 than the OBJ cycles allow. Sprites 0-7 are
@ in the side margins; the GBA does not fetch them, so they must not use up cycles.
scene_cycles:
    push  {r4, lr}
    ldr   r0, =VRAM + 24 * 0x800
    mov   r1, #64
    mov   r2, #1
    bl    make_grid_map
    mov   r4, #IO
    ldr   r1, =(1 << 14) | (24 << 8) | 3
    strh  r1, [r4, #0x08]           @ BG0CNT: priority 3
    ldr   r1, =sprites_cycles
    bl    load_sprites
    ldr   r1, =0x1140
    strh  r1, [r4]                  @ DISPCNT: mode 0, 1D sprites, BG0, OBJ
    pop   {r4, pc}

@ Scene 7: the widescreen registers, shown as the backdrop color
scene_registers:
    push  {r4-r9, lr}
    ldr   r0, =WIDESCREEN_REGS
    ldrh  r1, [r0]                  @ 0x5357
    ldrh  r2, [r0, #2]              @ left
    ldrh  r3, [r0, #4]              @ right
    ldrh  r4, [r0, #6]              @ top
    ldrh  r5, [r0, #8]              @ bottom
    ldr   r6, [r0]                  @ 32 bits: 0x5357 | left << 16
    ldr   r7, =0x001F               @ red
    ldr   r8, =0x5357
    cmp   r1, r8
    bne   9f
    orr   r9, r1, r2, lsl #16
    cmp   r6, r9
    ldrne r7, =0x03FF               @ yellow
    bne   9f
    orr   r9, r2, r3
    orr   r9, r9, r4
    orrs  r9, r9, r5
    ldreq r7, =0x7C00               @ blue: no margins
    beq   9f
    cmp   r2, #40
    cmpeq r3, #40
    cmpeq r4, #20
    cmpeq r5, #20
    ldreq r7, =0x03E0               @ green: the declared margins
9:  ldr   r0, =PALETTE
    strh  r7, [r0]                  @ the backdrop
    mov   r0, #IO
    mov   r1, #0
    strh  r1, [r0]                  @ DISPCNT: mode 0, no layers
    pop   {r4-r9, pc}

    .ltorg

    .balign 4
scenes:
    .word scene_text, scene_affine, scene_mode3, scene_mode5
    .word scene_windows, scene_mosaic, scene_cycles, scene_registers

@ 16 palettes: color 1 a hue, color 2 the same darker, color 3 white. Color 0 of
@ palette 0 is the backdrop, dark gray.
palette:
    .hword 0x1084, 0x001F, (0x001F >> 1) & 0x3DEF, 0x7FFF
    .hword 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .irp hue, 0x021F, 0x03FF, 0x03F0, 0x03E0, 0x43E0, 0x7FE0, 0x7E00, 0x7C00, 0x7C10, 0x7C1F, 0x401F, 0x631F, 0x63F8, 0x7F18, 0x7FFF
    .hword 0, \hue, (\hue >> 1) & 0x3DEF, 0x7FFF
    .hword 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .endr

@ 4bpp background tiles
tiles4:
    .word 0, 0, 0, 0, 0, 0, 0, 0    @ 0: transparent
    .word 0x22222222                @ 1: color 1, with a color 2 border along the top
    .rept 7                         @    and the left
    .word 0x11111112
    .endr
    .word 0x33333333                @ 2: hollow: a color 3 border along the top and the
    .rept 7                         @    left, transparent inside
    .word 0x00000003
    .endr
tiles4_end:
    .if tiles4_end - tiles4 - TILES4_SIZE
    .error "TILES4_SIZE does not match tiles4"
    .endif

@ 8bpp background tiles: 0 transparent; 1-16 a tile of palette n - 1's colors 1 and 2
@ (bordered like 4bpp tile 1)
tiles8:
    .space 64
    .irp pal, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    .set t8_border, \pal * 16 + 2
    .set t8_fill, \pal * 16 + 1
    .word t8_border * 0x01010101, t8_border * 0x01010101
    .rept 7
    .word t8_border | (t8_fill << 8) | (t8_fill << 16) | (t8_fill << 24), t8_fill * 0x01010101
    .endr
    .endr
tiles8_end:
    .if tiles8_end - tiles8 - TILES8_SIZE
    .error "TILES8_SIZE does not match tiles8"
    .endif

@ A 16x16 4bpp sprite, tiles in 1D order: color 1, a color 2 border and a color 3
@ diagonal from the top left corner, which shows flips and rotation
sprite16:
    .set spr_ty, 0
    .rept 2
    .set spr_tx, 0
    .rept 2
    .set spr_r, 0
    .rept 8
    .set spr_w, 0
    .set spr_c, 0
    .rept 8
    .set spr_x, spr_tx * 8 + spr_c
    .set spr_y, spr_ty * 8 + spr_r
    .set spr_edge, ((spr_x == 0) | (spr_y == 0) | (spr_x == 15) | (spr_y == 15)) & 1
    .set spr_diag, (spr_x == spr_y) & 1
    .set spr_v, 1 + spr_edge + ((spr_diag & (1 - spr_edge)) * 2)
    .set spr_w, spr_w | (spr_v << (spr_c * 4))
    .set spr_c, spr_c + 1
    .endr
    .word spr_w
    .set spr_r, spr_r + 1
    .endr
    .set spr_tx, spr_tx + 1
    .endr
    .set spr_ty, spr_ty + 1
    .endr

@ Sprite lists: attributes 0, 1 and 2. In attribute 1, 0x4000 is 16x16 (square size 1),
@ 0x8000 32x32, 0xC000 64x64; x from 240 is in the right margin, x from 472 in the left
@ one (x - 512); y from 160 is in the bottom margin, y from 236 in the top one (y - 256).
sprites_text:
    .hword  40, 0x4000 | 250, SPRITE16_TILE | (0 << 12)            @ right margin
    .hword  40, 0x5000 | 482, SPRITE16_TILE | (1 << 12)            @ left margin, flipped horizontally
    .hword 238, 0x6000 |  60, SPRITE16_TILE | (2 << 12)            @ top margin, flipped vertically
    .hword 162, 0x4000 |  60, SPRITE16_TILE | (3 << 12)            @ bottom margin
    .hword 170, 0x4000 | 270, SPRITE16_TILE | (4 << 12)            @ bottom right corner, cut by the picture's edges
    .hword 100, 0x4000 | 232, SPRITE16_TILE | (5 << 12)            @ across the right edge
    .hword 152, 0x4000 | 120, SPRITE16_TILE | (6 << 12)            @ across the bottom edge
    .hword 100, 0x4000 | 504, SPRITE16_TILE | (7 << 12)            @ across the left edge
    .hword 248, 0x4000 | 160, SPRITE16_TILE | (8 << 12)            @ across the top edge
    .hword 250, 0x7000 | 500, SPRITE16_TILE | (9 << 12)            @ across the top left corner, flipped both ways
    .hword  40, 0x4000 | 300, SPRITE16_TILE | (10 << 12)           @ beyond the right margin: not shown
    .hword 200, 0x4000 | 100, SPRITE16_TILE | (11 << 12)           @ below the bottom margin: not shown
    .hword  60, 0x8000 | 460, GRID_TILE | (12 << 12)               @ 32x32, half in the left margin
    .hword 0xFFFF

sprites_affine:
    .hword  60 | 0x300, 0x4000 | 476, SPRITE16_TILE | (0 << 12)    @ left margin
    .hword 110 | 0x300, 0x4000 | 228, SPRITE16_TILE | (1 << 12)    @ across the right edge
    .hword 150 | 0x300, 0x4000 | 200, SPRITE16_TILE | (2 << 12)    @ across the bottom edge
    .hword 236 | 0x300, 0x4000 | 100, SPRITE16_TILE | (3 << 12)    @ across the top edge
    .hword 216 | 0x300, 0x4000 | 160, SPRITE16_TILE | (4 << 12)    @ top margin
    .hword 0xFFFF

sprites_bitmap:
    .hword  70, 0x4000 | 256, SPRITE16_TILE | (0 << 12)            @ right margin
    .hword  70, 0x4000 | 490, SPRITE16_TILE | (4 << 12)            @ left margin
    .hword 242, 0x4000 | 112, SPRITE16_TILE | (8 << 12)            @ top margin
    .hword 164, 0x4000 | 112, SPRITE16_TILE | (12 << 12)           @ bottom margin
    .hword 0xFFFF

sprites_mode5:
    .hword  60 | 0x1000, 0x4000 | 246, SPRITE16_TILE | (1 << 12)   @ right margin, mosaic
    .hword  60 | 0x1000, 0x4000 | 478, SPRITE16_TILE | (5 << 12)   @ left margin, mosaic
    .hword 0xFFFF

sprites_windows:
    .hword  80 | 0x800, 0x8000 | 488, GRID_TILE                    @ OBJ window, across the left edge
    .hword 140 | 0x800, 0xC000 | 100, GRID_TILE                    @ OBJ window, across the bottom edge
    .hword 238, 0x4000 |  10, SPRITE16_TILE | (3 << 12)            @ top margin, in window 0
    .hword  90, 0x4000 | 250, SPRITE16_TILE | (6 << 12)            @ right margin, in window 1
    .hword 0xFFFF

sprites_mosaic:
    .hword  50 | 0x1000, 0x4000 | 252, SPRITE16_TILE | (2 << 12)   @ right margin
    .hword  50 | 0x1000, 0x4000 | 486, SPRITE16_TILE | (6 << 12)   @ left margin
    .hword 240 | 0x1000, 0x4000 |  80, SPRITE16_TILE | (10 << 12)  @ top margin
    .hword 165 | 0x1000, 0x4000 |  80, SPRITE16_TILE | (14 << 12)  @ bottom margin
    .hword 110 | 0x1000, 0x4000 | 506, SPRITE16_TILE | (3 << 12)   @ across the left edge
    .hword 0xFFFF

sprites_cycles:
    .set spr_n, 0
    .rept 6
    .hword 40, 0xC000 | (240 + spr_n * 6), GRID_TILE | (spr_n << 12)   @ right margin
    .set spr_n, spr_n + 1
    .endr
    .hword 40, 0xC000 | 440, GRID_TILE | (6 << 12)                 @ left margin
    .hword 40, 0xC000 | 444, GRID_TILE | (7 << 12)
    .set spr_n, 0
    .rept 32
    .hword 40, 0xC000 | (spr_n * 5), GRID_TILE | ((spr_n & 15) << 12)  @ on the screen
    .set spr_n, spr_n + 1
    .endr
    .hword 0xFFFF
