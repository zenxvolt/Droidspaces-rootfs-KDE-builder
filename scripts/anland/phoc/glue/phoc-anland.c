/*
 * Glue between phoc and the Anland wlroots backend.
 *
 * The backend expects Mango's global `server` and two text-input helpers.
 * Everything here answers those questions from phoc's own state, using only
 * public phoc and wlroots interfaces.
 */
#define G_LOG_DOMAIN "phoc-anland"

#include "phoc-config.h"

#define PHOC_ANLAND_GLUE 1
#include "mango/common/server.h"
#include "mango/ext-protocol/text-input.h"
#include "anland_backend/internal.h"

#include "phoc-anland.h"

#include "cursor.h"
#include "desktop.h"
#include "seat.h"
#include "server.h"

#include <wlr/types/wlr_linux_dmabuf_v1.h>

#include <math.h>
#include <string.h>

#define PHOC_ANLAND_DESIGN_WIDTH 360.0

static PhocServer *attached_server;
static struct phoc_anland_server_view view;
static struct mango_text_input active_text_input;
static struct mango_input_method_relay relay;

static struct anland_backend *attached_backend;
static struct wl_listener backend_destroy;
static struct wlr_seat *watched_seat;
static struct wl_listener seat_destroy;


static void
unwatch_seat (void)
{
  if (watched_seat == NULL)
    return;

  wl_list_remove (&seat_destroy.link);
  watched_seat = NULL;
}

/*
 * The backend hooks its clipboard and focus listeners to the seat when it
 * starts and only unhooks them when it is destroyed. Phoc tears its seats
 * down before the backend, and wlroots aborts if a seat dies with listeners
 * still attached, so unhook them here.
 */
static void
handle_seat_destroy (struct wl_listener *listener, void *data)
{
  struct anland_backend *backend = attached_backend;

  unwatch_seat ();
  /* While the backend is being destroyed it has already unhooked both
   * listeners (without clearing the flags); releasing its input devices is
   * what drops phoc's last seat reference and gets us here. */
  if (backend == NULL || backend->destroying)
    return;

  if (backend->selection_listener_set) {
    wl_list_remove (&backend->selection_listener.link);
    backend->selection_listener_set = false;
  }
  if (backend->focus_listener_set) {
    wl_list_remove (&backend->focus_change.link);
    backend->focus_listener_set = false;
  }
}


static void
watch_seat (struct wlr_seat *seat)
{
  if (watched_seat != NULL || attached_backend == NULL)
    return;

  watched_seat = seat;
  seat_destroy.notify = handle_seat_destroy;
  wl_signal_add (&seat->events.destroy, &seat_destroy);
}


static void
handle_backend_destroy (struct wl_listener *listener, void *data)
{
  unwatch_seat ();
  wl_list_remove (&backend_destroy.link);
  attached_backend = NULL;
}


struct wlr_backend *
phoc_anland_backend_create (PhocServer *server, struct wl_event_loop *loop, const char *socket_path)
{
  struct wlr_backend *backend;

  attached_server = server;
  backend = mango_anland_backend_create (loop, socket_path);
  if (backend == NULL)
    return NULL;

  attached_backend = wl_container_of (backend, attached_backend, base);
  backend_destroy.notify = handle_backend_destroy;
  wl_signal_add (&backend->events.destroy, &backend_destroy);

  return backend;
}


static struct wlr_text_input_v3 *
find_enabled_text_input (PhocDesktop *desktop, struct wlr_seat *seat)
{
  struct wlr_surface *focus = seat->keyboard_state.focused_surface;
  struct wlr_text_input_v3 *input;

  if (focus == NULL || desktop == NULL || desktop->text_input == NULL)
    return NULL;

  wl_list_for_each (input, &desktop->text_input->text_inputs, link) {
    if (input->seat == seat && input->focused_surface == focus && input->current_enabled)
      return input;
  }

  return NULL;
}


struct phoc_anland_server_view *
phoc_anland_server_view (void)
{
  PhocServer *server = attached_server;
  PhocSeat *seat = NULL;

  memset (&view, 0, sizeof (view));
  active_text_input.input = NULL;
  relay.active_text_input = NULL;

  if (server == NULL)
    return &view;

  view.display = phoc_server_get_wl_display (server);
  view.backend = phoc_server_get_backend (server);

  /* The backend only starts after phoc_server_setup() created the input. */
  if (phoc_server_get_input (server) != NULL)
    seat = phoc_server_get_last_active_seat (server);
  if (seat == NULL)
    return &view;

  view.seat = seat->seat;
  watch_seat (seat->seat);
  if (seat->cursor)
    view.active_constraint = seat->cursor->active_constraint;

  active_text_input.input = find_enabled_text_input (phoc_server_get_desktop (server), seat->seat);
  if (active_text_input.input)
    relay.active_text_input = &active_text_input;
  view.input_method_relay = &relay;

  return &view;
}


