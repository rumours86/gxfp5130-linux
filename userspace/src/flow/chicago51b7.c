/* SPDX-License-Identifier: GPL-2.0 */
/*
 * GXFP51B7 (ChicagoHS, chip 0x2504) session: sensor init, TLS, finger
 * detection and capture.
 *
 * The sequence follows a Windows capture of the same sensor. The 256-byte
 * config is built from the live OTP as gfspi does (src/algo/chicago_hs_cfg.c).
 * The first MANUAL uses the no-finger DOWN table cached by an earlier open,
 * or a zero table when there is none, as Windows does with goodix.dat.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/platform_util.h>
#include "gxfp/flow/chicago51b7.h"
#include "gxfp/algo/chicago_hs_cfg.h"
#include "gxfp/io/dev.h"
#include "gxfp/proto/goodix_proto.h"
#include "gxfp/proto/milan_hu_fdt.h"
#include "gxfp/algo/image/plain.h"
#include "../tls/tls_engine.h"

struct gxfp51b7 {
	struct gxfp_dev dev;
	struct gxfp_tls_engine *tls;
	FILE *trace;
	uint8_t rx[32768];
	int installed;
	int log;
	int (*cancel)(void *);
	void *cancel_ctx;
	uint8_t seed[14];	/* encoded MANUAL payload, also the DOWN base */
	uint16_t up[6];		/* UP base computed at finger-down */
	uint16_t dark[5120];	/* median of three no-finger frames */
	int delta, noise, waiting, pending_ack;
	uint64_t ack_deadline;
};

static void session_log(struct gxfp51b7 *s, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void session_log(struct gxfp51b7 *s, const char *fmt, ...)
{
	va_list ap;

	if (!s->log)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static uint64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Read exactly len bytes from a regular file without group/other access. */
static int private_read(const char *p, uint8_t *b, size_t len)
{
	struct stat st;
	int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return -errno;
	int ret = 0;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) ||
	    st.st_size != (off_t)len)
		ret = -EINVAL;
	else if (read(fd, b, len) != (ssize_t)len)
		ret = -EIO;
	close(fd);
	return ret;
}

/* Replace p with b, mode 0600; the old file stays intact on failure. */
static int private_write(const char *p, const uint8_t *b, size_t len)
{
	char tmp[4096];
	int fd, ret = 0;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", p) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;
	if (write(fd, b, len) != (ssize_t)len)
		ret = -EIO;
	if (close(fd) && !ret)
		ret = -EIO;
	if (!ret && rename(tmp, p))
		ret = -errno;
	if (ret)
		unlink(tmp);
	return ret;
}

static int trace(struct gxfp51b7 *s, uint8_t dir, const uint8_t *b, size_t n)
{
	if (!s->trace)
		return 0;
	uint8_t h[5] = { dir, n, n >> 8, n >> 16, n >> 24 };

	return fwrite(h, 1, 5, s->trace) == 5 &&
	       fwrite(b, 1, n, s->trace) == n &&
	       !fflush(s->trace) ? 0 : -EIO;
}

static int tls_send(void *ctx, const uint8_t *b, size_t n)
{
	struct gxfp51b7 *s = ctx;
	int ret = gxfp_dev_send_packet(&s->dev, 0xb, b, n);

	if (!ret)
		ret = trace(s, 'S', b, n);
	session_log(s, "TX TLS type=%u bytes=%zu\n", b[0], n);
	return ret;
}

/* Return 1 for an ordinary frame, 0 for TLS, negative error. */
static int receive(struct gxfp51b7 *s, struct gxfp_frame_parsed *f, int timeout)
{
	struct gxfp_tap_hdr h;
	const uint8_t *p;
	size_t n;
	int ret = gxfp_dev_poll_readable(&s->dev, timeout);

	if (ret)
		return ret;
	ssize_t nr = gxfp_dev_read_record(&s->dev, s->rx, sizeof(s->rx), &h, &p, &n);

	if (nr < 0)
		return nr;
	if (h.type == 0xb) {
		session_log(s, "RX TLS type=%u bytes=%zu\n", n ? p[0] : 0, n);
		ret = trace(s, 'C', p, n);
		if (!ret)
			ret = gxfp_tls_engine_feed_cipher_record(s->tls, p, n);
		return ret;
	}
	if (h.type != 0xa || !gxfp_parse_goodix_body(p, n, f) || !f->valid ||
	    f->payload_len + 4u != n || !f->proto_checksum_ok)
		return -EBADMSG;
	session_log(s, "RX cmd=%02x bytes=%u", f->cmd, f->payload_len);
	if (f->cmd == 0xb0 && f->payload_len == 2)
		session_log(s, " ack-for=%02x status=%02x", f->payload[0], f->payload[1]);
	session_log(s, "\n");
	return 1;
}

