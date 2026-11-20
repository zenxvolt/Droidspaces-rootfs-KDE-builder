#define _GNU_SOURCE
#include "anland-clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

static const char *const text_types[] = {
    "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};

struct transfer {
    struct wl_event_source *io, *timer;
    int fd;
    size_t size, offset;
    char text[];
};

struct clipboard_write {
    struct wl_list link;
    struct transfer *transfer;
};

struct anland_clipboard {
    struct wl_event_loop *loop;
    struct wl_global *global;
    struct wl_list devices, writes;
    struct wl_client *focus;
    struct transfer *receive;
    void (*send)(void *data, const void *text, size_t size);
    void *data;
    char *text;
    size_t size;
};

struct clipboard_offer {
    struct anland_clipboard *clipboard;
    size_t size;
    char text[];
};

struct clipboard_source {
    unsigned int types;
    bool used;
};

static int text_type(const char *type)
{
    for (size_t i = 0; i < sizeof(text_types) / sizeof(*text_types); i++)
        if (!strcmp(type, text_types[i]))
            return (int)i;
    return -1;
}

static void close_transfer(struct transfer *t)
{
    if (t->io)
        wl_event_source_remove(t->io);
    if (t->timer)
        wl_event_source_remove(t->timer);
    close(t->fd);
    free(t);
}

static struct transfer *start_transfer(struct wl_event_loop *loop, int fd, size_t size,
                                       uint32_t mask, wl_event_loop_fd_func_t io,
                                       wl_event_loop_timer_func_t timeout, void *data)
{
    struct transfer *t = calloc(1, sizeof(*t) + size);
    if (!t) {
        close(fd);
        return NULL;
    }
    t->fd = fd;
    t->size = size;
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        t->io = wl_event_loop_add_fd(loop, fd, mask, io, data);
        t->timer = wl_event_loop_add_timer(loop, timeout, data);
        if (t->io && t->timer && wl_event_source_timer_update(t->timer, 3000) == 0)
            return t;
    }
    close_transfer(t);
    return NULL;
}

static void finish_write(struct clipboard_write *w)
{
    close_transfer(w->transfer);
    wl_list_remove(&w->link);
    free(w);
}

static int write_timeout(void *data)
{
    finish_write(data);
    return 0;
}

static int write_clipboard(int fd, uint32_t mask, void *data)
{
    struct clipboard_write *w = data;
    struct transfer *t = w->transfer;
    if (!(mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP))) {
        while (t->offset < t->size) {
            ssize_t n = write(fd, t->text + t->offset, t->size - t->offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            if (n <= 0)
                break;
            t->offset += (size_t)n;
        }
    }
    finish_write(w);
    return 0;
}

static void offer_receive(struct wl_client *client, struct wl_resource *resource,
                          const char *mime, int32_t fd)
{
    struct clipboard_offer *o = wl_resource_get_user_data(resource);
    if (client != o->clipboard->focus || text_type(mime) < 0) {
        close(fd);
        return;
    }
    struct clipboard_write *w = malloc(sizeof(*w));
    if (!w) {
        close(fd);
        wl_client_post_no_memory(client);
        return;
    }
    w->transfer = start_transfer(o->clipboard->loop, fd, o->size, WL_EVENT_WRITABLE,
                                 write_clipboard, write_timeout, w);
    if (!w->transfer) {
        free(w);
        return;
    }
    memcpy(w->transfer->text, o->text, o->size);
    wl_list_insert(&o->clipboard->writes, &w->link);
}

static void destroy_request(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void free_resource_data(struct wl_resource *resource)
{
    free(wl_resource_get_user_data(resource));
}

static void offer_accept(struct wl_client *client, struct wl_resource *resource,
                         uint32_t serial, const char *mime)
{
    (void)client; (void)resource; (void)serial; (void)mime;
}

static const struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = destroy_request,
};

static void publish_selection(struct anland_clipboard *c, struct wl_resource *device)
{
    struct wl_client *client = wl_resource_get_client(device);
    if (!c->size) {
        wl_data_device_send_selection(device, NULL);
        return;
    }

    struct clipboard_offer *o = malloc(sizeof(*o) + c->size);
    struct wl_resource *offer = o ? wl_resource_create(client, &wl_data_offer_interface, 1, 0) : NULL;
    if (!offer) {
        free(o);
        wl_client_post_no_memory(client);
        return;
    }
    o->clipboard = c;
    o->size = c->size;
    memcpy(o->text, c->text, c->size);
    wl_resource_set_implementation(offer, &offer_impl, o, free_resource_data);
    wl_data_device_send_data_offer(device, offer);
    for (size_t i = 0; i < sizeof(text_types) / sizeof(*text_types); i++)
        wl_data_offer_send_offer(offer, text_types[i]);
    wl_data_device_send_selection(device, offer);
}

void anland_clipboard_cancel_receive(struct anland_clipboard *c)
{
    if (c->receive)
        close_transfer(c->receive);
    c->receive = NULL;
}

static int read_timeout(void *data)
{
    anland_clipboard_cancel_receive(data);
    return 0;
}

