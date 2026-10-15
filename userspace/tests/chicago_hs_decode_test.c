/* SPDX-License-Identifier: GPL-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gxfp/algo/image/decoder.h"

#define CHECK(x) do {							\
		if (!(x)) {						\
			fprintf(stderr, "line %d: %s\n", __LINE__, #x);	\
			exit(1);					\
		}							\
	} while (0)

/* Distinct value per active pixel; padding rows carry a 0xfXX marker. */
static uint16_t value(unsigned int row, unsigned int col)
{
	return row < 64 ? (row * 37 + col * 11) & 4095 : 0xf00 | (row & 255);
}

int main(void)
{
	uint8_t payload[10560], framed[10569];
	struct gxfp_decoded_image image = { 0 };

	/* Pack 80 columns of 88 12-bit samples, 4 samples per 6 bytes. */
	for (unsigned int col = 0; col < 80; col++) {
		for (unsigned int row = 0; row < 88; row += 4) {
			uint16_t a = value(row, col), b = value(row + 1, col);
			uint16_t c = value(row + 2, col), d = value(row + 3, col);
			uint8_t *p = payload + col * 132 + row / 4 * 6;

			p[0] = (a >> 8) | ((b & 15) << 4);
			p[1] = a;
			p[2] = c;
			p[3] = b >> 4;
			p[4] = d >> 4;
			p[5] = (c >> 8) | ((d & 15) << 4);
		}
	}

	CHECK(gxfp_decode_chicago_hs(payload, sizeof(payload) - 1, &image) == -EINVAL);
	CHECK(gxfp_decode_chicago_hs(payload, sizeof(payload), &image) == 0);
	CHECK(image.rows == 64 && image.cols == 80);
	for (unsigned int row = 0; row < 64; row++)
		for (unsigned int col = 0; col < 80; col++)
			CHECK(image.pixels[row * 80 + col] == value(row, col));
	gxfp_decoded_image_free(&image);

	/* AUTO strips the 5-byte header and the CRC and finds the mode by length. */
	memset(framed, 0, sizeof(framed));
	memcpy(framed + 5, payload, sizeof(payload));
	CHECK(gxfp_decode_image(framed, sizeof(framed), GXFP_DECODE_AUTO, &image) == 0);
	CHECK(image.rows == 64 && image.cols == 80);
	CHECK(image.pixels[63 * 80 + 79] == value(63, 79));
	gxfp_decoded_image_free(&image);

	puts("ChicagoHS column pitch, active crop, all pixel values, AUTO framing, truncation: PASS");
	return 0;
}
