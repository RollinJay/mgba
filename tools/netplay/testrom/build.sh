#!/bin/sh
# Builds linkstress.gba (needs GNU binutils for arm-none-eabi).
set -eu
cd "$(dirname "$0")"
arm-none-eabi-as -mcpu=arm7tdmi -o linkstress.o linkstress.s
printf 'SECTIONS { . = 0x08000000; .text : { *(.text*) } }\n' > linkstress.ld
arm-none-eabi-ld -T linkstress.ld -o linkstress.elf linkstress.o
arm-none-eabi-objcopy -O binary linkstress.elf linkstress.gba
rm -f linkstress.o linkstress.ld linkstress.elf