/*
 * Send exactly one request; require the matching ACK (status 01) plus the
 * response when response_len >= 0. Late DOWN/UP events are skipped. The 5130
 * selectors are not used: they can drop TLS records or accept unrelated ACKs.
 */
static int exchange(struct gxfp51b7 *s, uint8_t cmd, const uint8_t *p, size_t n,
		    int response_len, uint8_t *out)
{
	uint8_t frame[508];
	int len = gxfp_goodix_build_frame(cmd, p, n, frame, sizeof(frame));

	if (len < 0)
		return len;
	session_log(s, "TX cmd=%02x bytes=%zu\n", cmd, n);
	int ret = gxfp_dev_send_packet(&s->dev, 0xa, frame, len);

	if (ret)
		return ret;
	int ack = 0, response = response_len < 0;
	uint64_t end = now_ms() + 2500;

	while (now_ms() < end) {
		if (s->cancel && s->cancel(s->cancel_ctx))
			return -EINTR;
		struct gxfp_frame_parsed f;

		ret = receive(s, &f, 100);
		if (ret == -EAGAIN || ret == 0)
			continue;
		if (ret < 0)
			return ret;
		if (f.cmd == 0xb0) {
			if (f.payload_len != 2 || f.payload[1] != 1)
				return -EPROTO;
			if (f.payload[0] != cmd) {
				if (f.payload[0] == 0x32 || f.payload[0] == 0x34)
					continue;
				return -EPROTO;
			}
			ack = 1;
		} else if (f.cmd == cmd) {
			if (response_len < 0 || f.payload_len != (unsigned int)response_len)
				return -EPROTO;
			if (out)
				memcpy(out, f.payload, response_len);
			response = 1;
		} else if (f.cmd == 0x32 || f.cmd == 0x34) {
			continue;
		} else {
			return -EPROTO;
		}
		if (ack && response)
			return 0;
	}
	return -ETIMEDOUT;
}

static int result01(const uint8_t *b)
{
	return b[0] == 1 && b[1] == 1 ? 0 : -EPROTO;
}

static int init_sensor(struct gxfp51b7 *s, struct gxfp_chicago_hs_calibration *cal)
{
	const uint8_t installed[2] = { 1, 0 }, zero[2] = { 0 }, reset[2] = { 5, 20 };
	const uint8_t reg0[5] = { 0, 0, 0, 4, 0 }, idle[2] = { 20, 0 };
	const uint8_t timing[8] = { 0x88, 0x0c, 0xc8, 0, 0xc8, 0, 0xc8, 0 };
	uint8_t reply[64], config[GXFP_CHICAGO_HS_CFG_LEN];
	int ret;

	/* 0x96: tell the firmware a host driver is installed (sent twice). */
	ret = exchange(s, 0x96, installed, 2, -1, NULL);
	if (ret)
		return ret;
	s->installed = 1;
	ret = exchange(s, 0x96, installed, 2, -1, NULL);
	if (ret)
		return ret;
	/* 0xa8: firmware version; only this firmware is known to work. */
	ret = exchange(s, 0xa8, zero, 2, 19, reply);
	if (ret)
		return ret;
	if (memcmp(reply, "GF_9ELIBE_EC_19024", sizeof("GF_9ELIBE_EC_19024")))
		return -EPROTO;
	/* 0xa2: sensor reset. */
	ret = exchange(s, 0xa2, reset, 2, 3, reply);
	if (ret)
		return ret;
	if (reply[0] != 1)
		return -EPROTO;
	usleep(200000);
	/* 0x82: register 0 holds the chip ID. */
	ret = exchange(s, 0x82, reg0, 5, 4, reply);
	if (ret)
		return ret;
	/* Wire pair-swap: [04 a2 00 25] -> [a2 04 25 00], chip 2504. */
	if (reply[0] != 4 || reply[2] != 0 || reply[3] != 0x25)
		return -EPROTO;
	session_log(s, "%s\n", "Live chip identity: ChicagoHS 2504");
	/* 0xa6: OTP; the config and the FDT delta come from it. */
	ret = exchange(s, 0xa6, zero, 2, 64, reply);
	if (ret)
		return ret;
	ret = gxfp_chicago_hs_build_config(reply, config, cal);
	mbedtls_platform_zeroize(reply, sizeof(reply));
	if (ret)
		return ret;
	/* Windows only logs a CRC mismatch and carries on; so does this. */
	session_log(s, "OTP CRC %s; %s tcode 0x%x, FDT delta %u, FDT offset %u\n",
		    cal->crc_ok ? "ok" : "MISMATCH",
		    cal->has_tcode ? "calibrated" : "default", cal->tcode,
		    cal->fdt_delta, cal->fdt_offset);
	ret = exchange(s, 0xa2, reset, 2, 3, reply);
	if (ret)
		return ret;
	if (reply[0] != 1)
		return -EPROTO;
	usleep(200000);
	/* 0x70 idle, 0x98 timing, 0x90 config; volatile, nothing is flashed. */
	ret = exchange(s, 0x70, idle, 2, -1, NULL);
	if (ret)
		return ret;
	ret = exchange(s, 0x98, timing, 8, 2, reply);
	if (ret)
		return ret;
	ret = result01(reply);
	if (ret)
		return ret;
	ret = exchange(s, 0x90, config, sizeof(config), 2, reply);
	if (ret)
		return ret;
	ret = result01(reply);
	if (ret)
		return ret;
	ret = exchange(s, 0x70, idle, 2, -1, NULL);
	if (ret)
		return ret;
	ret = exchange(s, 0x98, timing, 8, 2, reply);
	if (ret)
		return ret;
	ret = result01(reply);
	if (ret)
		return ret;
	session_log(s, "%s\n", "SENSOR_INITIALIZED: config accepted");
	return 0;
}

