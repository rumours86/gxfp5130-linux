/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Goodix GXFP51B7 (ChicagoHS) press sensor behind the eSPI /dev/gxfp
 * transport. Experimental: verified on Huawei MACHC-WAX9 with BIOS 1.26.
 *
 * All sensor I/O except finger-event polling runs in a worker thread
 * (src/flow/chicago51b7.c blocks). Only one job runs at a time; a job
 * requested while another runs cancels it and starts afterwards.
 *
 * Optional environment:
 *   FP_GXFP51B7_PSK            48-byte PSK file (default below)
 *   FP_GXFP51B7_MATERIAL       directory with otp.bin, config.bin and
 *                              manual-seed.bin for this sensor
 *   FP_GXFP51B7_STATUS         file to write the finger prompt state to,
 *                              for test UIs
 *   FP_GXFP51B7_DIAGNOSTIC_DIR save every captured frame there as a 0600
 *                              PGM (biometric data, debugging only)
 */
#define FP_COMPONENT "gxfp51b7"

#include "drivers_api.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "gxfp/flow/chicago51b7.h"

#define GXFP51B7_DEFAULT_PSK_PATH      "/var/lib/fprint/gxfp51b7/psk.bin"
#define GXFP51B7_DEFAULT_MATERIAL_PATH "/var/lib/fprint/gxfp51b7"
#define GXFP51B7_POLL_MS               20

/*
 * Ridge score threshold (see sigfm_ridge_scores()). SIGFM alone almost never
 * matches separate touches on this 80x64 sensor. One user, one 16-frame
 * enrollment of the right index finger: 20 of 24 ordinary touches matched
 * and 25 of 38 off-centre or older ones, scoring 13.7..58. The misses found
 * no true counterpart in the enrolled frames with any method tried. 158
 * impostor attempts (left index, other fingers) scored at most 1.9: none
 * reached 2 on both scores. Family members' fingers, 18 touches against two
 * enrolled fingers, scored at most 5.3. Of 17384 non-overlapping pairs of
 * 80x64 crops of another sensor's print, one reached 9.2. The false accept
 * rate is an estimate, not a measurement: see docs/gxfp51b7.md.
 */
#define GXFP51B7_RIDGE_THRESHOLD 12.0

/*
 * Enrollment. In the first 16-stage enrollment, 8 of the 16 frames nearly
 * repeated another (score 45..67): half the touches added nothing. Replaying
 * the saved touches in order and refusing frames that score >= 40 against an
 * accepted one kept 16 distinct frames and raised later sessions' matching
 * touches from 32 to 37 of 48, as many as 24..32 frames without refusing
 * (36..38). More frames only raise the false accept rate, so 16 stay.
 */
#define GXFP51B7_ENROLL_STAGES 16
#define GXFP51B7_RIDGE_DUPLICATE 40.0

typedef enum {
  JOB_NONE,
  JOB_OPEN,
  JOB_CAPTURE,
  JOB_STOP,
  JOB_CLOSE,
} Job;

struct _FpiDeviceGxfp51b7
{
  FpImageDevice     parent;

  struct gxfp51b7  *session;
  GCancellable     *worker_cancel;
  GSource          *timer;
  gboolean          busy;
  Job               pending;

  gchar            *dev_path;
  gchar            *key_path;
  gchar            *material_path;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGxfp51b7, fpi_device_gxfp51b7, FPI, DEVICE_GXFP51B7, FpImageDevice)
G_DEFINE_TYPE (FpiDeviceGxfp51b7, fpi_device_gxfp51b7, FP_TYPE_IMAGE_DEVICE)

static void start_job (FpiDeviceGxfp51b7 *self,
                       Job                job);

static void
write_status (const char *state)
{
  const char *path = g_getenv ("FP_GXFP51B7_STATUS");

  if (path && g_file_set_contents (path, state, -1, NULL))
    g_chmod (path, 0644);
}

static void
stop_timer (FpiDeviceGxfp51b7 *self)
{
  if (self->timer)
    {
      g_source_destroy (self->timer);
      self->timer = NULL;
    }
}

static int
worker_cancelled (void *ctx)
{
  return g_cancellable_is_cancelled (ctx);
}

static void
image_free (gpointer p)
{
  struct gxfp_decoded_image *img = p;

  gxfp_decoded_image_free (img);
  g_free (img);
}

