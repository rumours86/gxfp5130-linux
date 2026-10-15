/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gxfp51b7_capture: calibrate, wait for one touch and save the image.
 *
 *   gxfp51b7_capture <psk.bin> <out.pgm>
 *
 * Keep the finger off the sensor until READY_FOR_FINGER is printed.
 * The image is written with mode 0600; it is biometric data.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gxfp/flow/chicago51b7.h"

#define POLL_MS 20
#define WAIT_MS 30000

static int wait_transition(struct gxfp51b7 *s, int want_present)
{
	int ret = gxfp51b7_arm(s, want_present);

	if (ret)
		return ret;
	for (int waited = 0; waited < WAIT_MS; waited += POLL_MS) {
		int present = !want_present;

		ret = gxfp51b7_poll(s, &present);
		if (ret < 0)
			return ret;
		if (ret == 1 && present == want_present)
			return 0;
		usleep(POLL_MS * 1000);
	}
	return -ETIMEDOUT;
}

static int write_pgm(const char *path, const struct gxfp_decoded_image *img)
{
	char header[32];
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	FILE *f;
	int bad;

	if (fd < 0)
		return -errno;
	f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		return -errno;
	}
	snprintf(header, sizeof(header), "P5\n%d %d\n255\n", img->cols, img->rows);
	fputs(header, f);
	for (int i = 0; i < img->cols * img->rows; i++)
		fputc(img->pixels[i] * 255u / 4095u, f);
	bad = ferror(f);
	if (fclose(f))
		bad = 1;
	return bad ? -EIO : 0;
}

int main(int argc, char **argv)
{
	struct gxfp51b7 *s = NULL;
	struct gxfp_decoded_image img = { 0 };
	int ret;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <psk.bin> <out.pgm>\n", argv[0]);
		return 2;
	}
	ret = gxfp51b7_open(&s, "/dev/gxfp", argv[1], 1);
	if (ret)
		goto out;
	puts("READY_FOR_FINGER");
	fflush(stdout);
	ret = wait_transition(s, 1);
	if (!ret)
		ret = gxfp51b7_capture(s, &img);
	if (!ret)
		ret = write_pgm(argv[2], &img);
	gxfp_decoded_image_free(&img);
	if (!ret) {
		puts("REMOVE_FINGER");
		fflush(stdout);
		ret = wait_transition(s, 0);
	}
out:
	gxfp51b7_close(s);
	printf("RESULT %d (%s)\n", ret, strerror(ret < 0 ? -ret : 0));
	return ret ? 1 : 0;
}
