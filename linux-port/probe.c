/* probe.c — drive the Dolby DAP (libswdap.so) through the AOSP audio-effect
 * interface on Linux, under qemu-aarch64 user-mode emulation.
 *
 * Sequence (mirrors what the Android audio HAL does):
 *   create() -> INIT -> SET_CONFIG -> SET_DEVICE -> SET_AUDIO_MODE
 *            -> EffectDap::setParam("preg", 0)   [gain-sync, see README]
 *            -> ENABLE -> GET_CONFIG -> N x process()
 *
 * Build: see build.sh.  Usage:
 *   ./probe [rate] [ch] [format] [enable] [frames] [blocks]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <math.h>
#include <link.h>
#include "aosp_effect_abi.h"
#include "apply_packed.h"

typedef int (*create_t)(const effect_uuid_s *, int, int, effect_interface_s ***);
typedef int (*cmd_t)(effect_interface_s **, uint32_t, uint32_t, void *, uint32_t *, void *);
typedef int (*proc_t)(effect_interface_s *, audio_buffer_s *, audio_buffer_s *);

/* DAP implementation uuid from configs/audio_effects.xml */
static const effect_uuid_s DAP_UUID = {
    0x9d4921da, 0x8225, 0x4f29, 0xaefa, { 0x39, 0x53, 0x7a, 0x04, 0xbc, 0xaa }
};

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int rate   = argc > 1 ? atoi(argv[1]) : 48000;
    int ch     = argc > 2 ? atoi(argv[2]) : 2;
    int fmtid  = argc > 3 ? atoi(argv[3]) : 1;   /* audio_format_t */
    int enable = argc > 4 ? atoi(argv[4]) : 1;
    int frames = argc > 5 ? atoi(argv[5]) : 512;
    int blocks = argc > 6 ? atoi(argv[6]) : 50;

    void *h = dlopen("./stage/libswdap.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void *hs = dlopen("./stage/extra_stubs.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hs) { fprintf(stderr, "dlopen extra_stubs: %s\n", dlerror()); return 1; }

    /* libswdap.so uses Android packed (APS2) relocations that glibc's loader
     * ignores; apply them manually now that we know the load base. */
    struct link_map *lm;
    dlinfo(h, RTLD_DI_LINKMAP, &lm);
    unsigned long base = (unsigned long)lm->l_addr;
    int nr = apply_packed_relocs(base);
    printf("base=%lx relocs=%d\n", base, nr);

    create_t create = dlsym(h, "_ZN5dolby9DlbEffect6createEPK12audio_uuid_siiPPP18effect_interface_s");
    cmd_t command = dlsym(h, "_ZN5dolby9DlbEffect13effectCommandEPP18effect_interface_sjjPvPjS4_");
    proc_t process = dlsym(h, "_ZN5dolby9DlbEffect13effectProcessEPP18effect_interface_sP14audio_buffer_sS5_");
    if (!create || !command || !process) { fprintf(stderr, "dlsym failed\n"); return 1; }

    effect_interface_s **iface = NULL;
    int rc = create(&DAP_UUID, 0, 0, &iface);
    printf("create rc=%d iface=%p\n", rc, (void *)iface);
    if (rc != 0 || !iface) return 2;

    uint32_t rsz = sizeof(int);
    int r = 0, dummy = 0;
    r = command(iface, 0, 4, &dummy, &rsz, &r);            /* EFFECT_CMD_INIT */
    printf("INIT -> %d\n", r);

    effect_config_s cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.inputCfg.buffer.frameCount = frames;
    cfg.inputCfg.samplingRate = rate;
    cfg.inputCfg.channels = 3;                              /* AUDIO_CHANNEL_OUT_STEREO */
    cfg.inputCfg.format = fmtid;
    cfg.inputCfg.accessMode = 1;
    cfg.inputCfg.mask = 0xff;
    cfg.outputCfg = cfg.inputCfg;
    rsz = sizeof(int);
    r = command(iface, 1, sizeof(cfg), &cfg, &rsz, &r);     /* EFFECT_CMD_SET_CONFIG */
    printf("SET_CONFIG -> %d\n", r);

    uint32_t dev = 0x2;                                     /* AUDIO_DEVICE_OUT_SPEAKER */
    rsz = sizeof(int);
    r = command(iface, 9, 4, &dev, &rsz, &r);               /* EFFECT_CMD_SET_DEVICE */
    printf("SET_DEVICE -> %d\n", r);

    int audiomode = 0;                                      /* AUDIO_MODE_NORMAL */
    rsz = sizeof(int);
    r = command(iface, 11, 4, &audiomode, &rsz, &r);        /* EFFECT_CMD_SET_AUDIO_MODE */
    printf("SET_AUDIO_MODE -> %d\n", r);

    /* Gain-sync: align EffectDap's lastGain mirror (+0x208) with the value
     * getParam() returns for the (still empty) parameter cache, so the
     * first process() does not run the pregain-priming path.  Priming would
     * call crossfade->setEnabled(0), which moves DlbCrossfadeProcess from
     * PREROLL_ACTIVE to PROCESS_BYPASSED, and the effect then returns
     * -ENOSYS on every block forever (see linux-port/docs/RE-notes.md).
     * Object walk: iface = DlbDlbEffect base, +8 = EffectContext*,
     * ctx+24 = EffectDap*. */
    {
        typedef int (*dsetparam_t)(void *, uint32_t, int, const int *, int);
        dsetparam_t dset = (dsetparam_t)dlsym(h, "_ZN5dolby9EffectDap8setParamE15audio_devices_tiPKii");
        void *ctx = *(void **)((char *)iface + 8);
        void *dap = ctx ? *(void **)((char *)ctx + 24) : NULL;
        if (ctx && dap && dset) {
            int v = 0;
            r = dset(dap, 0x40000000u, 0x67657270u /* "preg" */, &v, 1);
            printf("gain-sync setParam(preg,0) -> %d\n", r);
        }
    }

    if (enable) {
        rsz = sizeof(int);
        r = command(iface, 3, 0, NULL, &rsz, &r);           /* EFFECT_CMD_ENABLE */
        printf("ENABLE -> %d\n", r);
    }

    rsz = sizeof(cfg);
    r = command(iface, 14, sizeof(cfg), &cfg, &rsz, &cfg);  /* EFFECT_CMD_GET_CONFIG */
    printf("GET_CONFIG -> %d in(%u,%u,%u) out(%u,%u,%u)\n", r,
        cfg.inputCfg.samplingRate, cfg.inputCfg.channels, cfg.inputCfg.format,
        cfg.outputCfg.samplingRate, cfg.outputCfg.channels, cfg.outputCfg.format);

    int bps = fmtid == 1 ? 2 : 4;
    int16_t *ib = calloc(frames * ch, bps);
    int16_t *ob = calloc(frames * ch, bps);
    if (!ib || !ob) return 3;

    int failures = 0;
    for (int b = 0; b < blocks; b++) {
        /* continuous-phase test tone */
        for (int i = 0; i < frames * ch; i++)
            ib[i] = (int16_t)(8000.0 * sin(2 * M_PI * 500.0 * (b * frames + i / ch) / rate));
        {
            audio_buffer_s bin = { frames, { .raw = ib } }, bout = { frames, { .raw = ob } };
            r = process((effect_interface_s *)iface, &bin, &bout);
        }
        if (r != 0) failures++;
        if (b < 3 || b == blocks - 1) {
            long nz = 0; double e = 0;
            for (int i = 0; i < frames * ch; i++) {
                if (ob[i]) nz++;
                e += (double)ob[i] * ob[i];
            }
            printf("blk%d rc=%d nz=%ld rms_out=%.1f ib0=%d ob0=%d\n",
                   b, r, nz, sqrt(e / (frames * ch)), ib[0], ob[0]);
        }
    }
    printf("DONE blocks=%d failures=%d\n", blocks, failures);
    (void)dummy;
    return failures ? 4 : 0;
}
