/* pw-dolby.c — PipeWire filter node that pipes audio through the Dolby DAP
 * (Dolby Atmos / DAP2) via the dolbyd RPC daemon (aarch64 libswdap.so under
 * qemu-aarch64, or native on arm64).
 *
 * Graph position:  [any source] -> dolby_atmos (this node) -> [sink]
 *
 * Build: ./build-pw.sh        Run: ./run-pw-dolby.sh
 * Then link it in your player or with pw-link:
 *   pw-link <source-output_FL> dolby_atmos:input_FL
 *   pw-link <source-output_FR> dolby_atmos:input_FR
 *   pw-link dolby_atmos:output_FL <sink-playback_FL>
 *   pw-link dolby_atmos:output_FR <sink-playback_FR>
 *
 * Format: ports exchange f32 (the PipeWire graph's native format — pw_filter
 * ports get no automatic converter, so offering s16 would make every link
 * fail to negotiate); audio is converted to s16 for the DAP engine and back.
 * One port per channel, as PipeWire filters expect; the interleaved block
 * the DAP wants is assembled here, and arbitrary quantum sizes are buffered
 * into engine-sized blocks by small FIFOs.  Note the blocking socket
 * round-trip per block: this adds latency and is not RT-safe; for a
 * production node run dolbyd on arm64 natively (or port the libs to x86_64).
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/audio/format-utils.h>

#define MAGIC 0x31425044u /* 'DBP1' */
enum { CMD_HELLO = 0, CMD_PROCESS = 1, CMD_ENABLE = 2, CMD_DISABLE = 3, CMD_BYE = 4 };

#define MAX_CH 8

struct impl {
    struct pw_main_loop *loop;
    struct pw_filter *filter;
    void *in_port[MAX_CH];
    void *out_port[MAX_CH];
    int fd;
    uint32_t rate, ch, frames;
    uint32_t block;         /* interleaved s16 bytes per engine block */
    uint8_t *out;           /* daemon output staging (one block) */
    uint8_t *fifo_in;  size_t fifo_in_cap, fifo_in_len, fifo_in_off;
    uint8_t *fifo_out; size_t fifo_out_cap, fifo_out_len, fifo_out_off;
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
    put32(p, d->rate); put32(p + 4, d->ch); put32(p + 8, 1 /* s16 */); put32(p + 12, d->frames);
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

static void on_process(void *userdata, struct spa_io_position *position) {
    struct impl *d = userdata;
    struct pw_buffer *in_b[MAX_CH] = { 0 }, *out_b[MAX_CH] = { 0 };
    struct spa_buffer *inb[MAX_CH] = { 0 }, *outb[MAX_CH] = { 0 };
    int ok = 1;
    for (uint32_t c = 0; c < d->ch; c++) {
        if (!(in_b[c] = pw_filter_dequeue_buffer(d->in_port[c])) ||
            !(out_b[c] = pw_filter_dequeue_buffer(d->out_port[c])))
            ok = 0;
        if (ok) { inb[c] = in_b[c]->buffer; outb[c] = out_b[c]->buffer; }
    }

    uint32_t n_frames = ok ? inb[0]->datas[0].chunk->size / 4 : 0; /* f32 mono plane */

    if (ok && n_frames > 0) {
        /* f32 planes -> interleaved s16, appended to the input FIFO */
        size_t add = (size_t)n_frames * d->ch * 2;
        uint8_t *fi = buf_grow(d->fifo_in, d->fifo_in_len + add, &d->fifo_in_cap);
        if (!fi) ok = 0;
        else {
            int16_t *s = (int16_t *)(fi + d->fifo_in_len);
            for (uint32_t i = 0; i < n_frames; i++)
                for (uint32_t c = 0; c < d->ch; c++) {
                    const float *src = (const float *)
                        (inb[c]->datas[0].data + inb[c]->datas[0].chunk->offset);
                    s[i * d->ch + c] = f2s(src[i]);
                }
            d->fifo_in = fi;
            d->fifo_in_len += add;
        }
    }
    if (ok && n_frames > 0) {
        /* push whole engine blocks through the DAP */
        while (d->fifo_in_len - d->fifo_in_off >= d->block) {
            int rc = daemon_process(d, d->fifo_in + d->fifo_in_off, d->out);
            if (rc != 0) {
                fprintf(stderr, "dolby: process rc=%d, emitting silence\n", rc);
                d->fifo_in_off = d->fifo_in_len = 0;
                ok = 0;
                break;
            }
            uint8_t *fo = buf_grow(d->fifo_out, d->fifo_out_len + d->block, &d->fifo_out_cap);
            if (!fo) { ok = 0; break; }
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

    uint32_t emit = 0;
    if (ok && n_frames > 0) {
        size_t avail = d->fifo_out_len - d->fifo_out_off;
        emit = n_frames;
        if (avail < (size_t)emit * d->ch * 2)
            emit = (uint32_t)(avail / (d->ch * 2));
        for (uint32_t c = 0; c < d->ch; c++) {
            float *dst = (float *)outb[c]->datas[0].data;
            const int16_t *o = (const int16_t *)(d->fifo_out + d->fifo_out_off);
            for (uint32_t i = 0; i < emit; i++)
                dst[i] = (float)o[i * d->ch + c] / 32768.0f;
            for (uint32_t i = emit; i < n_frames; i++)
                dst[i] = 0.0f;
        }
        d->fifo_out_off += (size_t)emit * d->ch * 2;
        if (d->fifo_out_off) {
            memmove(d->fifo_out, d->fifo_out + d->fifo_out_off, d->fifo_out_len - d->fifo_out_off);
            d->fifo_out_len -= d->fifo_out_off;
            d->fifo_out_off = 0;
        }
    } else {
        for (uint32_t c = 0; c < d->ch; c++)
            if (outb[c]) memset(outb[c]->datas[0].data, 0, n_frames * 4);
    }

    for (uint32_t c = 0; c < d->ch; c++) {
        if (outb[c]) {
            outb[c]->datas[0].chunk->offset = 0;
            outb[c]->datas[0].chunk->size = n_frames * 4;
            outb[c]->datas[0].chunk->stride = 4;
        }
        if (in_b[c]) pw_filter_queue_buffer(d->in_port[c], in_b[c]);
        if (out_b[c]) pw_filter_queue_buffer(d->out_port[c], out_b[c]);
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

static const char *CH_NAMES[8] = { "FL", "FR", "FC", "LFE", "SL", "SR", "RL", "RR" };

static int add_channel_port(struct impl *d, enum pw_direction dir, uint32_t c) {
    struct pw_properties *props = pw_properties_new(
        PW_KEY_PORT_NAME, NULL,
        PW_KEY_AUDIO_CHANNEL, CH_NAMES[c],
        NULL);
    char name[64];
    snprintf(name, sizeof(name), "%s_%s", dir == PW_DIRECTION_INPUT ? "input" : "output",
             CH_NAMES[c]);
    pw_properties_set(props, PW_KEY_PORT_NAME, name);

    uint8_t podbuf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
    const struct spa_pod *params[1];
    struct spa_audio_info_raw raw = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = d->rate,
        .channels = 1);
    raw.position[0] = (enum spa_audio_channel)(SPA_AUDIO_CHANNEL_START_Aux + 0);
    /* use the proper channel position where we know it */
    switch (c) {
    case 0: raw.position[0] = SPA_AUDIO_CHANNEL_FL; break;
    case 1: raw.position[0] = SPA_AUDIO_CHANNEL_FR; break;
    case 2: raw.position[0] = SPA_AUDIO_CHANNEL_FC; break;
    case 3: raw.position[0] = SPA_AUDIO_CHANNEL_LFE; break;
    case 4: raw.position[0] = SPA_AUDIO_CHANNEL_SL; break;
    case 5: raw.position[0] = SPA_AUDIO_CHANNEL_SR; break;
    case 6: raw.position[0] = SPA_AUDIO_CHANNEL_RL; break;
    case 7: raw.position[0] = SPA_AUDIO_CHANNEL_RR; break;
    }
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

    void *pd = pw_filter_add_port(d->filter, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                  0, props, params, 1);
    if (dir == PW_DIRECTION_INPUT) d->in_port[c] = pd; else d->out_port[c] = pd;
    return pd ? 0 : -1;
}

static void do_quit(void *userdata, int signal_number) {
    struct impl *d = userdata;
    pw_main_loop_quit(d->loop);
}

int main(int argc, char **argv) {
    struct impl impl = { .fd = -1, .rate = 48000, .ch = 2, .frames = 256 };
    const char *socket_path = "/tmp/dolbyd.sock";
    const char *node_name = "dolby_atmos";
    const char *remote = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) impl.rate = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc) impl.ch = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) impl.frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_path = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) node_name = argv[++i];
        else if (!strcmp(argv[i], "--remote") && i + 1 < argc) remote = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (impl.ch < 1 || impl.ch > MAX_CH) { fprintf(stderr, "channels must be 1..8\n"); return 2; }
    impl.block = (size_t)impl.frames * impl.ch * 2;
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
    fprintf(stderr, "dolby: session up (rate=%u ch=%u frames=%u)\n",
            impl.rate, impl.ch, impl.frames);

