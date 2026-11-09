/*
 * sd.h -- the bring-up of the microSD slot (SDCC2).
 *
 * See shim/sd.c for what it does and WHY THE ORDER MATTERS (the clock
 * before the controller).
 */
#pragma once
#include <stdint.h>

/* The result of the bring-up: 0 = the card is identified and the first
 * sector has been read. */
int sd_init(uint8_t *block0);

/* How far it got (the step names are in sd.c). */
extern int sd_init_stage;
extern int sd_init_error;

/* What the card answered. */
extern uint16_t sd_rca;
extern uint32_t sd_cid[4];
extern uint32_t sd_ocr;
