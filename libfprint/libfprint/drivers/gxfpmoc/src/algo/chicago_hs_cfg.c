/* SPDX-License-Identifier: GPL-2.0 */
#include "gxfp/algo/chicago_hs_cfg.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/*
 * gfspi.dll .data RVA 0x93510 + 12 * 0x100: the chip 0x2504 entry of the
 * download_general_config table. Byte 0, then (offset, length) pairs of the
 * register sections; a section is a list of (LE16 register, LE16 value).
 * The checksum in fe/ff is stale in the DLL too and is always recomputed.
 */
static const uint8_t template12[GXFP_CHICAGO_HS_CFG_LEN] = {
	0x70, 0x11, 0x60, 0x71, 0x2c, 0x9d, 0x2c, 0xc9, 0x1c, 0xe5, 0x18, 0xfd, 0x00, 0xfd, 0x00, 0xfd,
	0x03, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x04, 0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00,
	0xc4, 0x88, 0x00, 0x00, 0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
	0xc1, 0x90, 0x00, 0xbb, 0xbb, 0x92, 0x00, 0xb1, 0xb1, 0x94, 0x00, 0x00, 0xa8, 0x96, 0x00, 0x00,
	0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
	0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
	0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
	0x40, 0x2a, 0x01, 0x02, 0x04, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x32, 0x00, 0x80, 0x00, 0x01,
	0x00, 0x5c, 0x00, 0x80, 0x00, 0x56, 0x00, 0x24, 0x20, 0x58, 0x00, 0x03, 0x02, 0x32, 0x00, 0x0c,
	0x02, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80, 0x15, 0x2a, 0x01, 0x82,
	0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14, 0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00,
	0x01, 0x56, 0x00, 0x04, 0x20, 0x58, 0x00, 0x03, 0x02, 0x32, 0x00, 0x0c, 0x02, 0x66, 0x00, 0x03,
	0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80, 0x15, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80,
	0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04, 0x03, 0x64, 0x00, 0x19, 0x00, 0x66, 0x00, 0x03,
	0x00, 0x7c, 0x00, 0x01, 0x58, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x52, 0x00, 0x08,
	0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x01, 0x58, 0x00, 0xbc, 0xff,
};

/* Header bytes holding (offset, length) of the sections patched below. */
enum {
	SECTION_FDT = 5,	/* 0082 delta, 0056 offset */
	SECTION_IMAGE = 9,	/* 005c tcode */
};

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* 1c0b0: all 128 words, checksum included, sum to 0x5a5b. */
static uint16_t word_sum(const uint8_t *cfg, unsigned int words)
{
	uint16_t sum = 0;

	for (unsigned int i = 0; i < words; i++)
		sum += read_le16(cfg + 2 * i);
	return sum;
}

static void set_checksum(uint8_t *cfg)
{
	uint16_t sum = 0x5a5b - word_sum(cfg, 127);

	cfg[0xfe] = (uint8_t)sum;
	cfg[0xff] = (uint8_t)(sum >> 8);
}

/* Offset of the value of the next entry for reg at or after *pos, or -1. */
static int next_reg(const uint8_t *cfg, unsigned int section, uint16_t reg,
		    unsigned int *pos)
{
	unsigned int off = cfg[section], len = cfg[section + 1];

	for (; *pos < len; *pos += 4) {
		unsigned int at = off + *pos;

		if (at + 4 > GXFP_CHICAGO_HS_CFG_LEN - 2)
			return -1;
		if (read_le16(cfg + at) == reg) {
			*pos += 4;
			return (int)at + 2;
		}
	}
	return -1;
}

static int first_reg(const uint8_t *cfg, unsigned int section, uint16_t reg)
{
	unsigned int pos = 0;

	return next_reg(cfg, section, reg, &pos);
}

/* CRC8, polynomial 0x07, inverted result (1ca40). */
static uint8_t crc8(uint8_t crc, const uint8_t *p, size_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int bit = 0; bit < 8; bit++)
			crc = crc & 0x80 ? (uint8_t)(crc << 1) ^ 0x07 : (uint8_t)(crc << 1);
	}
	return crc;
}

