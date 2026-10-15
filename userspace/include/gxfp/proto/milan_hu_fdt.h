#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Experimental six-channel codec shared by ChicagoHS and MilanHU.
 * GXFP51B7's observed chip ID is 0x2504 (ChicagoHS), not MilanHU 0x220f.
 * The historical file/API name does not select a sensor or calibration path.
 * Not a device initialization flow; validate raw bases before applying them.
 */
#define GXFP_HU_FDT_CHANNELS 6
#define GXFP_HU_FDT_PAYLOAD_SIZE 14
#define GXFP_HU_FDT_EVENT_SIZE 16

enum gxfp_hu_fdt_command {
	GXFP_HU_FDT_DOWN = 0x32,
	GXFP_HU_FDT_UP = 0x34,
	GXFP_HU_FDT_MANUAL = 0x36,
};

struct gxfp_hu_fdt_event {
	uint16_t status;
	uint16_t touch_mask;
	uint16_t raw[GXFP_HU_FDT_CHANNELS];
};

struct gxfp_hu_fdt_up_options {
	/* Zero selects the Windows default (0x15). Obtain calibration separately. */
	int16_t delta;
	uint16_t saved_touch_mask;
	/* Windows gfFDTUPbase's second argument: skip untouched-channel fallback. */
	bool use_all_channels;
	/* Windows context+0x5128 == 1: merge masks and retain unsigned minima. */
	bool merge_min;
};

/* Check outer command, length and checksum before parsing the event payload.
 * Unknown status values are preserved; this codec does not advance a flow.
 * Functions return -errno on invalid arguments without changing output.
 */
int gxfp_hu_fdt_parse_event(uint8_t cmd, const uint8_t *payload, size_t len,
			  struct gxfp_hu_fdt_event *out);
int gxfp_hu_fdt_down_base(const uint16_t raw[GXFP_HU_FDT_CHANNELS],
			 uint16_t out[GXFP_HU_FDT_CHANNELS]);
/* previous is required only for merge_min; previous and out may be identical. */
int gxfp_hu_fdt_up_base(const struct gxfp_hu_fdt_event *event,
		       const struct gxfp_hu_fdt_up_options *options,
		       const uint16_t previous[GXFP_HU_FDT_CHANNELS],
		       uint16_t out[GXFP_HU_FDT_CHANNELS]);
/* Encode the one-table (second byte = 1) variant, returning payload length.
 * Feed the result to gxfp_goodix_build_frame(), then add the transport header.
 * For MANUAL the caller supplies the current down base, as Windows does.
 */
int gxfp_hu_fdt_encode(uint8_t cmd, const uint16_t base[GXFP_HU_FDT_CHANNELS],
		       uint8_t *out, size_t capacity);