static int start_tls(struct gxfp51b7 *s)
{
	const uint8_t zero[2] = { 0 }, state_req = 0x55;
	uint8_t state[10];
	/* 0xd0 starts the handshake; the sensor is the TLS client. */
	int ret = exchange(s, 0xd0, zero, 2, -1, NULL);

	if (ret)
		return ret;
	uint64_t end = now_ms() + 15000;

	for (;;) {
		if (s->cancel && s->cancel(s->cancel_ctx))
			return -EINTR;
		ret = gxfp_tls_engine_handshake_step(s->tls);
		if (ret == 1)
			break;
		if (ret != -EAGAIN)
			return ret;
		if (now_ms() >= end)
			return -ETIMEDOUT;
		struct gxfp_frame_parsed f;

		ret = receive(s, &f, 100);
		if (ret < 0 && ret != -EAGAIN)
			return ret;
		if (ret == 1)
			return -EPROTO;
	}
	/* 0xd4 ends the handshake; 0xae state byte 7 = 1 means connected. */
	ret = exchange(s, 0xd4, zero, 2, -1, NULL);
	if (!ret)
		ret = exchange(s, 0xae, &state_req, 1, 10, state);
	if (!ret && state[7] != 1)
		ret = -EPROTO;
	if (!ret)
		session_log(s, "%s\n", "TLS_AUTHENTICATED: EC state confirms connection");
	return ret;
}

/*
 * 0x36 MANUAL: read the six FDT channels with no finger and turn them into
 * the next DOWN base. -EBUSY means a finger is on the sensor.
 */
static int manual(struct gxfp51b7 *s, uint8_t seed[14])
{
	uint8_t reply[16];
	int ret = exchange(s, 0x36, seed, 14, 16, reply);

	if (ret)
		return ret;
	struct gxfp_hu_fdt_event event;

	ret = gxfp_hu_fdt_parse_event(0x36, reply, sizeof(reply), &event);
	if (ret || event.status != 0x100)
		return ret ? ret : -EPROTO;
	if (event.touch_mask)
		return -EBUSY;
	for (unsigned int i = 0; i < 6; i++)
		if (((event.raw[i] >> 1) & 255) == 0 || ((event.raw[i] >> 1) & 255) == 255)
			return -ERANGE;
	uint16_t base[6];

	ret = gxfp_hu_fdt_down_base(event.raw, base);
	if (!ret)
		ret = gxfp_hu_fdt_encode(0x36, base, seed, 14);
	return ret == 14 ? 0 : (ret < 0 ? ret : -EPROTO);
}

