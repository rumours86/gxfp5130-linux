#include "gxfp/proto/milan_hu_fdt.h"

#include <errno.h>

static bool fdt_command(uint8_t cmd)
{
	return cmd == GXFP_HU_FDT_DOWN || cmd == GXFP_HU_FDT_UP ||
	       cmd == GXFP_HU_FDT_MANUAL;
}

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t encode_threshold(int32_t value)
{
	/* Match the DLL's 16-bit truncation without signed overflow/left shifts. */
	return ((uint16_t)(uint8_t)value << 8) | 0x80u;
}

int gxfp_hu_fdt_parse_event(uint8_t cmd, const uint8_t *payload, size_t len,
			  struct gxfp_hu_fdt_event *out)
{
	if (!fdt_command(cmd) || !payload || !out || len != GXFP_HU_FDT_EVENT_SIZE)
		return -EINVAL;

	out->status = read_le16(payload);
	out->touch_mask = read_le16(payload + 2);
	for (size_t i = 0; i < GXFP_HU_FDT_CHANNELS; i++)
		out->raw[i] = read_le16(payload + 4 + 2 * i);
	return 0;
}

int gxfp_hu_fdt_down_base(const uint16_t raw[GXFP_HU_FDT_CHANNELS],
			 uint16_t out[GXFP_HU_FDT_CHANNELS])
{
	if (!raw || !out)
		return -EINVAL;
	for (size_t i = 0; i < GXFP_HU_FDT_CHANNELS; i++)
		out[i] = encode_threshold(raw[i] >> 1);
	return 0;
}

int gxfp_hu_fdt_up_base(const struct gxfp_hu_fdt_event *event,
		       const struct gxfp_hu_fdt_up_options *options,
		       const uint16_t previous[GXFP_HU_FDT_CHANNELS],
		       uint16_t out[GXFP_HU_FDT_CHANNELS])
{
	int32_t delta;
	uint16_t mask;

	if (!event || !options || !out || (options->merge_min && !previous))
		return -EINVAL;
	delta = options->delta ? options->delta : 0x15;
	mask = event->touch_mask;
	if (options->merge_min)
		mask |= options->saved_touch_mask;
	for (size_t i = 0; i < GXFP_HU_FDT_CHANNELS; i++) {
		int32_t value = (event->raw[i] >> 1) + delta;
		uint16_t encoded;

		if (!options->use_all_channels && !(mask & (1u << i)))
			value = delta - 2;
		encoded = encode_threshold(value);
		out[i] = options->merge_min && previous[i] < encoded ? previous[i] : encoded;
	}
	return 0;
}

int gxfp_hu_fdt_encode(uint8_t cmd, const uint16_t base[GXFP_HU_FDT_CHANNELS],
		       uint8_t *out, size_t capacity)
{
	if (!fdt_command(cmd) || !base || !out)
		return -EINVAL;
	if (capacity < GXFP_HU_FDT_PAYLOAD_SIZE)
		return -ENOSPC;

	out[0] = cmd == GXFP_HU_FDT_DOWN ? 0x0c : cmd == GXFP_HU_FDT_UP ? 0x0e : 0x0d;
	out[1] = 1;
	for (size_t i = 0; i < GXFP_HU_FDT_CHANNELS; i++) {
		out[2 + 2 * i] = (uint8_t)base[i];
		out[3 + 2 * i] = (uint8_t)(base[i] >> 8);
	}
	return GXFP_HU_FDT_PAYLOAD_SIZE;
}
