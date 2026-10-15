/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

#include <stdint.h>

/*
 * GXFP51B7 (ChicagoHS, chip 0x2504) volatile sensor config, built from the
 * sensor's own 64-byte OTP the way gfspi.dll 1.1.151.18 does it:
 *
 *   download_general_config (1c11c)  copy the sensor type 12 template
 *   ChicagoHU_check_sensor (2e920)   OTP 0x2a -> tcode, register 005c of the
 *                                    section at header bytes 9/10 (1c66c),
 *                                    and FDT delta, high byte of register
 *                                    0082 of the section at 5/6 (1c200);
 *                                    OTP 0x1b -> FDT offset, low byte of
 *                                    register 0056 of that section (1c4c0)
 *
 * Each step rewrites the checksum in bytes fe/ff. Pure functions: nothing
 * here talks to the device.
 */
#define GXFP_CHICAGO_HS_OTP_LEN 64
#define GXFP_CHICAGO_HS_CFG_LEN 256

struct gxfp_chicago_hs_calibration {
	/* The three CRC8s of the OTP match. Windows only logs a mismatch. */
	int crc_ok;
	/* OTP 0x2a/0x2b/0x2d accepted; otherwise the template values stay. */
	int has_tcode;
	/* Value of register 005c in the image section. */
	uint16_t tcode;
	/* High byte of register 0082: the FDT UP margin. */
	uint8_t fdt_delta;
	/* 0..3 from OTP 0x1b; nonzero sets the low byte of 0056 to offset + 4. */
	uint8_t fdt_offset;
};

/* Returns 0, or -EINVAL for NULL otp/cfg. cal may be NULL. */
int gxfp_chicago_hs_build_config(const uint8_t otp[GXFP_CHICAGO_HS_OTP_LEN],
				 uint8_t cfg[GXFP_CHICAGO_HS_CFG_LEN],
				 struct gxfp_chicago_hs_calibration *cal);
/* 1 if the 128 LE16 words of cfg sum to 0x5a5b, else 0. */
int gxfp_chicago_hs_config_checksum_ok(const uint8_t cfg[GXFP_CHICAGO_HS_CFG_LEN]);