static void
worker (GTask        *task,
        gpointer      source,
        gpointer      data,
        GCancellable *cancel)
{
  FpiDeviceGxfp51b7 *self = source;
  Job job = GPOINTER_TO_INT (data);
  struct gxfp_decoded_image *img = NULL;
  int ret = 0;

  switch (job)
    {
    case JOB_OPEN:
      ret = gxfp51b7_open (&self->session, self->dev_path, self->key_path,
                           self->material_path, g_getenv ("FP_GXFP_LOG") != NULL);
      break;

    case JOB_CAPTURE:
      img = g_new0 (struct gxfp_decoded_image, 1);
      gxfp51b7_set_cancel (self->session, worker_cancelled, cancel);
      ret = gxfp51b7_capture (self->session, img);
      gxfp51b7_set_cancel (self->session, NULL, NULL);
      break;

    case JOB_STOP:
      ret = gxfp51b7_stop (self->session);
      break;

    case JOB_CLOSE:
      gxfp51b7_close (self->session);
      self->session = NULL;
      break;

    case JOB_NONE:
      break;
    }

  if (ret)
    {
      if (img)
        image_free (img);
      if (ret == -ENODATA)
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "51B7 image contrast below calibrated noise");
      else
        g_task_return_new_error (task, G_IO_ERROR, g_io_error_from_errno (-ret),
                                 "51B7 operation failed: %s (%d)",
                                 g_strerror (-ret), ret);
    }
  else if (img)
    {
      g_task_return_pointer (task, img, image_free);
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }
}

/* Opt-in debugging aid. Created 0600 from the start: this is a fingerprint. */
static void
dump_frame (FpImage *image)
{
  const char *dir = g_getenv ("FP_GXFP51B7_DIAGNOSTIC_DIR");
  g_autofree gchar *path = NULL;
  g_autofree gchar *header = NULL;
  gsize len = (gsize) image->width * image->height;
  int fd;

  if (!dir)
    return;

  path = g_strdup_printf ("%s/frame-%" G_GINT64_FORMAT ".pgm", dir, g_get_real_time ());
  header = g_strdup_printf ("P5\n%d %d\n255\n", image->width, image->height);
  fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    {
      fp_warn ("cannot create %s: %s", path, g_strerror (errno));
      return;
    }
  if (write (fd, header, strlen (header)) != (gssize) strlen (header) ||
      write (fd, image->data, len) != (gssize) len)
    fp_warn ("short write to %s", path);
  close (fd);
}

static void
job_done (GObject      *obj,
          GAsyncResult *result,
          gpointer      ctx)
{
  FpiDeviceGxfp51b7 *self = FPI_DEVICE_GXFP51B7 (obj);
  FpImageDevice *dev = FP_IMAGE_DEVICE (self);
  Job job = GPOINTER_TO_INT (ctx);
  GError *error = NULL;
  struct gxfp_decoded_image *img = NULL;
  FpImage *image;

  if (job == JOB_CAPTURE)
    img = g_task_propagate_pointer (G_TASK (result), &error);
  else
    g_task_propagate_boolean (G_TASK (result), &error);

  self->busy = FALSE;
  g_clear_object (&self->worker_cancel);

  /* A newer request cancelled this job; drop its result and run that one. */
  if (self->pending != JOB_NONE)
    {
      Job pending = self->pending;

      self->pending = JOB_NONE;
      if (img)
        image_free (img);
      g_clear_error (&error);
      start_job (self, pending);
      return;
    }

  switch (job)
    {
    case JOB_OPEN:
      fpi_image_device_open_complete (dev, error);
      return;

    case JOB_STOP:
      fpi_image_device_deactivate_complete (dev, error);
      return;

    case JOB_CLOSE:
      fpi_image_device_close_complete (dev, error);
      return;

    case JOB_CAPTURE:
    case JOB_NONE:
      break;
    }

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA))
        {
          g_clear_error (&error);
          fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_GENERAL);
        }
      else
        {
          fpi_image_device_session_error (dev, error);
        }
      return;
    }

  image = fp_image_new (img->cols, img->rows);
  for (gsize i = 0; i < (gsize) img->cols * img->rows; i++)
    image->data[i] = (guint8) (img->pixels[i] * 255u / 4095u);
  image_free (img);
  dump_frame (image);
  fpi_image_device_image_captured (dev, image);
}

static void
start_job (FpiDeviceGxfp51b7 *self,
           Job                job)
{
  GTask *task;

  stop_timer (self);
  if (self->busy)
    {
      self->pending = job;
      if (self->worker_cancel)
        g_cancellable_cancel (self->worker_cancel);
      return;
    }

  if (job == JOB_OPEN)
    write_status ("calibrating");

  self->busy = TRUE;
  self->worker_cancel = g_cancellable_new ();
  task = g_task_new (self, self->worker_cancel, job_done, GINT_TO_POINTER (job));
  /* The flow checks cancellation itself and leaves the sensor idle. */
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_task_data (task, GINT_TO_POINTER (job), NULL);
  g_task_run_in_thread (task, worker);
  g_object_unref (task);
}

