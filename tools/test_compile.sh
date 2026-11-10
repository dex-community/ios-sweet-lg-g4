#!/bin/bash
set -e
CC=$(command -v aarch64-elf-gcc || command -v aarch64-linux-gnu-gcc)
$CC -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -DSHIM_HW_DIAG=1 -c shim/emmc.c -o /tmp/emmc_test.o
echo "Compiles successfully!"
rm -f /tmp/emmc_test.o