struct otp_span {
	uint8_t at, len;
};

static int otp_crc_ok(const uint8_t *otp, const struct otp_span *span,
		      unsigned int spans, unsigned int crc_at)
{
	uint8_t crc = 0;

	for (unsigned int i = 0; i < spans; i++)
		crc = crc8(crc, otp + span[i].at, span[i].len);
	return (uint8_t)~crc == otp[crc_at];
}

/* ChicagoHU_OTPCRCCheck (2e1a0): cp, mt and ft blocks. */
static int otp_crcs_ok(const uint8_t *otp)
{
	static const struct otp_span cp[] = { { 0x00, 11 }, { 0x24, 4 } };
	static const struct otp_span mt[] = {
		{ 0x14, 8 }, { 0x1d, 7 }, { 0x28, 10 }, { 0x36, 2 },
	};
	static const struct otp_span ft[] = {
		{ 0x0b, 9 }, { 0x1c, 1 }, { 0x32, 4 }, { 0x38, 4 }, { 0x3e, 1 },
	};

	return otp_crc_ok(otp, cp, 2, 0x3c) & otp_crc_ok(otp, mt, 4, 0x3f) &
	       otp_crc_ok(otp, ft, 5, 0x3d);
}

int gxfp_chicago_hs_config_checksum_ok(const uint8_t cfg[GXFP_CHICAGO_HS_CFG_LEN])
{
	return cfg && word_sum(cfg, 128) == 0x5a5b;
}

int gxfp_chicago_hs_build_config(const uint8_t otp[GXFP_CHICAGO_HS_OTP_LEN],
				 uint8_t cfg[GXFP_CHICAGO_HS_CFG_LEN],
				 struct gxfp_chicago_hs_calibration *cal)
{
	struct gxfp_chicago_hs_calibration c = { 0 };
	uint8_t a, b, d, f;
	int at;

	if (!otp || !cfg)
		return -EINVAL;
	memcpy(cfg, template12, sizeof(template12));
	c.crc_ok = otp_crcs_ok(otp);

	/* Calibration byte 0x2a, its complement 0x2b and its copy 0x2d. */
	a = otp[0x2a];
	b = otp[0x2b];
	d = otp[0x2d];
	c.has_tcode = (a && a == (uint8_t)~b) || (d && d == (uint8_t)~b) ||
		      (a && d == a);
	if (c.has_tcode) {
		/* Windows recomputes from 0x2a even when it accepted 0x2d. */
		unsigned int tcode = ((a >> 4) + 1) * 16 + 64;
		unsigned int threshold = ((a & 15) + 2) * 100;
		unsigned int delta = (threshold * 256 / tcode / 3 >> 4) & 255;

		at = first_reg(cfg, SECTION_IMAGE, 0x005c);
		if (at >= 0) {
			cfg[at] = (uint8_t)tcode;
			cfg[at + 1] = (uint8_t)(tcode >> 8);
		}
		at = first_reg(cfg, SECTION_FDT, 0x0082);
		if (at >= 0)
			cfg[at + 1] = (uint8_t)delta;
	}

	/* FDT offset: two of the three 2-bit copies in 0x1b must agree. */
	f = otp[0x1b];
	if (f) {
		unsigned int lo = f & 3, hi = f >> 4 & 3, inv = ~(unsigned int)f >> 2 & 3;

		if (lo == hi || lo == inv)
			c.fdt_offset = lo;
		else if (hi == inv)
			c.fdt_offset = hi;
	}
	if (c.fdt_offset) {
		unsigned int pos = 0;

		while ((at = next_reg(cfg, SECTION_FDT, 0x0056, &pos)) >= 0)
			cfg[at] = c.fdt_offset + 4;
	}
	set_checksum(cfg);

	at = first_reg(cfg, SECTION_IMAGE, 0x005c);
	c.tcode = at >= 0 ? read_le16(cfg + at) : 0;
	at = first_reg(cfg, SECTION_FDT, 0x0082);
	c.fdt_delta = at >= 0 ? cfg[at + 1] : 0;
	if (cal)
		*cal = c;
	return 0;
}
