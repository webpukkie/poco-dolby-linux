/* pw-dolby.c — Dolby Atmos (DAP) virtual sink for PipeWire.
 *
 * Creates a virtual output device that applications can simply select as
 * their playback device (default sink).  Audio played into it is routed
 * through the dolbyd RPC daemon (aarch64 libswdap.so under qemu-aarch64,
 * or native on arm64) and played back on the real hardware sink:
 *
 *   [apps] -> "Dolby Atmos (DAP)" (this process) -> [real sink]
 *
 * No pw-link wiring is needed: the session manager (WirePlumber) links the
 * output stream to the real sink automatically.  Both streams advertise
 * f32 / stereo / session-rate and the server-side stream converters handle
 * everything else (s16 sources, mono, other rates).
 *
 * The real output device is chosen at startup: the sink that was default
 * before this one existed (read from WirePlumber's "default" metadata), or
 * the --sink argument.  Pass --sink explicitly to chain into another device
 * (e.g. the LiteFX sink) or after a restart.
 *
 * Note the blocking socket round-trip per 256-frame block: this adds
 * latency and is not RT-safe; for a production node run dolbyd on arm64
 * natively (or port the libs to x86_64).
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>

#define MAGIC 0x31425044u /* 'DBP1' */
enum { CMD_HELLO = 0, CMD_PROCESS = 1, CMD_ENABLE = 2, CMD_DISABLE = 3, CMD_BYE = 4 };

#define CH 2
#define SINK_WAIT_MS 3000
#define PRIME_BLOCKS 2

struct impl {
    struct pw_main_loop *loop;
    struct pw_context *ctx;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook core_listener, registry_listener, meta_listener;
    struct pw_metadata *metadata;
    char default_sink[256];
    int have_default, synced, pending_seq;

    struct pw_stream *sink, *playback;
    struct spa_hook sink_listener, playback_listener;

    int fd;
    uint32_t rate, frames;
    size_t block;           /* interleaved s16 bytes per engine block */
    uint8_t *out;           /* daemon output staging (one block) */
    uint8_t *fifo_in;  size_t fifo_in_cap, fifo_in_len, fifo_in_off;
    uint8_t *fifo_out; size_t fifo_out_cap, fifo_out_len, fifo_out_off;
    int primed;
    unsigned long long blocks;
};

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= k;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) return 1;
        p += k; n -= k;
    }
    return 0;
}

static int daemon_hello(struct impl *d) {
    uint8_t hdr[12], p[16], rep[4];
    put32(hdr, MAGIC); put32(hdr + 4, CMD_HELLO); put32(hdr + 8, 16);
    put32(p, d->rate); put32(p + 4, CH); put32(p + 8, 1 /* s16 */); put32(p + 12, d->frames);
    if (write_full(d->fd, hdr, sizeof(hdr)) || write_full(d->fd, p, 16) ||
        read_full(d->fd, rep, 4)) {
        fprintf(stderr, "dolby: daemon HELLO failed\n");
        return -1;
    }
    if (get32(rep) != 0) { fprintf(stderr, "dolby: daemon rejected session\n"); return -1; }
    return 0;
}

static int daemon_process(struct impl *d, const uint8_t *in, uint8_t *out) {
    uint8_t hdr[12], rep[4];
    put32(hdr, MAGIC); put32(hdr + 4, CMD_PROCESS); put32(hdr + 8, d->block);
    if (write_full(d->fd, hdr, sizeof(hdr)) || write_full(d->fd, in, d->block) ||
        read_full(d->fd, rep, 4) || read_full(d->fd, out, d->block))
        return -1;
    return (int)get32(rep);
}

static uint8_t *buf_grow(uint8_t *p, size_t need, size_t *cap) {
    if (need <= *cap) return p;
    size_t nc = *cap ? *cap : 8192;
    while (nc < need) nc <<= 1;
    uint8_t *np = realloc(p, nc);
    if (np) *cap = nc;
    return np;
}

static int16_t f2s(float x) {
    if (x > 1.0f) return 32767;
    if (x < -1.0f) return -32768;
    return (int16_t)(x * 32767.0f);
}

static void fifo_out_reset(struct impl *d) {
    d->fifo_out_len = d->fifo_out_off = 0;
    d->primed = 0;
}