static void
poll_finger (FpDevice *device,
             gpointer  data)
{
  FpiDeviceGxfp51b7 *self = data;
  int present = 0;
  int ret;

  self->timer = NULL;
  if (self->busy || !self->session)
    return;

  /* Drain a few queued frames per tick. */
  for (int i = 0; i < 8; i++)
    {
      ret = gxfp51b7_poll (self->session, &present);
      if (ret < 0)
        {
          fpi_image_device_session_error (FP_IMAGE_DEVICE (self),
                                          fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                    "51B7 finger event: %s",
                                                                    g_strerror (-ret)));
          return;
        }
      if (ret == 1)
        {
          fp_dbg ("51B7 FINGER_%s", present ? "ON" : "OFF");
          fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (self), present);
          return;
        }
    }

  self->timer = fpi_device_add_timeout (device, GXFP51B7_POLL_MS, poll_finger, self, NULL);
}

static void
gxfp51b7_img_open (FpImageDevice *device)
{
  FpiDeviceGxfp51b7 *self = FPI_DEVICE_GXFP51B7 (device);
  const char *key = g_getenv ("FP_GXFP51B7_PSK");
  const char *material = g_getenv ("FP_GXFP51B7_MATERIAL");

  g_free (self->dev_path);
  g_free (self->key_path);
  g_free (self->material_path);
  self->dev_path = g_strdup (fpi_device_get_udev_data (FP_DEVICE (device),
                                                       FPI_DEVICE_UDEV_SUBTYPE_CHARDEV));
  self->key_path = g_strdup (key ? key : GXFP51B7_DEFAULT_PSK_PATH);
  self->material_path = g_strdup (material ? material : GXFP51B7_DEFAULT_MATERIAL_PATH);
  start_job (self, JOB_OPEN);
}

static void
gxfp51b7_img_close (FpImageDevice *device)
{
  start_job (FPI_DEVICE_GXFP51B7 (device), JOB_CLOSE);
}

static void
gxfp51b7_activate (FpImageDevice *device)
{
  fpi_image_device_activate_complete (device, NULL);
}

static void
gxfp51b7_deactivate (FpImageDevice *device)
{
  start_job (FPI_DEVICE_GXFP51B7 (device), JOB_STOP);
}

static void
gxfp51b7_change_state (FpImageDevice       *device,
                       FpiImageDeviceState  state)
{
  FpiDeviceGxfp51b7 *self = FPI_DEVICE_GXFP51B7 (device);
  gboolean want_finger;
  int ret;

  stop_timer (self);

  if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
    {
      write_status ("capture");
      start_job (self, JOB_CAPTURE);
      return;
    }

  if (state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON &&
      state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    return;

  want_finger = state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON;
  write_status (want_finger ? "finger-on" : "finger-off");
  ret = gxfp51b7_arm (self->session, want_finger);
  if (ret)
    {
      fpi_image_device_session_error (device,
                                      fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                "51B7 arm failed: %s",
                                                                g_strerror (-ret)));
      return;
    }
  self->timer = fpi_device_add_timeout (FP_DEVICE (device), GXFP51B7_POLL_MS,
                                        poll_finger, self, NULL);
}

static void
fpi_device_gxfp51b7_dispose (GObject *obj)
{
  FpiDeviceGxfp51b7 *self = FPI_DEVICE_GXFP51B7 (obj);

  stop_timer (self);
  /* A running GTask holds a reference, so no worker is active here. */
  gxfp51b7_close (self->session);
  self->session = NULL;
  g_clear_object (&self->worker_cancel);
  g_clear_pointer (&self->dev_path, g_free);
  g_clear_pointer (&self->key_path, g_free);
  g_clear_pointer (&self->material_path, g_free);

  G_OBJECT_CLASS (fpi_device_gxfp51b7_parent_class)->dispose (obj);
}

static const FpIdEntry id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_CHARDEV, .chardev_acpi_id = "GXFP51B7" },
  { .udev_types = 0 }
};

static void
fpi_device_gxfp51b7_init (FpiDeviceGxfp51b7 *self)
{
}

static void
fpi_device_gxfp51b7_class_init (FpiDeviceGxfp51b7Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  object_class->dispose = fpi_device_gxfp51b7_dispose;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Goodix GXFP51B7 ChicagoHS";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = GXFP51B7_ENROLL_STAGES;

  img_class->img_width = 80;
  img_class->img_height = 64;
  img_class->algorithm = FPI_DEVICE_ALGO_SIGFM;
  img_class->ridge_threshold = GXFP51B7_RIDGE_THRESHOLD;
  img_class->ridge_duplicate = GXFP51B7_RIDGE_DUPLICATE;
  img_class->img_open = gxfp51b7_img_open;
  img_class->img_close = gxfp51b7_img_close;
  img_class->activate = gxfp51b7_activate;
  img_class->deactivate = gxfp51b7_deactivate;
  img_class->change_state = gxfp51b7_change_state;
}
