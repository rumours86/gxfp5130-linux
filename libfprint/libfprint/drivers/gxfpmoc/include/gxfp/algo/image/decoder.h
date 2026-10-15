#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum gxfp_decode_mode {
	GXFP_DECODE_AUTO = 0,
	GXFP_DECODE_MILANG_RAW,
	GXFP_DECODE_MILANG_NAV,
	GXFP_DECODE_CHICAGOH_RAW,
	GXFP_DECODE_CHICAGOH_NAV,
	GXFP_DECODE_CHICAGOH_RAW_U16LE,
	GXFP_DECODE_CHICAGOH_NAV_U16LE,
	GXFP_DECODE_CHICAGOHS_RAW,
};

struct gxfp_decoded_image {
	uint16_t *pixels;
	int rows;
	int cols;
};

struct gxfp_decode_auto_result {
	enum gxfp_decode_mode mode;
	const uint8_t *payload;
	size_t payload_len;
};

int gxfp_decode_auto_detect(const uint8_t *buf, size_t len,
			     struct gxfp_decode_auto_result *res);

int gxfp_decode_milan(const uint8_t *payload, size_t payload_len,
		      int rows, int cols, struct gxfp_decoded_image *out);

int gxfp_decode_chicago(const uint8_t *payload, size_t payload_len,
			int rows, int cols, int is_u16le,
			struct gxfp_decoded_image *out);

/* 2504/ChicagoHS: 80 columns, 64 active rows; packed column pitch 88 rows.
 * Exactly 10560 wire bytes, 12-bit sample packing; discard 24 padding rows. */
int gxfp_decode_chicago_hs(const uint8_t *payload, size_t payload_len,
			   struct gxfp_decoded_image *out);

int gxfp_decode_image(const uint8_t *buf, size_t len, enum gxfp_decode_mode mode,
		      struct gxfp_decoded_image *out);

void gxfp_decoded_image_free(struct gxfp_decoded_image *img);

#ifdef __cplusplus
}
#endif
