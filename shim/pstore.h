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

/*
 * Read back the log of the PREVIOUS run and print it (2026-11-10).
 *
 * The pstore console zone holds the LAST writer only, and the recovery
 * overwrites it with its own console as soon as it boots: the log of the
 * run that just finished is unreadable by then. The shim runs before
 * anything else touches the zone, so it can read the record of the
 * previous run and print it. Every run therefore prints the one before it,
 * with no external tool in between.
 *
 * It must be called BEFORE pstore_console_write() of this run.
 */
void pstore_read_previous(void);
