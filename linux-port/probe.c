/* probe.c — self-test for the Dolby DAP wrapper (dap_effect.c): drives
 * libswdap.so through the AOSP audio-effect interface on Linux, under
 * qemu-aarch64 user-mode emulation, with a synthetic test tone.
 *
 * Build: see build.sh.  Usage:
 *   ./probe [rate] [ch] [format] [enable] [frames] [blocks]
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "dap_effect.h"

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int rate   = argc > 1 ? atoi(argv[1]) : 48000;
    int ch     = argc > 2 ? atoi(argv[2]) : 2;
    int fmtid  = argc > 3 ? atoi(argv[3]) : 1;   /* audio_format_t */
    int enable = argc > 4 ? atoi(argv[4]) : 1;
    int frames = argc > 5 ? atoi(argv[5]) : 512;
    int blocks = argc > 6 ? atoi(argv[6]) : 50;

    if (dap_engine_open(NULL)) return 1;

    dap_effect_t *e = dap_effect_new(rate, ch, fmtid, frames, enable);
    if (!e) return 2;
    printf("lifecycle ok in(%d,%d,%d)\n", rate, ch, fmtid);

    int bps = dap_effect_bps(fmtid);
    int16_t *ib = calloc(frames * ch, bps);
    int16_t *ob = calloc(frames * ch, bps);
    if (!ib || !ob) return 3;

    int failures = 0;
    for (int b = 0; b < blocks; b++) {
        /* continuous-phase test tone */
        for (int i = 0; i < frames * ch; i++)
            ib[i] = (int16_t)(8000.0 * sin(2 * M_PI * 500.0 * (b * frames + i / ch) / rate));
        int r = dap_effect_process(e, ib, ob);
        if (r != 0) failures++;
        if (b < 3 || b == blocks - 1) {
            long nz = 0; double e2 = 0;
            for (int i = 0; i < frames * ch; i++) {
                if (ob[i]) nz++;
                e2 += (double)ob[i] * ob[i];
            }
            printf("blk%d rc=%d nz=%ld rms_out=%.1f ib0=%d ob0=%d\n",
                   b, r, nz, sqrt(e2 / (frames * ch)), ib[0], ob[0]);
        }
    }
    printf("DONE blocks=%d failures=%d\n", blocks, failures);
    dap_effect_free(e);
    return failures ? 4 : 0;
}
