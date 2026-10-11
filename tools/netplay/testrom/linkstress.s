@ linkstress.s - link cable stress test for gbanet.
@
@ An emulator test program: it boots in mGBA but not on hardware (no Nintendo logo).
@ Run one copy per GBA on a linked machine. The GBAs alternate every 120 frames between
@   multiplayer mode: 115200 baud; the parent (SI=0) starts a new transfer as soon as
@                     the previous one has finished, so the cable is almost always busy
@   Normal mode:      32-bit, 256 kHz; the parent drives the clock on every timer-0
@                     tick (4096 cycles); the child re-arms after every transfer
@ Every completed transfer folds the received data into a hash and sends
@ hash ^ count ^ keys next, so keypad input reaches the other GBAs through the cable.
@ IWRAM 0x03000100: +0 hash, +4 transfers, +8 keys, +12 phase (0 multi, 1 normal),
@                   +16 frames left in this phase, +20 parent flag, +24 frames
    .syntax unified
    .cpu arm7tdmi
    .arm
    .section .text, "ax"
    .global _start

    .equ STATE,  0x03000100
    .equ PHASE_FRAMES, 120

_start:
    b     main                      @ 0x00 (mGBA checks for the 0xEA branch opcode)
    .space 156                      @ 0x04: logo (left empty)
    .ascii "LINKSTRESS  "           @ 0xA0: title
    .ascii "GBNT"                   @ 0xAC: game code
    .ascii "00"                     @ 0xB0: maker code
    .byte 0x96                      @ 0xB2: fixed value
    .byte 0, 0                      @ 0xB3: unit code, device type
    .space 7
    .byte 0, 0                      @ 0xBC: version, complement (unchecked by mGBA)
    .space 2

main:
    mov   r0, #0x04000000
    mov   r1, #0
    strh  r1, [r0]                  @ DISPCNT = 0
    ldr   r3, =STATE
    mov   r2, #0