/*
 * Calibration needs the sensor bare. A finger that is already there when a
 * verification starts (a touch to wake the lock screen, or one put down
 * before the prompt) used to fail the open at once with -EBUSY; wait up to
 * 4 s for it to be lifted instead. Right after the touch mask clears the
 * frames still carry the finger (noise99 20..64 against 11..12), so once a
 * touch was seen the mask must stay clear for 0.5 s.
 */
static int manual_bare(struct gxfp51b7 *s)
{
	const struct timespec pause = { 0, 100 * 1000 * 1000 };
	const uint64_t end = now_ms() + 4000;
	unsigned int clear = 0, settle = 0;
	int ret;

	for (;;) {
		ret = manual(s, s->seed);
		if (ret == -EBUSY) {
			if (!settle)
				session_log(s, "%s\n", "51B7 calibration: finger on the sensor, waiting for it to lift");
			settle = 5;
			clear = 0;
		} else if (ret || ++clear > settle) {
			return ret;
		}
		if (now_ms() >= end)
			return ret;
		nanosleep(&pause, NULL);
	}
}

/*
 * The first MANUAL carries the current DOWN table. Windows sends the one an
 * earlier session cached in goodix.dat; without that file the table is
 * still all zero (gf_update_all_base, 5c738). This is the no-cache case:
 * keep only the readings, a touch mask against a zero table means nothing.
 * A finger already resting on the sensor goes unnoticed here, which is why
 * the bare table is cached for the next open.
 */
static int manual_bootstrap(struct gxfp51b7 *s)
{
	const uint16_t zero[6] = { 0 };
	uint8_t seed[14], reply[16];
	struct gxfp_hu_fdt_event event;
	uint16_t base[6];
	int ret = gxfp_hu_fdt_encode(0x36, zero, seed, sizeof(seed));

	if (ret < 0)
		return ret;
	ret = exchange(s, 0x36, seed, sizeof(seed), 16, reply);
	if (ret)
		return ret;
	ret = gxfp_hu_fdt_parse_event(0x36, reply, sizeof(reply), &event);
	if (ret || event.status != 0x100)
		return ret ? ret : -EPROTO;
	for (unsigned int i = 0; i < 6; i++)
		if (((event.raw[i] >> 1) & 255) == 0 || ((event.raw[i] >> 1) & 255) == 255)
			return -ERANGE;
	session_log(s, "MANUAL from a zero table: touch mask 0x%x (ignored)\n",
		    event.touch_mask);
	ret = gxfp_hu_fdt_down_base(event.raw, base);
	if (!ret)
		ret = gxfp_hu_fdt_encode(0x36, base, s->seed, sizeof(s->seed));
	return ret == 14 ? 0 : (ret < 0 ? ret : -EPROTO);
}

/*
 * 0x82: register 0082 holds the FDT delta in its high byte once a MANUAL has
 * applied the FDT section of the config; Windows reads it after two. A
 * match shows the generated config is the live one.
 * Like the chip ID the reply is pair-swapped, so the high byte comes first.
 */
static int check_fdt_delta(struct gxfp51b7 *s, uint8_t delta)
{
	const uint8_t req[5] = { 0, 0x82, 0, 2, 0 };
	uint8_t reply[2];
	int ret = exchange(s, 0x82, req, sizeof(req), 2, reply);

	if (ret)
		return ret;
	session_log(s, "Register 0082 high byte %u, config %u\n", reply[0], delta);
	return reply[0] == delta ? 0 : -EPROTO;
}

/* A cached MANUAL payload: header 0d 01, no channel at either end of the range. */
static int seed_usable(const uint8_t seed[14])
{
	if (seed[0] != 0x0d || seed[1] != 1)
		return 0;
	for (unsigned int i = 0; i < 6; i++)
		if (seed[3 + 2 * i] == 0 || seed[3 + 2 * i] == 255)
			return 0;
	return 1;
}