/* f32 interleaved -> s16 interleaved FIFO; push whole blocks through the DAP */
static void pump(struct impl *d, const float *src, uint32_t n_frames) {
    size_t add = (size_t)n_frames * CH * 2;
    uint8_t *fi = buf_grow(d->fifo_in, d->fifo_in_len + add, &d->fifo_in_cap);
    if (!fi) return;
    d->fifo_in = fi;
    int16_t *s = (int16_t *)(d->fifo_in + d->fifo_in_len);
    for (uint32_t i = 0; i < (size_t)n_frames * CH; i++)
        s[i] = f2s(src[i]);
    d->fifo_in_len += add;

    while (d->fifo_in_len - d->fifo_in_off >= d->block) {
        int rc = daemon_process(d, d->fifo_in + d->fifo_in_off, d->out);
        if (rc != 0) {
            fprintf(stderr, "dolby: process rc=%d, dropping buffered audio\n", rc);
            d->fifo_in_len = d->fifo_in_off = 0;
            fifo_out_reset(d);
            return;
        }
        uint8_t *fo = buf_grow(d->fifo_out, d->fifo_out_len + d->block, &d->fifo_out_cap);
        if (!fo) return;
        d->fifo_out = fo;
        memcpy(d->fifo_out + d->fifo_out_len, d->out, d->block);
        d->fifo_out_len += d->block;
        d->fifo_in_off += d->block;
        d->blocks++;
    }
    if (d->fifo_in_off) {
        memmove(d->fifo_in, d->fifo_in + d->fifo_in_off, d->fifo_in_len - d->fifo_in_off);
        d->fifo_in_len -= d->fifo_in_off;
        d->fifo_in_off = 0;
    }
}

/* The virtual sink: receives whatever apps play into it. */
static void on_sink_process(void *data) {
    struct impl *d = data;
    struct pw_buffer *b;
    while ((b = pw_stream_dequeue_buffer(d->sink)) != NULL) {
        struct spa_data *sd = &b->buffer->datas[0];
        if (sd->data)
            pump(d, (const float *)(sd->data + sd->chunk->offset),
                 sd->chunk->size / (uint32_t)(CH * sizeof(float)));
        pw_stream_queue_buffer(d->sink, b);
    }
}

static void on_playback_process(void *data) {
    struct impl *d = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(d->playback);
    if (!b) return;
    struct spa_data *sd = &b->buffer->datas[0];
    size_t stride = CH * sizeof(float);
    uint32_t n = (uint32_t)(sd->maxsize / stride);
    if (b->requested > 0 && b->requested < n)
        n = (uint32_t)b->requested;
    float *dst = (float *)sd->data;
    uint32_t avail = (uint32_t)((d->fifo_out_len - d->fifo_out_off) / stride);

    if (!d->primed && avail >= PRIME_BLOCKS * d->frames)
        d->primed = 1;
    uint32_t emit = d->primed ? (avail < n ? avail : n) : 0;
    if (emit) {
        const int16_t *o = (const int16_t *)(d->fifo_out + d->fifo_out_off);
        for (uint32_t i = 0; i < (size_t)emit * CH; i++)
            dst[i] = (float)o[i] / 32768.0f;
        d->fifo_out_off += (size_t)emit * stride;
        memmove(d->fifo_out, d->fifo_out + d->fifo_out_off, d->fifo_out_len - d->fifo_out_off);
        d->fifo_out_len -= d->fifo_out_off;
        d->fifo_out_off = 0;
    }
    memset(dst + (size_t)emit * CH, 0, (size_t)(n - emit) * stride);
    sd->chunk->offset = 0;
    sd->chunk->size = n * stride;
    sd->chunk->stride = stride;
    pw_stream_queue_buffer(d->playback, b);
}

static void on_sink_state(void *data, enum pw_stream_state old,
                          enum pw_stream_state state, const char *error) {
    (void)data; (void)old;
    if (error)
        fprintf(stderr, "dolby: sink stream error: %s\n", error);
    if (state == PW_STREAM_STATE_STREAMING)
        fprintf(stderr, "dolby: sink 'dolby_atmos' streaming\n");
}

static void on_playback_state(void *data, enum pw_stream_state old,
                              enum pw_stream_state state, const char *error) {
    (void)data; (void)old;
    if (error)
        fprintf(stderr, "dolby: output stream error: %s\n", error);
    if (state == PW_STREAM_STATE_STREAMING)
        fprintf(stderr, "dolby: output streaming\n");
}

static const struct pw_stream_events sink_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_sink_state,
    .process = on_sink_process,
};

