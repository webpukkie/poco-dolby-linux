#define _GNU_SOURCE
#include "dap_effect.h"

#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aosp_effect_abi.h"
#include "apply_packed.h"

typedef int (*create_t)(const effect_uuid_s *, int, int, effect_interface_s ***);
typedef int (*cmd_t)(effect_interface_s **, uint32_t, uint32_t, void *, uint32_t *, void *);
typedef int (*proc_t)(effect_interface_s *, audio_buffer_s *, audio_buffer_s *);

/* DAP implementation uuid from configs/audio_effects.xml */
static const effect_uuid_s DAP_UUID = {
    0x9d4921da, 0x8225, 0x4f29, 0xaefa, { 0x39, 0x53, 0x7a, 0x04, 0xbc, 0xaa }
};

static void *g_hdl, *g_hdl_stubs;
static create_t create_fn;
static cmd_t cmd_fn;
static proc_t proc_fn;

struct dap_effect {
    effect_interface_s **iface;
    int frames;
    int ch;
    int fmtid;
};

int dap_effect_bps(int fmtid) { return fmtid == 1 ? 2 : 4; }

int dap_engine_open(const char *stage_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/libswdap.so", stage_dir ? stage_dir : "./stage");
    g_hdl = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_hdl) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    snprintf(path, sizeof(path), "%s/extra_stubs.so", stage_dir ? stage_dir : "./stage");
    g_hdl_stubs = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_hdl_stubs) { fprintf(stderr, "dlopen extra_stubs: %s\n", dlerror()); return 1; }

    /* libswdap.so uses Android packed (APS2) relocations that glibc's loader
     * ignores; apply them manually now that we know the load base. */
    struct link_map *lm;
    dlinfo(g_hdl, RTLD_DI_LINKMAP, &lm);
    int nr = apply_packed_relocs((unsigned long)lm->l_addr);
    fprintf(stderr, "dap: base=%lx relocs=%d\n", (unsigned long)lm->l_addr, nr);

    create_fn = (create_t)dlsym(g_hdl, "_ZN5dolby9DlbEffect6createEPK12audio_uuid_siiPPP18effect_interface_s");
    cmd_fn = (cmd_t)dlsym(g_hdl, "_ZN5dolby9DlbEffect13effectCommandEPP18effect_interface_sjjPvPjS4_");
    proc_fn = (proc_t)dlsym(g_hdl, "_ZN5dolby9DlbEffect13effectProcessEPP18effect_interface_sP14audio_buffer_sS5_");
    if (!create_fn || !cmd_fn || !proc_fn) { fprintf(stderr, "dap: dlsym failed\n"); return 1; }
    return 0;
}

void dap_engine_close(void) {
    if (g_hdl) dlclose(g_hdl);
    if (g_hdl_stubs) dlclose(g_hdl_stubs);
    g_hdl = g_hdl_stubs = NULL;
}

/* Gain-sync: align EffectDap's lastGain mirror (+0x208) with the value
 * getParam() returns for the (still empty) parameter cache, so the first
 * process() does not run the pregain-priming path.  Priming would call
 * crossfade->setEnabled(0), which moves DlbCrossfadeProcess from
 * PREROLL_ACTIVE to PROCESS_BYPASSED, and the effect then returns -ENOSYS
 * on every block forever (see docs/RE-notes.md).
 * Object walk: iface = DlbDlbEffect base, +8 = EffectContext*, ctx+24 = EffectDap*. */
static int gain_sync(effect_interface_s **iface) {
    typedef int (*dsetparam_t)(void *, uint32_t, int, const int *, int);
    dsetparam_t dset = (dsetparam_t)dlsym(g_hdl, "_ZN5dolby9EffectDap8setParamE15audio_devices_tiPKii");
    void *ctx = *(void **)((char *)iface + 8);
    void *dap = ctx ? *(void **)((char *)ctx + 24) : NULL;
    if (!ctx || !dap || !dset) return -1;
    int v = 0;
    return dset(dap, 0x40000000u, 0x67657270u /* "preg" */, &v, 1);
}

dap_effect_t *dap_effect_new(int rate, int ch, int fmtid, int frames, int enable) {
    dap_effect_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->ch = ch; e->fmtid = fmtid; e->frames = frames;

    effect_interface_s **iface = NULL;
    int rc = create_fn(&DAP_UUID, 0, 0, &iface);
    if (rc != 0 || !iface) { fprintf(stderr, "dap: create rc=%d\n", rc); free(e); return NULL; }
    e->iface = iface;

    uint32_t rsz = sizeof(int);
    int r = 0, dummy = 0;
    r = cmd_fn(iface, 0, 4, &dummy, &rsz, &r);              /* EFFECT_CMD_INIT */
    if (r) { fprintf(stderr, "dap: INIT rc=%d\n", r); goto fail; }

    effect_config_s cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.inputCfg.buffer.frameCount = frames;
    cfg.inputCfg.samplingRate = rate;
    cfg.inputCfg.channels = 3;                              /* AUDIO_CHANNEL_OUT_STEREO */
    cfg.inputCfg.format = fmtid;
    cfg.inputCfg.accessMode = 1;
    cfg.inputCfg.mask = 0xff;
    cfg.outputCfg = cfg.inputCfg;
    rsz = sizeof(int);
    r = cmd_fn(iface, 1, sizeof(cfg), &cfg, &rsz, &r);      /* EFFECT_CMD_SET_CONFIG */
    if (r) { fprintf(stderr, "dap: SET_CONFIG rc=%d\n", r); goto fail; }

    uint32_t dev = 0x2;                                     /* AUDIO_DEVICE_OUT_SPEAKER */
    rsz = sizeof(int);
    r = cmd_fn(iface, 9, 4, &dev, &rsz, &r);                /* EFFECT_CMD_SET_DEVICE */
    if (r) { fprintf(stderr, "dap: SET_DEVICE rc=%d\n", r); goto fail; }

    int audiomode = 0;                                      /* AUDIO_MODE_NORMAL */
    rsz = sizeof(int);
    r = cmd_fn(iface, 11, 4, &audiomode, &rsz, &r);         /* EFFECT_CMD_SET_AUDIO_MODE */
    if (r) { fprintf(stderr, "dap: SET_AUDIO_MODE rc=%d\n", r); goto fail; }

    r = gain_sync(iface);
    if (r) { fprintf(stderr, "dap: gain-sync rc=%d\n", r); goto fail; }

    if (enable) {
        rsz = sizeof(int);
        r = cmd_fn(iface, 3, 0, NULL, &rsz, &r);            /* EFFECT_CMD_ENABLE */
        if (r) { fprintf(stderr, "dap: ENABLE rc=%d\n", r); goto fail; }
    }
    return e;
fail:
    free(e);
    return NULL;
}

int dap_effect_set_enable(dap_effect_t *e, int enable) {
    uint32_t rsz = sizeof(int);
    int r = 0;
    return cmd_fn(e->iface, enable ? 3 : 4, 0, NULL, &rsz, &r); /* ENABLE/DISABLE */
}

int dap_effect_process(dap_effect_t *e, void *in, void *out) {
    audio_buffer_s bin = { (uint32_t)e->frames, { .raw = in } };
    audio_buffer_s bout = { (uint32_t)e->frames, { .raw = out } };
    return proc_fn((effect_interface_s *)e->iface, &bin, &bout);
}

void dap_effect_free(dap_effect_t *e) { free(e); }
