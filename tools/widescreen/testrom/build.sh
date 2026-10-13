#!/bin/sh
# Builds wstest.gba (needs GNU binutils for arm-none-eabi).
set -eu
cd "$(dirname "$0")"
arm-none-eabi-as -mcpu=arm7tdmi -o wstest.o wstest.s
printf 'SECTIONS { . = 0x08000000; .text : { *(.text*) } }\n' > wstest.ld
arm-none-eabi-ld -T wstest.ld -o wstest.elf wstest.o
arm-none-eabi-objcopy -O binary wstest.elf wstest.gba
rm -f wstest.o wstest.ld wstest.elf
chmod 644 wstest.gba
