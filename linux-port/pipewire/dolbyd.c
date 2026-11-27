/* dolbyd.c — RPC daemon that drives the Dolby DAP (libswdap.so) under
 * qemu-aarch64 on Linux.  The native PipeWire filter node (pw-dolby.c)
 * connects over a Unix socket and shuttles PCM blocks through it.
 *
 * Protocol (all integers little-endian u32):
 *   request header: { magic 'DBP1', cmd, payload_len }
 *   cmds:
 *     0 HELLO   payload { rate, ch, fmt, frames }   reply { status }
 *     1 PROCESS payload raw PCM (frames*ch*bps)     reply { status } + PCM
 *     2 ENABLE  payload none                        reply { status }
 *     3 DISABLE payload none                        reply { status }
 *     4 BYE     payload none                        (no reply, close)
 * The effect is created per connection with the HELLO parameters.
 *
 * Build (aarch64, via build.sh): dolbyd
 * Run: ./run-daemon.sh [socket-path]   (default /tmp/dolbyd.sock)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../dap_effect.h"

#define MAGIC 0x31425044u /* 'DBP1' */

enum { CMD_HELLO = 0, CMD_PROCESS = 1, CMD_ENABLE = 2, CMD_DISABLE = 3, CMD_BYE = 4 };

struct hdr { uint32_t magic, cmd, len; };

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) return 1; /* eof */
        p += k; n -= k;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= k;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int serve_client(int cfd) {
    dap_effect_t *e = NULL;
    uint32_t rate = 0, ch = 2, fmtid = 1, frames = 0;
    void *scratch = NULL, *out = NULL;
    size_t block = 0;
    int ret = 0;

    for (;;) {
        struct hdr h;
        int rc = read_full(cfd, &h, sizeof(h));
        if (rc == 1) break;                 /* client closed */
        if (rc < 0 || h.magic != MAGIC) { fprintf(stderr, "dolbyd: bad request\n"); break; }

        if (h.cmd == CMD_HELLO && h.len == 16) {
            uint8_t p[16];
            if (read_full(cfd, p, 16)) break;
            rate = get32(p); ch = get32(p + 4); fmtid = get32(p + 8); frames = get32(p + 12);
            free(scratch); free(out);
            block = (size_t)frames * ch * dap_effect_bps(fmtid);
            scratch = malloc(block); out = malloc(block);
            if (!scratch || !out) { ret = 1; break; }
            dap_effect_free(e);
            e = dap_effect_new(rate, ch, fmtid, frames, 1);
            uint8_t rep[4];
            put32(rep, e ? 0 : 1);
            if (write_full(cfd, rep, 4)) break;
            if (!e) { fprintf(stderr, "dolbyd: effect init failed\n"); break; }
            fprintf(stderr, "dolbyd: session rate=%u ch=%u fmt=%u frames=%u\n",
                    rate, ch, fmtid, frames);
        } else if (h.cmd == CMD_PROCESS && e && h.len == block) {
            if (read_full(cfd, scratch, block)) break;
            int r = dap_effect_process(e, scratch, out);
            uint8_t rep[4];
            put32(rep, (uint32_t)r);
            if (write_full(cfd, rep, 4) || write_full(cfd, out, block)) break;
        } else if ((h.cmd == CMD_ENABLE || h.cmd == CMD_DISABLE) && e && h.len == 0) {
            int r = dap_effect_set_enable(e, h.cmd == CMD_ENABLE);
            uint8_t rep[4];
            put32(rep, (uint32_t)r);
            if (write_full(cfd, rep, 4)) break;
        } else if (h.cmd == CMD_BYE) {
            break;
        } else {
            fprintf(stderr, "dolbyd: bad cmd=%u len=%u (block=%zu)\n", h.cmd, h.len, block);
            break;
        }
    }
    free(scratch); free(out);
    dap_effect_free(e);
    return ret;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/dolbyd.sock";
    if (dap_engine_open(NULL)) return 1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(fd, 1) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "dolbyd: listening on %s\n", path);

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); return 1; }
        fprintf(stderr, "dolbyd: client connected\n");
        int rc = serve_client(cfd);
        close(cfd);
        fprintf(stderr, "dolbyd: client done rc=%d\n", rc);
        if (rc) return rc;
    }
}
