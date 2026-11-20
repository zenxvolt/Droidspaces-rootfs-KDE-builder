#ifndef ANLAND_CLIPBOARD_H
#define ANLAND_CLIPBOARD_H

#include <stddef.h>

struct wl_client;
struct wl_display;
struct anland_clipboard;

enum { ANLAND_CLIPBOARD_LIMIT = 1024 * 1024 };

struct anland_clipboard *anland_clipboard_create(
    struct wl_display *display,
    void (*send)(void *data, const void *text, size_t size), void *data);
void anland_clipboard_destroy(struct anland_clipboard *clipboard);
void anland_clipboard_focus(struct anland_clipboard *clipboard, struct wl_client *client);
void anland_clipboard_set_text(struct anland_clipboard *clipboard, const void *text, size_t size);
void anland_clipboard_cancel_receive(struct anland_clipboard *clipboard);

#endif
