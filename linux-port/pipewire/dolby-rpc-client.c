/* dolby-rpc-client.c — native test client for dolbyd: raw s16le PCM on
 * stdin, DAP-processed PCM on stdout.
 *
 *   ./run-daemon.sh &
 *   ffmpeg -i in.wav -f s16le -ar 48000 -ac 2 - | ./dolby-rpc-client 48000 2 > out.raw
 *   ffmpeg -f s16le -ar 48000 -ac 2 -i out.raw out.wav
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAGIC 0x31425044u /* 'DBP1' */

enum { CMD_HELLO = 0, CMD_PROCESS = 1, CMD_ENABLE = 2, CMD_DISABLE = 3, CMD_BYE = 4 };

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

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

static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= k;
    }
    return 0;
}

static int rpc(int fd, uint32_t cmd, const uint8_t *payload, uint32_t len,
               uint8_t *reply, uint32_t reply_len) {
    uint8_t hdr[12];
    put32(hdr, MAGIC); put32(hdr + 4, cmd); put32(hdr + 8, len);
    if (write_full(fd, hdr, sizeof(hdr))) return -1;
    if (len && write_full(fd, payload, len)) return -1;
    if (reply_len && read_full(fd, reply, reply_len)) return -1;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = "/tmp/dolbyd.sock";
    uint32_t rate = argc > 1 ? (uint32_t)atoi(argv[1]) : 48000;
    uint32_t ch = argc > 2 ? (uint32_t)atoi(argv[2]) : 2;
    uint32_t frames = argc > 3 ? (uint32_t)atoi(argv[3]) : 512;
    uint32_t fmtid = 1; /* s16 */
    size_t block = (size_t)frames * ch * 2;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect (is dolbyd running?)"); return 1;
    }

    uint8_t p[16], rep[4];
    put32(p, rate); put32(p + 4, ch); put32(p + 8, fmtid); put32(p + 12, frames);
    if (rpc(fd, CMD_HELLO, p, 16, rep, 4) || get32(rep) != 0) {
        fprintf(stderr, "client: HELLO failed (status=%u)\n", get32(rep));
        return 1;
    }
    fprintf(stderr, "client: session up (rate=%u ch=%u frames=%u)\n", rate, ch, frames);

    uint8_t *in = malloc(block), *out = malloc(block);
    long long blocks = 0;
    for (;;) {
        size_t got = fread(in, 1, block, stdin);
        if (got == 0) break;
        if (got < block) memset(in + got, 0, block - got); /* pad final partial block */
        if (rpc(fd, CMD_PROCESS, in, block, rep, 4)) { perror("client: PROCESS"); return 1; }
        if (read_full(fd, out, block)) { fprintf(stderr, "client: short reply\n"); return 1; }
        if (get32(rep) != 0) {
            fprintf(stderr, "client: PROCESS rc=%u at block %lld\n", get32(rep), blocks);
            return 1;
        }
        if (fwrite(out, 1, got, stdout) != got) return 1;
        blocks++;
    }
    rpc(fd, CMD_BYE, NULL, 0, NULL, 0);
    fprintf(stderr, "client: done blocks=%lld\n", blocks);
    return 0;
}
