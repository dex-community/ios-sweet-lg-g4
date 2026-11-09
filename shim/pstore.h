#pragma once
#include <stdint.h>

/*
 * Leave the shim's log in the ramoops console zone, where the next Android
 * boot reads it back as /sys/fs/pstore/console-ramoops-0 (see pstore.c).
 *
 * The write is attempted at most twice: once when the eMMC bring-up is
 * over and once in reset(). A third attempt is never made, so that a fault
 * in the second one cannot loop in the exception handler.
 */
void pstore_console_write(const char *text, unsigned len);