/* 0x20 GET_IMAGE: one frame over TLS, decoded to 80x64 12-bit samples. */
static int raw_capture(struct gxfp51b7 *s, struct gxfp_decoded_image *out)
{
	int ret;
	const uint8_t image_req[2] = { 1, 0 };

	ret = exchange(s, 0x20, image_req, 2, -1, NULL);
	if (ret)
		return ret;
	uint8_t plain[16384];
	size_t n;
	uint64_t end = now_ms() + 5000;

	for (;;) {
		if (s->cancel && s->cancel(s->cancel_ctx))
			return -EINTR;
		ret = gxfp_tls_engine_pull_plain(s->tls, plain, sizeof(plain), &n);
		if (ret == 1)
			break;
		if (ret != -EAGAIN)
			return ret;
		if (now_ms() >= end)
			return -ETIMEDOUT;
		struct gxfp_frame_parsed f;

		ret = receive(s, &f, 100);
		if (ret < 0 && ret != -EAGAIN)
			return ret;
		if (ret == 1)
			return -EPROTO;
	}
	struct gxfp_frame_parsed f;
	struct gxfp_plain_image_sink sink = { 0 };

	/* 0x88 is the firmware's "checksum disabled" marker. */
	if (!gxfp_parse_goodix_body(plain, n, &f) || !f.valid || f.cmd != 0x20 ||
	    f.payload_len + 4u != n ||
	    (!f.proto_checksum_ok && f.proto_checksum != 0x88) ||
	    f.payload_len != 10569 || f.payload[0] == 0xaa) {
		ret = -EBADMSG;
		goto out;
	}
	ret = gxfp_plain_image_sink_append(&sink, f.payload, f.payload_len);
	if (!ret && !sink.have_oneframe)
		ret = -EBADMSG;
	/* Payload: 5-byte header, 10560 bytes of samples, CRC32. */
	if (!ret)
		ret = gxfp_decode_chicago_hs(f.payload + 5, sink.body_len - 4, out);
out:
	gxfp_plain_image_sink_reset(&sink);
	mbedtls_platform_zeroize(plain, sizeof(plain));
	return ret;
}

static int send_fdt(struct gxfp51b7 *s, uint8_t cmd, const uint16_t base[6])
{
	uint8_t payload[14], frame[18];
	int n = gxfp_hu_fdt_encode(cmd, base, payload, sizeof(payload));

	if (n < 0)
		return n;
	n = gxfp_goodix_build_frame(cmd, payload, n, frame, sizeof(frame));
	if (n < 0)
		return n;
	int ret = gxfp_dev_send_packet(&s->dev, 0xa, frame, n);

	if (!ret) {
		s->pending_ack = cmd;
		s->ack_deadline = now_ms() + 2500;
	}
	return ret;
}

void gxfp51b7_set_cancel(struct gxfp51b7 *s, int (*cancel)(void *), void *ctx)
{
	if (s) {
		s->cancel = cancel;
		s->cancel_ctx = ctx;
	}
}

int gxfp51b7_stop(struct gxfp51b7 *s)
{
	if (!s)
		return -EINVAL;
	const uint8_t idle[2] = { 20, 0 };
	int ret = exchange(s, 0x70, idle, 2, -1, NULL);

	s->pending_ack = 0;
	s->waiting = 0;
	return ret;
}

void gxfp51b7_close(struct gxfp51b7 *s)
{
	if (!s)
		return;
	s->cancel = NULL;
	if (s->installed && s->dev.fd >= 0) {
		const uint8_t zero[2] = { 0 };

		(void)gxfp51b7_stop(s);
		/* 0x96 00 00: host driver going away. */
		(void)exchange(s, 0x96, zero, 2, -1, NULL);
	}
	gxfp_tls_engine_free(s->tls);
	gxfp_dev_close(&s->dev);
	mbedtls_platform_zeroize(s, sizeof(*s));
	free(s);
}