    pw_init(&argc, &argv);
    impl.loop = pw_main_loop_new(NULL);
    if (!impl.loop) { fprintf(stderr, "pw_main_loop_new failed\n"); return 1; }
    struct pw_loop *l = pw_main_loop_get_loop(impl.loop);
    struct spa_source *sig_int = pw_loop_add_signal(l, SIGINT, do_quit, &impl);
    struct spa_source *sig_term = pw_loop_add_signal(l, SIGTERM, do_quit, &impl);

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, node_name,
        PW_KEY_NODE_DESCRIPTION, "Dolby Atmos (DAP)",
        PW_KEY_NODE_LATENCY, NULL, /* set below */
        NULL);
    char latency[32];
    snprintf(latency, sizeof(latency), "%u/%u", impl.frames, impl.rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency);
    if (remote) pw_properties_set(props, PW_KEY_REMOTE_NAME, remote);

    impl.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(impl.loop), node_name, props, &filter_events, &impl);
    if (!impl.filter) { fprintf(stderr, "pw_filter_new_simple failed\n"); return 1; }

    for (uint32_t c = 0; c < impl.ch; c++) {
        if (add_channel_port(&impl, PW_DIRECTION_INPUT, c) ||
            add_channel_port(&impl, PW_DIRECTION_OUTPUT, c)) {
            fprintf(stderr, "pw_filter_add_port failed\n");
            return 1;
        }
    }

    if (pw_filter_connect(impl.filter, 0, NULL, 0) < 0) {
        fprintf(stderr, "pw_filter_connect failed\n");
        return 1;
    }
    fprintf(stderr, "dolby: node '%s' running (%u Hz, s16, %u ch, %u-frame blocks)\n",
            node_name, impl.rate, impl.ch, impl.frames);
    pw_main_loop_run(impl.loop);

    uint8_t hdr[12];
    put32(hdr, MAGIC); put32(hdr + 4, CMD_BYE); put32(hdr + 8, 0);
    write_full(impl.fd, hdr, sizeof(hdr));
    close(impl.fd);
    fprintf(stderr, "dolby: processed %llu blocks\n", impl.blocks);
    (void)sig_int;
    (void)sig_term;
    return 0;
}