static const struct pw_stream_events playback_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_playback_state,
    .process = on_playback_process,
};

static void on_core_done(void *data, uint32_t id, int seq) {
    struct impl *d = data;
    if (id == PW_ID_CORE && seq == d->pending_seq)
        d->synced = 1;
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
};

static void core_sync(struct impl *d) {
    d->synced = 0;
    d->pending_seq = pw_core_sync(d->core, PW_ID_CORE, 0);
    while (!d->synced) {
        if (pw_loop_iterate(pw_main_loop_get_loop(d->loop), -1) < 0)
            break;
    }
}

/* remember the default sink as it was before we registered ours */
static int on_meta_property(void *data, uint32_t subject, const char *key,
                            const char *type, const char *value) {
    struct impl *d = data;
    char name[256];
    (void)type;
    if (d->have_default || subject != 0 || !key || !value)
        return 0;
    if (strcmp(key, "default.audio.sink") != 0 &&
        strcmp(key, "default.configured.audio.sink") != 0)
        return 0;
    if (spa_json_str_object_find(value, strlen(value), "name", name, sizeof(name)) > 0 &&
        name[0] && strcmp(name, "dolby_atmos") != 0) {
        snprintf(d->default_sink, sizeof(d->default_sink), "%s", name);
        d->have_default = 1;
    }
    return 0;
}

static const struct pw_metadata_events meta_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_meta_property,
};

static void on_registry_global(void *data, uint32_t id, uint32_t perms,
                               const char *type, uint32_t version,
                               const struct spa_dict *props) {
    struct impl *d = data;
    const char *name;
    (void)perms; (void)version;
    if (d->metadata || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0)
        return;
    name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : NULL;
    if (!name || strcmp(name, "default") != 0)
        return;
    d->metadata = pw_registry_bind(d->registry, id, type, PW_VERSION_METADATA, 0);
    if (d->metadata)
        pw_metadata_add_listener(d->metadata, &d->meta_listener, &meta_events, d);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
};

/* The default sink as it was before we registered ours. */
static void find_default_sink(struct impl *d) {
    struct timespec ts;
    uint64_t t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    do {
        if (pw_loop_iterate(pw_main_loop_get_loop(d->loop), 50) < 0)
            break;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t1 = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    } while (!d->have_default && t1 - t0 < SINK_WAIT_MS);
}

static struct pw_stream *connect_stream(struct impl *d, const char *stream_name,
                                        struct pw_properties *props,
                                        const struct pw_stream_events *ev,
                                        struct spa_hook *hook,
                                        enum pw_direction dir,
                                        enum pw_stream_flags flags) {
    uint8_t podbuf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
    const struct spa_pod *params[1];
    struct spa_audio_info_raw raw = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = d->rate,
        .channels = CH);
    raw.position[0] = SPA_AUDIO_CHANNEL_FL;
    raw.position[1] = SPA_AUDIO_CHANNEL_FR;
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

    struct pw_stream *s = pw_stream_new(d->core, stream_name, props);
    if (!s) return NULL;
    pw_stream_add_listener(s, hook, ev, d);
    if (pw_stream_connect(s, dir, PW_ID_ANY, flags, params, 1) < 0) {
        fprintf(stderr, "pw_stream_connect(%s) failed\n", stream_name);
        return NULL;
    }
    return s;
}

static void do_quit(void *userdata, int signal_number) {
    struct impl *d = userdata;
    (void)signal_number;
    pw_main_loop_quit(d->loop);
}