int gxfp51b7_open(struct gxfp51b7 **out, const char *dev, const char *keypath,
		  const char *fdt_cache, int log)
{
	if (!out || !dev || !keypath)
		return -EINVAL;
	uint8_t key[48] = { 0 };
	struct gxfp_chicago_hs_calibration cal;
	uint8_t cached[14] = { 0 };
	int have_cache = 0;
	struct gxfp51b7 *s = calloc(1, sizeof(*s));

	if (!s)
		return -ENOMEM;
	*out = NULL;
	s->dev.fd = -1;
	s->log = log;
	int ret = private_read(keypath, key, 48);

	if (ret)
		goto fail;
	if (fdt_cache && !private_read(fdt_cache, cached, sizeof(cached)) &&
	    seed_usable(cached)) {
		memcpy(s->seed, cached, sizeof(cached));
		have_cache = 1;
	}
	ret = gxfp_tls_engine_create(&s->tls, key, 48, tls_send, s);
	mbedtls_platform_zeroize(key, 48);
	if (ret)
		goto fail;
	ret = gxfp_dev_open(&s->dev, dev, O_RDWR | O_NONBLOCK);
	if (!ret)
		ret = gxfp_dev_flush_rxq(&s->dev);
	if (!ret)
		ret = init_sensor(s, &cal);
	if (!ret)
		ret = start_tls(s);
	if (!ret && !have_cache)
		ret = manual_bootstrap(s);
	if (ret)
		goto fail;
	/* FDT UP margin; the template default when the OTP had no tcode. */
	s->delta = cal.fdt_delta;
	/*
	 * MANUAL reports the current mask; initial UP only reports transitions
	 * and cannot serve as an absence query before a DOWN has happened.
	 */
	struct gxfp_decoded_image frames[3] = { { 0 } };

	for (unsigned int k = 0; k < 3; k++) {
		ret = manual_bare(s);
		if (!ret)
			ret = raw_capture(s, &frames[k]);
		if (ret)
			break;
	}
	if (!ret) {
		/* Background = per-pixel median; noise = 99th percentile of |frame - median|. */
		unsigned int hist[4096] = { 0 };

		for (unsigned int i = 0; i < 5120; i++) {
			uint16_t a = frames[0].pixels[i], b = frames[1].pixels[i];
			uint16_t c = frames[2].pixels[i];
			uint16_t lo = a < b ? a : b;

			lo = lo < c ? lo : c;
			uint16_t hi = a > b ? a : b;

			hi = hi > c ? hi : c;
			s->dark[i] = a + b + c - lo - hi;
			for (unsigned int k = 0; k < 3; k++)
				hist[abs((int)frames[k].pixels[i] - s->dark[i])]++;
		}
		unsigned int count = 0;

		for (s->noise = 0; s->noise < 4095; s->noise++) {
			count += hist[s->noise];
			if (count >= 15207)
				break;
		}
		if (s->noise < 2)
			s->noise = 2;
		ret = manual_bare(s);
		/* After a MANUAL, as Windows: before one it read 40 for a config of 31. */
		if (!ret)
			ret = check_fdt_delta(s, cal.fdt_delta);
		session_log(s, "51B7 calibration: 3 background frames; noise99=%d delta=%d (%s)\n",
			    s->noise, s->delta, have_cache ? "cached FDT base" : "zero-table start");
	}
	for (unsigned int k = 0; k < 3; k++)
		gxfp_decoded_image_free(&frames[k]);
	/*
	 * A touch against the cached table is a finger or a stale cache; drop
	 * the cache so that the next open starts over from a zero table.
	 */
	if (ret == -EBUSY && have_cache)
		unlink(fdt_cache);
	if (ret)
		goto fail;
	/*
	 * Cache the bare table. Without a cache nothing proved the sensor bare,
	 * so require background noise of a bare sensor (11..13 here; a finger
	 * lifted just before calibration gave 20..64).
	 */
	if (fdt_cache && (have_cache || s->noise < 20) &&
	    memcmp(cached, s->seed, sizeof(cached))) {
		int err = private_write(fdt_cache, s->seed, sizeof(s->seed));

		if (err)
			session_log(s, "FDT base cache not written: %d\n", err);
	}
	*out = s;
	return 0;
fail:
	mbedtls_platform_zeroize(key, 48);
	gxfp51b7_close(s);
	return ret;
}

/* Wait for a finger (present != 0) or for its removal. */
int gxfp51b7_arm(struct gxfp51b7 *s, int present)
{
	if (!s)
		return -EINVAL;
	s->waiting = present ? 1 : 2;
	uint16_t down[6];

	for (unsigned int i = 0; i < 6; i++)
		down[i] = s->seed[2 + i * 2] | ((uint16_t)s->seed[3 + i * 2] << 8);
	return send_fdt(s, present ? 0x32 : 0x34, present ? down : s->up);
}

