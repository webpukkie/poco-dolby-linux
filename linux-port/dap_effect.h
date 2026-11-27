/* dap_effect.h — reusable wrapper that drives the Dolby DAP (libswdap.so)
 * through the AOSP audio-effect interface on Linux/glibc.
 *
 * Lifecycle per instance (mirrors what the Android audio HAL does):
 *   create() -> INIT -> SET_CONFIG -> SET_DEVICE -> SET_AUDIO_MODE
 *            -> EffectDap::setParam("preg", 0)   [gain-sync, see README]
 *            -> ENABLE
 * and per block:
 *   process(in_scratch, out)  — in MUST be scratch: the library clobbers the
 *   input buffer once the crossfade is active (see docs/RE-notes.md).
 */
#ifndef DAP_EFFECT_H
#define DAP_EFFECT_H

#include <stdint.h>

typedef struct dap_effect dap_effect_t;

/* dlopen stage/libswdap.so (+ extra_stubs.so) and apply the APS2 packed
 * relocations.  stage_dir defaults to "./stage".  Returns 0 on success. */
int dap_engine_open(const char *stage_dir);
void dap_engine_close(void);

/* Run the full command lifecycle.  Returns 0 on success (*e set), else the
 * failing step code printed to stderr. */
dap_effect_t *dap_effect_new(int rate, int ch, int fmtid, int frames,
                             int enable);
int dap_effect_set_enable(dap_effect_t *e, int enable);
/* in must hold frames*ch samples (bps = dap_effect_bps(fmtid)); out receives
 * the processed audio.  Returns the library's rc (0 = ok). */
int dap_effect_process(dap_effect_t *e, void *in, void *out);
void dap_effect_free(dap_effect_t *e);

int dap_effect_bps(int fmtid);

#endif
