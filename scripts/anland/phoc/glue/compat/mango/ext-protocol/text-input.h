/*
 * Phoc stand-in for Mango's "mango/ext-protocol/text-input.h".
 *
 * The backend only needs to know whether the focused client has an enabled
 * text-input-v3 object, and a way to commit a string to it. phoc-anland.c
 * fills these in from wlroots' public text-input manager.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct wlr_text_input_v3;

struct mango_text_input {
  struct wlr_text_input_v3 *input;
};

struct mango_input_method_relay {
  /* NULL when the focused surface has no enabled text-input-v3. */
  struct mango_text_input *active_text_input;
};

bool mango_text_input_commit_utf8 (struct mango_input_method_relay *relay,
                                   const char                      *text,
                                   size_t                           length);
