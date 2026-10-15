/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ChicagoHS config from OTP. The expected numbers below are worked out by
 * hand from the gfspi formulas, not taken from the code under test.
 *
 * GXFP51B7_PRIVATE_VECTORS=<dir> additionally compares the config built from
 * <dir>/otp.bin with <dir>/config.bin, both from a Windows capture of one
 * sensor. Only the verdict is printed, never the bytes.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gxfp/algo/chicago_hs_cfg.h"

#define CHECK(x) do {							\
		if (!(x)) {						\
			fprintf(stderr, "line %d: %s\n", __LINE__, #x);	\
			exit(1);					\
		}							\
	} while (0)

/* Byte offsets in the type 12 template. */
#define TCODE_LO 0xeb		/* 005c value, image section at 0xe5 */
#define TCODE_HI 0xec
#define DELTA 0xc8		/* 0082 high byte, FDT section at 0x9d */
#define OFFSET 0xb3		/* 0056 low byte, FDT section */

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
	uint32_t h = 0x811c9dc5;

	while (n--)
		h = (h ^ *p++) * 0x01000193;
	return h;
}

/* All-zero OTP with the three CRC bytes set: CRC8 of zeros is 0, stored ~0. */
static void blank_otp(uint8_t *otp)
{
	memset(otp, 0, GXFP_CHICAGO_HS_OTP_LEN);
	otp[0x3c] = otp[0x3d] = otp[0x3f] = 0xff;
}

/* cfg differs from base exactly at the listed offsets, plus fe/ff. */
static int differs_only_at(const uint8_t *cfg, const uint8_t *base,
			   const unsigned int *at, unsigned int n)
{
	for (unsigned int i = 0; i < 0xfe; i++) {
		int listed = 0;

		for (unsigned int k = 0; k < n; k++)
			listed |= at[k] == i;
		if ((cfg[i] != base[i]) != listed)
			return 0;
	}
	return 1;
}

static void check_offset(uint8_t byte, uint8_t offset, const uint8_t *base)
{
	uint8_t otp[GXFP_CHICAGO_HS_OTP_LEN], cfg[GXFP_CHICAGO_HS_CFG_LEN];
	struct gxfp_chicago_hs_calibration cal;
	const unsigned int at[] = { OFFSET };

	blank_otp(otp);
	otp[0x1b] = byte;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(cal.fdt_offset == offset);
	CHECK(gxfp_chicago_hs_config_checksum_ok(cfg));
	if (offset)
		CHECK(cfg[OFFSET] == offset + 4 && differs_only_at(cfg, base, at, 1));
	else
		CHECK(!memcmp(cfg, base, sizeof(cfg)));
}

static int read_exact(const char *dir, const char *name, uint8_t *buf, size_t len)
{
	char path[4096];
	FILE *f;
	size_t n;
	int extra;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f)
		return -errno;
	n = fread(buf, 1, len, f);
	extra = fgetc(f) != EOF;
	fclose(f);
	return n == len && !extra ? 0 : -EINVAL;
}

