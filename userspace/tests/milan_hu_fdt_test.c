#include "gxfp/proto/milan_hu_fdt.h"
#include "gxfp/proto/goodix_proto.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthetic fixtures only. Keep checks enabled in Release builds too. */
#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static void check_words(const uint16_t *got, const uint16_t *expected)
{
	CHECK(memcmp(got, expected, GXFP_HU_FDT_CHANNELS * sizeof(*got)) == 0);
}

static void parse_and_bounds(void)
{
	const uint8_t payload[17] = {
		0x00, 0x02, 0x25, 0x00, 0x34, 0x12, 0xff, 0x00,
		0x00, 0x01, 0x01, 0x02, 0xff, 0xff, 0x00, 0x00, 0xee
	};
	const uint16_t raw[] = {0x1234, 0xff, 0x100, 0x201, 0xffff, 0};
	struct gxfp_hu_fdt_event e, before;
	uint16_t base[6] = {0};
	uint8_t out[14], sentinel[14];

	CHECK(gxfp_hu_fdt_parse_event(0x34, payload, 16, &e) == 0);
	CHECK(e.status == 0x200 && e.touch_mask == 0x25);
	check_words(e.raw, raw);
	memcpy(&before, &e, sizeof(e));
	for (size_t n = 0; n <= 17; n++) {
		if (n == 16)
			continue;
		CHECK(gxfp_hu_fdt_parse_event(0x32, payload, n, &e) == -EINVAL);
		CHECK(memcmp(&e, &before, sizeof(e)) == 0);
	}
	CHECK(gxfp_hu_fdt_parse_event(0xb0, payload, 16, &e) == -EINVAL);
	CHECK(gxfp_hu_fdt_parse_event(0x32, NULL, 16, &e) == -EINVAL);
	CHECK(gxfp_hu_fdt_parse_event(0x32, payload, 16, NULL) == -EINVAL);
	CHECK(memcmp(&e, &before, sizeof(e)) == 0);
	memset(out, 0x5a, sizeof(out));
	memcpy(sentinel, out, sizeof(out));
	CHECK(gxfp_hu_fdt_encode(0x32, base, out, 13) == -ENOSPC);
	CHECK(gxfp_hu_fdt_encode(0x20, base, out, 14) == -EINVAL);
	CHECK(gxfp_hu_fdt_encode(0x32, NULL, out, 14) == -EINVAL);
	CHECK(gxfp_hu_fdt_encode(0x32, base, NULL, 14) == -EINVAL);
	CHECK(memcmp(out, sentinel, sizeof(out)) == 0);
}

static void down_and_wire_format(void)
{
	const uint16_t raw[] = {0, 1, 2, 511, 512, 65535};
	const uint16_t expected[] = {0x0080, 0x0080, 0x0180, 0xff80, 0x0080, 0xff80};
	const uint8_t down_wire[] = {
		0x0c, 1, 0x80, 0, 0x80, 0, 0x80, 1, 0x80, 0xff, 0x80, 0, 0x80, 0xff
	};
	const uint8_t commands[] = {0x32, 0x34, 0x36};
	const uint8_t prefixes[] = {0x0c, 0x0e, 0x0d};
	uint16_t base[6];
	uint8_t payload[15], body[18];
	struct gxfp_frame_parsed f;

	CHECK(gxfp_hu_fdt_down_base(raw, base) == 0);
	check_words(base, expected);
	CHECK(gxfp_hu_fdt_down_base(NULL, base) == -EINVAL);
	CHECK(gxfp_hu_fdt_down_base(raw, NULL) == -EINVAL);
	check_words(base, expected);
	for (size_t i = 0; i < sizeof(commands); i++) {
		memset(payload, 0xcc, sizeof(payload));
		CHECK(gxfp_hu_fdt_encode(commands[i], base, payload, sizeof(payload)) == 14);
		CHECK(payload[0] == prefixes[i] && payload[14] == 0xcc);
		CHECK(memcmp(payload + 1, down_wire + 1, 13) == 0);
		CHECK(gxfp_goodix_build_frame(commands[i], payload, 14, body, sizeof(body)) == 18);
		CHECK(body[1] == 15 && body[2] == 0);
		CHECK(gxfp_parse_goodix_body(body, sizeof(body), &f));
		CHECK(f.valid && f.proto_checksum_ok && f.cmd == commands[i] && f.payload_len == 14);
		CHECK(memcmp(f.payload, payload, 14) == 0);
	}
}

static void up_thresholds(void)
{
	struct gxfp_hu_fdt_event e = {.touch_mask = 0x3f, .raw = {0, 2, 4, 6, 8, 10}};
	struct gxfp_hu_fdt_up_options opt = {0};
	const uint16_t defaults[] = {0x1580, 0x1680, 0x1780, 0x1880, 0x1980, 0x1a80};
	const uint16_t masked[] = {0x1d80, 0x2080, 0x2180, 0x1d80, 0x1d80, 0x2480};
	const uint16_t all[] = {0x1f80, 0x2080, 0x2180, 0x2280, 0x2380, 0x2480};
	uint16_t out[6];

	CHECK(gxfp_hu_fdt_up_base(&e, &opt, NULL, out) == 0);
	check_words(out, defaults);
	opt.delta = 31;
	e.touch_mask = 0x26;
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, NULL, out) == 0);
	check_words(out, masked);
	opt.use_all_channels = true;
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, NULL, out) == 0);
	check_words(out, all);
	opt.delta = INT16_MIN;
	opt.use_all_channels = false;
	e.touch_mask = 0;
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, NULL, out) == 0);
	for (size_t i = 0; i < 6; i++)
		CHECK(out[i] == 0xfe80);
	opt.delta = INT16_MAX;
	opt.use_all_channels = true;
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, NULL, out) == 0);
	CHECK(out[0] == 0xff80 && out[1] == 0x0080 && out[5] == 0x0480);
	opt.delta = 0;
	opt.use_all_channels = false;
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, NULL, out) == 0);
	for (size_t i = 0; i < 6; i++)
		CHECK(out[i] == 0x1380);
}

static void merge_and_errors(void)
{
	struct gxfp_hu_fdt_event e = {.touch_mask = 5, .raw = {10, 20, 30, 40, 50, 60}};
	struct gxfp_hu_fdt_up_options opt = {.delta = 31, .saved_touch_mask = 2, .merge_min = true};
	uint16_t previous[] = {0x2080, 0x3080, 0x2180, 0x1080, 0x3080, 0x3080};
	const uint16_t expected[] = {0x2080, 0x2980, 0x2180, 0x1080, 0x1d80, 0x1d80};
	uint16_t out[6] = {1, 2, 3, 4, 5, 6};
	const uint16_t sentinel[] = {1, 2, 3, 4, 5, 6};

	CHECK(gxfp_hu_fdt_up_base(&e, &opt, NULL, out) == -EINVAL);
	CHECK(gxfp_hu_fdt_up_base(NULL, &opt, previous, out) == -EINVAL);
	CHECK(gxfp_hu_fdt_up_base(&e, NULL, previous, out) == -EINVAL);
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, previous, NULL) == -EINVAL);
	check_words(out, sentinel);
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, previous, out) == 0);
	check_words(out, expected);
	CHECK(gxfp_hu_fdt_up_base(&e, &opt, previous, previous) == 0);
	check_words(previous, expected);
}

int main(void)
{
	parse_and_bounds();
	down_and_wire_format();
	up_thresholds();
	merge_and_errors();
	puts("MilanHU FDT: parsing, boundaries, wire format, thresholds and merge passed");
	return 0;
}