int gxfp51b7_poll(struct gxfp51b7 *s, int *present)
{
	struct gxfp_frame_parsed f;
	int ret = receive(s, &f, 0);

	if (ret == -EAGAIN) {
		if (s->pending_ack && now_ms() > s->ack_deadline)
			return -ETIMEDOUT;
		return 0;
	}
	if (ret <= 0)
		return ret;
	if (f.cmd == 0xb0) {
		if (f.payload_len != 2 || f.payload[1] != 1 || f.payload[0] != s->pending_ack)
			return -EPROTO;
		s->pending_ack = 0;
		return 0;
	}
	struct gxfp_hu_fdt_event e;

	ret = gxfp_hu_fdt_parse_event(f.cmd, f.payload, f.payload_len, &e);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < 6; i++)
		if (((e.raw[i] >> 1) & 255) == 0 || ((e.raw[i] >> 1) & 255) == 255)
			return -ERANGE;
	/* DOWN fired: remember the UP base for this contact. */
	if (e.status == 2 && s->waiting == 1 && e.touch_mask) {
		struct gxfp_hu_fdt_up_options options = { .delta = s->delta };

		ret = gxfp_hu_fdt_up_base(&e, &options, NULL, s->up);
		if (ret)
			return ret;
		s->waiting = 0;
		*present = 1;
		return 1;
	}
	/* UP fired: the reported levels become the next DOWN base. */
	if (e.status == 0x200 && s->waiting == 2) {
		uint16_t base[6];

		gxfp_hu_fdt_down_base(e.raw, base);
		gxfp_hu_fdt_encode(0x36, base, s->seed, 14);
		s->waiting = 0;
		*present = 0;
		return 1;
	}
	/* Baseline drifted while waiting for DOWN: re-arm with fresh levels. */
	if ((e.status == 0x80 || e.status == 0x82) && s->waiting == 1) {
		uint16_t base[6];

		gxfp_hu_fdt_down_base(e.raw, base);
		gxfp_hu_fdt_encode(0x36, base, s->seed, 14);
		return send_fdt(s, 0x32, base);
	}
	return -EPROTO;
}

int gxfp51b7_capture(struct gxfp51b7 *s, struct gxfp_decoded_image *out)
{
	/*
	 * DOWN can precede full skin contact. Keep cancellation responsive while
	 * settling, then suppress independent read noise with three exposures.
	 */
	for (unsigned int k = 0; k < 20; k++) {
		if (s->cancel && s->cancel(s->cancel_ctx))
			return -EINTR;
		usleep(10000);
	}
	struct gxfp_decoded_image frames[3] = { { 0 } };
	int ret = 0;

	for (unsigned int k = 0; k < 3; k++) {
		ret = raw_capture(s, &frames[k]);
		if (ret)
			break;
	}
	if (!ret) {
		*out = frames[0];
		memset(&frames[0], 0, sizeof(frames[0]));
		for (unsigned int i = 0; i < 5120; i++) {
			uint16_t a = out->pixels[i], b = frames[1].pixels[i];
			uint16_t c = frames[2].pixels[i];
			uint16_t lo = a < b ? a : b;

			lo = lo < c ? lo : c;
			uint16_t hi = a > b ? a : b;

			hi = hi > c ? hi : c;
			out->pixels[i] = a + b + c - lo - hi;
		}
	}
	for (unsigned int k = 0; k < 3; k++)
		gxfp_decoded_image_free(&frames[k]);
	if (ret)
		return ret;

	/* Subtract the background, then stretch the 2nd..98th percentile. */
	unsigned int hist[4096] = { 0 };

	for (unsigned int i = 0; i < 5120; i++) {
		int v = (int)s->dark[i] - out->pixels[i];

		if (v < 0)
			v = 0;
		out->pixels[i] = v;
		hist[v]++;
	}
	unsigned int low = 0, high = 4095, count = 0;

	for (low = 0; low < 4095; low++) {
		count += hist[low];
		if (count >= 102)
			break;
	}
	count = 0;
	for (high = 0; high < 4095; high++) {
		count += hist[high];
		if (count >= 5018)
			break;
	}
	session_log(s, "51B7 image: median3 contrast=%u noise99=%d\n", high - low, s->noise);
	/* Too flat to be a finger: let libfprint ask for another touch. */
	if (high <= low || (int)(high - low) < s->noise * 4) {
		gxfp_decoded_image_free(out);
		return -ENODATA;
	}
	for (unsigned int i = 0; i < 5120; i++) {
		int v = (int)out->pixels[i] - low;

		if (v < 0)
			v = 0;
		unsigned int norm = (unsigned int)v * 4095 / (high - low);

		out->pixels[i] = norm > 4095 ? 4095 : norm;
	}
	return 0;
}