int main(void)
{
	uint8_t otp[GXFP_CHICAGO_HS_OTP_LEN], base[GXFP_CHICAGO_HS_CFG_LEN];
	uint8_t cfg[GXFP_CHICAGO_HS_CFG_LEN];
	struct gxfp_chicago_hs_calibration cal;
	const char *dir;

	CHECK(gxfp_chicago_hs_build_config(NULL, cfg, NULL) == -EINVAL);
	CHECK(gxfp_chicago_hs_build_config(otp, NULL, NULL) == -EINVAL);

	/* No calibration byte: the template with a fresh checksum. */
	blank_otp(otp);
	CHECK(gxfp_chicago_hs_build_config(otp, base, &cal) == 0);
	CHECK(cal.crc_ok && !cal.has_tcode && cal.fdt_offset == 0);
	CHECK(cal.tcode == 0x0100 && cal.fdt_delta == 0x15);
	CHECK(base[TCODE_LO] == 0x00 && base[TCODE_HI] == 0x01);
	CHECK(base[DELTA] == 0x15 && base[OFFSET] == 0x04);
	CHECK(base[0xfe] == 0x93 && base[0xff] == 0x0e);
	CHECK(gxfp_chicago_hs_config_checksum_ok(base));
	/* Template bytes as extracted from gfspi.dll 1.1.151.18. */
	CHECK(fnv1a(base, sizeof(base)) == 0xbe9dcc38);
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, NULL) == 0);
	CHECK(!memcmp(cfg, base, sizeof(cfg)));
	cfg[0x40] ^= 1;
	CHECK(!gxfp_chicago_hs_config_checksum_ok(cfg));

	/* 0x5a: tcode (5 + 1) * 16 + 64 = 160; delta 1200 * 256 / 160 / 3 >> 4 = 40. */
	otp[0x2a] = 0x5a;
	otp[0x2b] = 0xa5;
	otp[0x2d] = 0x5a;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(cal.has_tcode && cal.tcode == 160 && cal.fdt_delta == 40);
	CHECK(!cal.crc_ok);	/* mt CRC covers 0x28..0x31 */
	{
		const unsigned int at[] = { TCODE_LO, TCODE_HI, DELTA };

		CHECK(cfg[TCODE_LO] == 160 && cfg[TCODE_HI] == 0 && cfg[DELTA] == 40);
		CHECK(differs_only_at(cfg, base, at, 3));
		CHECK(gxfp_chicago_hs_config_checksum_ok(cfg));
	}

	/* 0xff: tcode 16 * 16 + 64 = 0x140 needs both bytes; delta 1700 * 256 / 320 / 3 >> 4 = 28. */
	otp[0x2a] = 0xff;
	otp[0x2b] = 0x00;
	otp[0x2d] = 0xff;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(cal.tcode == 0x140 && cal.fdt_delta == 28);
	CHECK(cfg[TCODE_LO] == 0x40 && cfg[TCODE_HI] == 0x01 && cfg[DELTA] == 28);

	/* Complement and copy both wrong: template values stay. */
	otp[0x2a] = 0x5a;
	otp[0x2b] = 0x00;
	otp[0x2d] = 0x11;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(!cal.has_tcode && !memcmp(cfg, base, sizeof(cfg)));

	/* Complement wrong, copy equal to 0x2a: accepted. */
	otp[0x2a] = 0x5a;
	otp[0x2b] = 0x00;
	otp[0x2d] = 0x5a;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(cal.has_tcode && cal.tcode == 160);

	/* 0x2a empty, copy 0x2d valid: Windows accepts it but reloads 0x2a = 0. */
	otp[0x2a] = 0x00;
	otp[0x2b] = 0xc8;
	otp[0x2d] = 0x37;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(cal.has_tcode && cal.tcode == 80 && cal.fdt_delta == 13);

	/* FDT offset: low pair, high pair, complement of bits 2..3. */
	check_offset(0x00, 0, base);
	check_offset(0x22, 2, base);	/* low == high */
	check_offset(0x09, 1, base);	/* low == ~bits */
	check_offset(0x31, 3, base);	/* high == ~bits */
	check_offset(0x0d, 0, base);	/* high == ~bits == 0 */
	check_offset(0x0b, 0, base);	/* no two agree */

	/* CRC8 poly 0x07 of "123456789" is 0xf4 (CRC-8/SMBUS); stored inverted. */
	blank_otp(otp);
	memcpy(otp + 6, "12345", 5);
	memcpy(otp + 0x24, "6789", 4);
	otp[0x3c] = 0x0b;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(cal.crc_ok);
	otp[0x3c] = 0x0c;
	CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
	CHECK(!cal.crc_ok);

	dir = getenv("GXFP51B7_PRIVATE_VECTORS");
	if (dir) {
		uint8_t want[GXFP_CHICAGO_HS_CFG_LEN];

		CHECK(read_exact(dir, "otp.bin", otp, sizeof(otp)) == 0);
		CHECK(read_exact(dir, "config.bin", want, sizeof(want)) == 0);
		CHECK(gxfp_chicago_hs_build_config(otp, cfg, &cal) == 0);
		CHECK(cal.crc_ok && cal.has_tcode);
		CHECK(!memcmp(cfg, want, sizeof(want)));
		puts("private vector: config built from otp.bin equals config.bin");
	}
	memset(otp, 0, sizeof(otp));
	puts("ChicagoHS config: template, tcode, delta, defaults, 0x2d quirk, FDT offset, OTP CRC8: PASS");
	return 0;
}
