/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

#include "gxfp/algo/image/decoder.h"

struct gxfp51b7;

/*
 * Blocking calls; libfprint runs them outside the GLib main context.
 * open() initialises the sensor with a config built from its OTP,
 * authenticates TLS and calibrates the no-finger background, so the finger
 * must be off the sensor.
 */
int gxfp51b7_open(struct gxfp51b7 **out, const char *dev,
		  const char *key, int log);
void gxfp51b7_close(struct gxfp51b7 *s);
void gxfp51b7_set_cancel(struct gxfp51b7 *s, int (*cancel)(void *), void *ctx);
int gxfp51b7_stop(struct gxfp51b7 *s);
/* Arm finger detection: present != 0 waits for a touch, 0 for removal. */
int gxfp51b7_arm(struct gxfp51b7 *s, int present);
/* Nonblocking: 1 for a confirmed transition, 0 for none, -errno on error. */
int gxfp51b7_poll(struct gxfp51b7 *s, int *present);
/* -ENODATA: contrast below the calibrated noise, ask for another touch. */
int gxfp51b7_capture(struct gxfp51b7 *s, struct gxfp_decoded_image *out);
