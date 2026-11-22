/*
 * Phoc stand-in for Mango's "mango/common/server.h".
 *
 * The Anland wlroots backend (anland/producers/mango/anland_backend) was
 * written for Mango and reads a handful of fields from Mango's global
 * `server` struct. Phoc keeps the same state in GObjects, so this header maps
 * `server.<field>` onto a small view that phoc-anland.c refreshes on every
 * access. The backend sources are compiled unmodified.
 */
#pragma once

/*
 * Pull in every header the backend uses before `server` becomes a macro, so
 * the macro can never rewrite an identifier inside a library header. All of
 * them have include guards, so the backend's own later includes are no-ops.
 */
#include <anland_audio.h>
#include <anland_camera.h>
#include <anland_de_backend.h>
#include <anland_tablet.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_tablet_tool.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

struct mango_input_method_relay;

struct phoc_anland_server_view {
  struct wl_display                *display;
  struct wlr_backend               *backend;
  struct wlr_seat                  *seat;
  struct wlr_pointer_constraint_v1 *active_constraint;
  struct mango_input_method_relay  *input_method_relay;
};

struct phoc_anland_server_view *phoc_anland_server_view (void);

#ifndef PHOC_ANLAND_GLUE
#define server (*phoc_anland_server_view ())
#endif
