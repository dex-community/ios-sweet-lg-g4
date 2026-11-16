/*
 * log.h -- the log of the shim, and the places it has to be written.
 *
 * THE LOG HAS THREE PLACES
 * -------------------------
 *
 *   the RAM of the shim (con_log)   the log as it is written
 *   0x50000000                      a copy in plain RAM, for a recovery
 *                                    that reads it with /dev/mem
 *   the ramoops console zone        what the next kernel reads back as
 *                                    /sys/fs/pstore/console-ramoops-0
 *
 * The ramoops is the one that survives everything, and it is the one
 * that the recovery can read without any special kernel: it is what
 * makes a run that HANGS readable (the shim never reaches the end of
 * the run, so the copy in RAM is never written).
 *
 * So the log is written at every point where the run may end:
 *
 *   - before anything risky (a hang leaves the record of what was done);
 *   - when the run ends normally;
 *   - in the panic handler (a fault is described only there);
 *   - right before the jump to XNU (a kernel that dies at its first
 *     instruction leaves no trace of its own, and ours is the last one
 *     that was written).
 */
#pragma once
#include <stdint.h>

/* Write the log to the RAM and to the ramoops. */
void shim_log_flush(void);