bool
mango_text_input_commit_utf8 (struct mango_input_method_relay *text_relay,
                              const char                      *text,
                              size_t                           length)
{
  struct wlr_text_input_v3 *input;
  g_autofree char *copy = NULL;

  /* One commit_string must stay below the default 4096 byte wire buffer. */
  if (text == NULL || length == 0 || length > 4000 || memchr (text, '\0', length))
    return false;

  if (text_relay == NULL || text_relay->active_text_input == NULL)
    return false;

  input = text_relay->active_text_input->input;
  if (input == NULL || !input->current_enabled || input->focused_surface == NULL)
    return false;

  copy = g_strndup (text, length);
  if (!g_utf8_validate (copy, -1, NULL))
    return false;

  wlr_text_input_v3_send_commit_string (input, copy);
  wlr_text_input_v3_send_done (input);
  return true;
}


float
phoc_anland_output_compute_scale (struct wlr_output *output)
{
  const char *env = g_getenv ("PHOC_ANLAND_SCALE");
  double scale;
  int shortest;

  if (env && *env) {
    scale = g_ascii_strtod (env, NULL);
    if (scale >= 0.5 && scale <= 8.0)
      return scale;
    g_warning ("Ignoring invalid PHOC_ANLAND_SCALE '%s'", env);
  }

  shortest = MIN (output->width, output->height);
  if (shortest <= 0)
    return 1.0;

  /* Quarter steps keep text sharp and match what phosh-mobile-settings offers. */
  scale = floor (shortest / PHOC_ANLAND_DESIGN_WIDTH * 4.0) / 4.0;
  return CLAMP (scale, 1.0, 4.0);
}


static bool
is_yuv_format (uint32_t format)
{
  switch (format) {
  case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: case DRM_FORMAT_NV16: case DRM_FORMAT_NV61:
  case DRM_FORMAT_NV24: case DRM_FORMAT_NV42:
  case DRM_FORMAT_P010: case DRM_FORMAT_P012: case DRM_FORMAT_P016:
  case DRM_FORMAT_YUV410: case DRM_FORMAT_YVU410: case DRM_FORMAT_YUV411: case DRM_FORMAT_YVU411:
  case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: case DRM_FORMAT_YUV422: case DRM_FORMAT_YVU422:
  case DRM_FORMAT_YUV444: case DRM_FORMAT_YVU444:
  case DRM_FORMAT_YUYV: case DRM_FORMAT_YVYU: case DRM_FORMAT_UYVY: case DRM_FORMAT_VYUY:
  case DRM_FORMAT_AYUV: case DRM_FORMAT_XYUV8888:
#ifdef DRM_FORMAT_P210
  case DRM_FORMAT_P210:
#endif
#ifdef DRM_FORMAT_P030
  case DRM_FORMAT_P030:
#endif
#ifdef DRM_FORMAT_NV15
  case DRM_FORMAT_NV15:
#endif
#ifdef DRM_FORMAT_Y210
  case DRM_FORMAT_Y210: case DRM_FORMAT_Y212: case DRM_FORMAT_Y216:
  case DRM_FORMAT_Y410: case DRM_FORMAT_Y412: case DRM_FORMAT_Y416:
#endif
#ifdef DRM_FORMAT_VUY888
  case DRM_FORMAT_VUY888: case DRM_FORMAT_VUY101010:
#endif
    return true;
  default:
    return false;
  }
}

/*
 * wlroots hands YUV buffers (NV12, ...) to the driver as "external" EGL
 * images and lets it convert them to RGB while sampling. On Adreno with the
 * kgsl Mesa driver that conversion comes out with wrong colours, so video in
 * Chrome turned green and magenta. Without YUV in the advertised formats,
 * clients convert to RGB themselves, which they all can.
 */
void
phoc_anland_filter_dmabuf_feedback (struct wlr_linux_dmabuf_feedback_v1 *feedback)
{
  struct wlr_linux_dmabuf_feedback_v1_tranche *tranche;

  if (attached_backend == NULL || feedback == NULL)
    return;
  /* For drivers that do convert YUV correctly, and for comparing. */
  if (g_strcmp0 (g_getenv ("PHOC_ANLAND_ALLOW_YUV"), "1") == 0)
    return;

  wl_array_for_each (tranche, &feedback->tranches) {
    struct wlr_drm_format_set rgb = { 0 };

    for (size_t i = 0; i < tranche->formats.len; i++) {
      const struct wlr_drm_format *format = &tranche->formats.formats[i];

      if (is_yuv_format (format->format))
        continue;
      for (size_t j = 0; j < format->len; j++)
        wlr_drm_format_set_add (&rgb, format->format, format->modifiers[j]);
    }
    wlr_drm_format_set_finish (&tranche->formats);
    tranche->formats = rgb;
  }
}
