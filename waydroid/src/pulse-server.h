/*
 * A minimal PulseAudio server for Waydroid's audio HAL.
 *
 * Waydroid's Android plays sound through ALSA's "pulse" plugin, which is a
 * PulseAudio client: it connects to $PULSE_RUNTIME_PATH/native (the directory
 * comes from the waydroid.pulse_runtime_path property). There is no PulseAudio
 * daemon in this rootfs, so the bridge answers on that socket itself. It
 * speaks the part of the native protocol a playback client needs, mixes the
 * streams in real time and hands the result to a callback, which sends it to
 * the Anland app.
 *
 * Everything runs on the bridge's Wayland event loop; there are no threads.
 */
#ifndef PULSE_SERVER_H
#define PULSE_SERVER_H

#include <stddef.h>
#include <stdint.h>

struct wl_event_loop;
struct pulse_server;

/* Receives mixed, interleaved signed 16-bit PCM in the current output format.
 * Called about every 10 ms while at least one stream is playing. */
typedef void (*pulse_output_fn)(void *data, const int16_t *pcm, size_t frames,
                                uint32_t channels);

/* Creates the socket at socket_path (its directory is created as well) and
 * starts accepting clients. Returns NULL and sets errno on failure. */
struct pulse_server *pulse_server_new(struct wl_event_loop *loop, const char *socket_path,
                                      pulse_output_fn output, void *data);

/* The format the receiver wants. Streams are converted to it. */
void pulse_server_set_output_format(struct pulse_server *srv, uint32_t rate, uint32_t channels);

/* Delay between handing PCM to the output and it being heard, reported to the
 * clients so they can keep video in sync. */
void pulse_server_set_output_latency_us(struct pulse_server *srv, uint64_t usec);

void pulse_server_destroy(struct pulse_server *srv);

#endif