static int read_clipboard(int fd, uint32_t mask, void *data)
{
    struct anland_clipboard *c = data;
    struct transfer *t = c->receive;
    if (!(mask & WL_EVENT_ERROR)) {
        while (t->offset < t->size) {
            ssize_t n = read(fd, t->text + t->offset, t->size - t->offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            if (n == 0) {
                /* Avoid reflecting a host update back into the host clipboard. */
                c->receive = NULL;
                if (t->offset != c->size ||
                    (t->offset && memcmp(c->text, t->text, t->offset))) {
                    free(c->text);
                    c->text = NULL;
                    c->size = 0;
                    c->send(c->data, t->text, t->offset);
                }
                close_transfer(t);
                return 0;
            }
            if (n < 0)
                break;
            t->offset += (size_t)n;
        }
    }
    anland_clipboard_cancel_receive(c);
    return 0;
}

static void set_selection(struct wl_client *client, struct wl_resource *device,
                          struct wl_resource *source, uint32_t serial)
{
    (void)serial; /* Waydroid reuses the serial of its keyboard enter. */
    struct anland_clipboard *c = wl_resource_get_user_data(device);
    if (client != c->focus)
        return;
    if (!source) {
        anland_clipboard_cancel_receive(c);
        c->send(c->data, NULL, 0);
        return;
    }
    struct clipboard_source *s = wl_resource_get_user_data(source);
    if (s->used) {
        wl_data_source_send_cancelled(source);
        return;
    }
    s->used = true;
    if (!s->types) {
        wl_data_source_send_cancelled(source);
        return;
    }
    anland_clipboard_cancel_receive(c);
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == 0) {
        /* One extra byte detects a clipboard that exceeds our byte limit. */
        c->receive = start_transfer(c->loop, fds[0], ANLAND_CLIPBOARD_LIMIT + 1,
                                    WL_EVENT_READABLE, read_clipboard, read_timeout, c);
        if (c->receive)
            wl_data_source_send_send(source, text_types[__builtin_ctz(s->types)], fds[1]);
        close(fds[1]);
    }
    /* Wayland delivers the send request first. The client owns its copy of the pipe. */
    wl_data_source_send_cancelled(source);
}

static void start_drag(struct wl_client *client, struct wl_resource *device,
                       struct wl_resource *source, struct wl_resource *origin,
                       struct wl_resource *icon, uint32_t serial)
{
    (void)client; (void)device; (void)origin; (void)icon; (void)serial;
    if (source)
        wl_data_source_send_cancelled(source);
}

static const struct wl_data_device_interface device_impl = {
    .start_drag = start_drag,
    .set_selection = set_selection,
};

static void source_offer(struct wl_client *client, struct wl_resource *resource, const char *mime)
{
    (void)client;
    struct clipboard_source *s = wl_resource_get_user_data(resource);
    int type = text_type(mime);
    if (type >= 0 && !s->used)
        s->types |= 1u << type;
}

static const struct wl_data_source_interface source_impl = {
    .offer = source_offer,
    .destroy = destroy_request,
};

static void create_source(struct wl_client *client, struct wl_resource *manager, uint32_t id)
{
    struct clipboard_source *s = calloc(1, sizeof(*s));
    struct wl_resource *source = s ? wl_resource_create(client, &wl_data_source_interface, 1, id) : NULL;
    if (!source) {
        free(s);
        wl_client_post_no_memory(client);
        return;
    }
    (void)manager;
    wl_resource_set_implementation(source, &source_impl, s, free_resource_data);
}

static void device_destroy(struct wl_resource *device)
{
    wl_list_remove(wl_resource_get_link(device));
}

static void get_device(struct wl_client *client, struct wl_resource *manager, uint32_t id,
                       struct wl_resource *seat)
{
    (void)seat;
    struct anland_clipboard *c = wl_resource_get_user_data(manager);
    struct wl_resource *device = wl_resource_create(client, &wl_data_device_interface, 1, id);
    if (!device) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(device, &device_impl, c, device_destroy);
    wl_list_insert(&c->devices, wl_resource_get_link(device));
    if (c->focus == client && c->size)
        publish_selection(c, device);
}

static const struct wl_data_device_manager_interface manager_impl = {
    .create_data_source = create_source,
    .get_data_device = get_device,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *manager = wl_resource_create(client, &wl_data_device_manager_interface,
                                                      (int)version, id);
    if (!manager) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(manager, &manager_impl, data, NULL);
}

struct anland_clipboard *anland_clipboard_create(
    struct wl_display *display,
    void (*send)(void *data, const void *text, size_t size), void *data)
{
    struct anland_clipboard *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->loop = wl_display_get_event_loop(display);
    c->send = send;
    c->data = data;
    wl_list_init(&c->devices);
    wl_list_init(&c->writes);
    c->global = wl_global_create(display, &wl_data_device_manager_interface, 1, c, bind_manager);
    if (!c->global) {
        free(c);
        return NULL;
    }
    return c;
}

void anland_clipboard_destroy(struct anland_clipboard *c)
{
    anland_clipboard_cancel_receive(c);
    struct clipboard_write *w, *next;
    wl_list_for_each_safe(w, next, &c->writes, link)
        finish_write(w);
    wl_global_destroy(c->global);
    free(c->text);
    free(c);
}

void anland_clipboard_focus(struct anland_clipboard *c, struct wl_client *client)
{
    if (c->focus == client)
        return;
    anland_clipboard_cancel_receive(c);
    struct clipboard_write *w, *next;
    wl_list_for_each_safe(w, next, &c->writes, link)
        finish_write(w);
    struct wl_resource *device;
    wl_resource_for_each(device, &c->devices)
        if (wl_resource_get_client(device) == c->focus)
            wl_data_device_send_selection(device, NULL);
    c->focus = client;
    wl_resource_for_each(device, &c->devices)
        if (wl_resource_get_client(device) == client && c->size)
            publish_selection(c, device);
}

void anland_clipboard_set_text(struct anland_clipboard *c, const void *text, size_t size)
{
    char *copy = size ? malloc(size) : NULL;
    if (size && !copy)
        return;
    if (size)
        memcpy(copy, text, size);
    anland_clipboard_cancel_receive(c);
    free(c->text);
    c->text = copy;
    c->size = size;
    struct wl_resource *device;
    wl_resource_for_each(device, &c->devices)
        if (wl_resource_get_client(device) == c->focus)
            publish_selection(c, device);
}