int main(int argc, char **argv) {
    struct impl impl = { .fd = -1, .rate = 48000, .frames = 256 };
    const char *socket_path = "/tmp/dolbyd.sock";
    const char *sink = NULL;
    const char *node_name = "dolby_atmos";
    const char *remote = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) impl.rate = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) impl.frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_path = argv[++i];
        else if (!strcmp(argv[i], "--sink") && i + 1 < argc) sink = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) node_name = argv[++i];
        else if (!strcmp(argv[i], "--remote") && i + 1 < argc) remote = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (impl.rate < 8000 || impl.rate > 192000) { fprintf(stderr, "bad rate\n"); return 2; }
    if (impl.frames < 64 || impl.frames > 4096) { fprintf(stderr, "bad frames\n"); return 2; }
    impl.block = (size_t)impl.frames * CH * 2;
    impl.out = malloc(impl.block);
    if (!impl.out) return 1;

    impl.fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl.fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", socket_path);
    for (int tries = 0;; tries++) {
        if (connect(impl.fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
        if (tries >= 30) { perror("connect (is dolbyd running?)"); return 1; }
        usleep(200 * 1000);
    }
    if (daemon_hello(&impl)) return 1;
    fprintf(stderr, "dolby: session up (rate=%u ch=%d frames=%u)\n",
            impl.rate, CH, impl.frames);

    pw_init(&argc, &argv);
    impl.loop = pw_main_loop_new(NULL);
    if (!impl.loop) { fprintf(stderr, "pw_main_loop_new failed\n"); return 1; }
    struct pw_loop *l = pw_main_loop_get_loop(impl.loop);
    struct spa_source *sig_int = pw_loop_add_signal(l, SIGINT, do_quit, &impl);
    struct spa_source *sig_term = pw_loop_add_signal(l, SIGTERM, do_quit, &impl);

    impl.ctx = pw_context_new(l, NULL, 0);
    impl.core = pw_context_connect(impl.ctx, NULL, 0);
    if (!impl.core) { fprintf(stderr, "pw_context_connect failed\n"); return 1; }
    pw_core_add_listener(impl.core, &impl.core_listener, &core_events, &impl);
    impl.registry = pw_core_get_registry(impl.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(impl.registry, &impl.registry_listener, &registry_events, &impl);

    if (!sink) {
        find_default_sink(&impl);
        sink = impl.have_default ? impl.default_sink : NULL;
    }
    if (!sink) {
        fprintf(stderr,
                "dolby: could not find the default output device in WirePlumber's metadata.\n"
                "dolby: pass the hardware sink explicitly: %s --sink <sink-name>\n"
                "dolby: (find the name with: pactl get-default-sink)\n", argv[0]);
        return 1;
    }
    if (!strcmp(sink, node_name)) {
        fprintf(stderr, "dolby: refusing to output to our own sink (feedback loop); "
                        "pass --sink <hardware-sink-name>\n");
        return 1;
    }

    /* virtual sink: apps select this as their output device */
    struct pw_properties *sink_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Sink",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, node_name,
        PW_KEY_NODE_DESCRIPTION, "Dolby Atmos (DAP)",
        PW_KEY_NODE_WANT_DRIVER, "true",
        NULL);
    if (remote) pw_properties_set(sink_props, PW_KEY_REMOTE_NAME, remote);
    impl.sink = connect_stream(&impl, "dolby_atmos", sink_props, &sink_events,
                               &impl.sink_listener, PW_DIRECTION_INPUT,
                               PW_STREAM_FLAG_MAP_BUFFERS);
    if (!impl.sink) return 1;
    core_sync(&impl);

    /* playback: WirePlumber links this to the real output device */
    struct pw_properties *out_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "dolby_atmos_output",
        PW_KEY_NODE_DESCRIPTION, "Dolby Atmos Output",
        PW_KEY_NODE_WANT_DRIVER, "true",
        PW_KEY_TARGET_OBJECT, sink,
        NULL);
    impl.playback = connect_stream(&impl, "dolby_atmos_output", out_props, &playback_events,
                                   &impl.playback_listener, PW_DIRECTION_INPUT,
                                   PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (!impl.playback) return 1;

    fprintf(stderr, "dolby: virtual sink '%s' ('Dolby Atmos (DAP)') running "
                    "(%u Hz, %d ch, %u-frame blocks)\n",
            node_name, impl.rate, CH, impl.frames);
    fprintf(stderr, "dolby: output device: %s\n", sink);
    fprintf(stderr, "dolby: select 'Dolby Atmos (DAP)' as your default output device\n");

    pw_main_loop_run(impl.loop);

    uint8_t hdr[12];
    put32(hdr, MAGIC); put32(hdr + 4, CMD_BYE); put32(hdr + 8, 0);
    write_full(impl.fd, hdr, sizeof(hdr));
    close(impl.fd);
    fprintf(stderr, "dolby: processed %llu blocks\n", impl.blocks);
    pw_stream_destroy(impl.playback);
    pw_stream_destroy(impl.sink);
    pw_proxy_destroy((struct pw_proxy *)impl.registry);
    pw_core_disconnect(impl.core);
    pw_context_destroy(impl.ctx);
    pw_main_loop_destroy(impl.loop);
    pw_deinit();
    (void)sig_int;
    (void)sig_term;
    return 0;
}