1:  str   r1, [r3, r2]
    add   r2, r2, #4
    cmp   r2, #32
    blt   1b
    mov   r1, #PHASE_FRAMES
    str   r1, [r3, #16]
    ldr   r1, =irq
    ldr   r2, =0x03007FFC
    str   r1, [r2]                  @ IRQ handler address used by the BIOS
    mov   r1, #8
    strh  r1, [r0, #4]              @ DISPSTAT: VBlank IRQ
    add   r2, r0, #0x200
    ldr   r1, =0x0089               @ IE: VBlank, timer 0, serial
    strh  r1, [r2]
    mov   r1, #1
    strh  r1, [r2, #8]              @ IME
    add   r0, r0, #0x100            @ r0 = 0x04000100: timers, serial, keypad
    bl    enter_multi
    ldrh  r1, [r0, #0x28]
    tst   r1, #4                    @ SI: 0 = parent (first plug)
    moveq r1, #1
    movne r1, #0
    str   r1, [r3, #20]

loop:
    ldrh  r1, [r0, #0x30]          @ keys (active low)
    mvn   r1, r1
    ldr   r2, =0x3FF
    and   r1, r1, r2
    str   r1, [r3, #8]
    ldr   r1, [r3, #12]
    cmp   r1, #0                    @ multiplayer phase?
    bne   loop
    ldr   r1, [r3, #20]
    cmp   r1, #0                    @ parent?
    beq   loop
    ldrh  r1, [r0, #0x28]
    and   r2, r1, #0x88
    cmp   r2, #0x08                 @ every GBA ready, not busy
    bne   loop
    orr   r1, r1, #0x80             @ start
    strh  r1, [r0, #0x28]
    b     loop

@ Switch the serial port to multiplayer mode (r0 = 0x04000100, r3 = STATE). Clobbers r1, r2.
enter_multi:
    mov   r1, #0
    strh  r1, [r0, #0x02]          @ timer 0 off
    strh  r1, [r0, #0x34]          @ RCNT: serial modes
    ldr   r1, =0x6003               @ multiplayer, 115200 baud, IRQ
    strh  r1, [r0, #0x28]
    ldr   r1, [r3]
    strh  r1, [r0, #0x2A]          @ SIOMLT_SEND
    mov   r1, #0
    str   r1, [r3, #12]
    bx    lr

@ Switch to Normal 32-bit mode (same registers). Clobbers r1, r2.
enter_normal:
    mov   r1, #0
    strh  r1, [r0, #0x34]
    ldr   r2, [r3, #20]
    cmp   r2, #0
    ldrne r1, =0x5001               @ parent: internal 256 kHz clock, 32-bit, IRQ
    ldreq r1, =0x5000               @ child: external clock
    strh  r1, [r0, #0x28]
    ldr   r1, [r3]
    str   r1, [r0, #0x20]          @ SIODATA32
    cmp   r2, #0
    ldreq r1, =0x5080               @ child: armed
    strheq r1, [r0, #0x28]
    ldrne r1, =0xF000               @ parent: timer 0 every 4096 cycles
    strhne r1, [r0, #0x00]
    movne r1, #0xC0
    strhne r1, [r0, #0x02]
    mov   r1, #1
    str   r1, [r3, #12]
    bx    lr

irq:
    stmfd sp!, {r4-r7, lr}
    mov   r0, #0x04000000
    ldr   r3, =STATE
    add   r1, r0, #0x200
    ldrh  r7, [r1, #2]              @ IF
    strh  r7, [r1, #2]              @ acknowledge
    add   r0, r0, #0x100            @ r0 = 0x04000100
    tst   r7, #0x80
    blne  on_serial
    tst   r7, #0x08
    blne  on_timer
    tst   r7, #0x01
    blne  on_vblank
    ldmfd sp!, {r4-r7, lr}
    bx    lr

on_serial:
    ldr   r4, [r3]                  @ hash
    add   r4, r4, r4, lsl #5        @ * 33
    ldr   r1, [r3, #12]
    cmp   r1, #0
    bne   2f
    ldrh  r5, [r0, #0x20]          @ SIOMULTI0-3
    eor   r4, r4, r5
    ldrh  r5, [r0, #0x22]
    eor   r4, r4, r5, lsl #8
    ldrh  r5, [r0, #0x24]
    eor   r4, r4, r5, lsl #16
    ldrh  r5, [r0, #0x26]
    eor   r4, r4, r5, lsl #24
    b     3f
2:  ldr   r5, [r0, #0x20]          @ SIODATA32
    eor   r4, r4, r5
3:  str   r4, [r3]
    ldr   r5, [r3, #4]
    add   r5, r5, #1
    str   r5, [r3, #4]
    ldr   r6, [r3, #8]
    eor   r6, r6, r4
    eor   r6, r6, r5                @ next outgoing word
    cmp   r1, #0
    strheq r6, [r0, #0x2A]         @ multiplayer: SIOMLT_SEND
    bxeq  lr
    str   r6, [r0, #0x20]          @ Normal: SIODATA32
    ldr   r2, [r3, #20]
    cmp   r2, #0
    bxne  lr
    ldrh  r1, [r0, #0x28]          @ child: re-arm
    orr   r1, r1, #0x80
    strh  r1, [r0, #0x28]
    bx    lr

on_timer:
    ldr   r1, [r3, #12]
    cmp   r1, #1
    bxne  lr
    ldrh  r1, [r0, #0x28]
    tst   r1, #0x80                 @ previous transfer still running
    bxne  lr
    orr   r1, r1, #0x80
    strh  r1, [r0, #0x28]
    bx    lr

on_vblank:
    ldr   r1, [r3, #24]
    add   r1, r1, #1
    str   r1, [r3, #24]
    ldr   r1, [r3, #16]
    subs  r1, r1, #1
    movle r1, #PHASE_FRAMES
    str   r1, [r3, #16]
    bxgt  lr
    mov   r6, lr
    ldr   r1, [r3, #12]
    cmp   r1, #0
    bne   4f
    bl    enter_normal
    bx    r6
4:  bl    enter_multi
    bx    r6

    .pool
