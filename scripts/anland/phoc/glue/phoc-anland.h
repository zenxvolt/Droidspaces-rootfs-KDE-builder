/*
 * Phoc side of the Anland backend: the pieces phoc's own sources call.
 */
#pragma once

#include "anland_backend/anland.h"

typedef struct _PhocServer PhocServer;
struct wl_event_loop;
struct wlr_backend;
struct wlr_output;

/*
 * Create the Anland backend for @server. The backend reaches the server's
 * seat and display through the glue, so it must be created through here.
 */
struct wlr_backend *phoc_anland_backend_create      (PhocServer           *server,
                                                     struct wl_event_loop *loop,
                                                     const char           *socket_path);

/*
 * Scale for an Anland output. Android does not report a physical size, so
 * phoc's usual DPI based guess cannot work. Aim for Phosh's design width of
 * 360 logical pixels; PHOC_ANLAND_SCALE or a scale= line in phoc.ini wins.
 */
float               phoc_anland_output_compute_scale (struct wlr_output *output);
