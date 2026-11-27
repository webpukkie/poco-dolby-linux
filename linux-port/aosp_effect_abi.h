/* Minimal AOSP audio-effect ABI (64-bit Android), enough to drive DlbEffect. */
#ifndef AOSP_EFFECT_ABI_H
#define AOSP_EFFECT_ABI_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint16_t clockSeq;
    uint8_t  node[6];
} effect_uuid_s;

typedef struct {
    size_t frameCount;
    union { void *raw; int16_t *s16; int32_t *s32; float *f32; };
} audio_buffer_s;

typedef struct effect_buffer_provider_s {
    int32_t (*getBuffer)(struct effect_buffer_provider_s *, audio_buffer_s *);
    int32_t (*releaseBuffer)(struct effect_buffer_provider_s *, audio_buffer_s *);
    void *cookie;
} buffer_provider_s;

typedef struct {
    audio_buffer_s buffer;
    uint32_t samplingRate;
    uint32_t channels;
    buffer_provider_s bufferProvider;
    uint8_t format;
    uint8_t accessMode;
    uint16_t mask;
} buffer_config_s;

typedef struct {
    buffer_config_s inputCfg;
    buffer_config_s outputCfg;
} effect_config_s;

typedef struct effect_interface_s effect_interface_s;
struct effect_interface_s {
    int32_t (*process)(effect_interface_s *self, audio_buffer_s *in, audio_buffer_s *out);
    int32_t (*command)(effect_interface_s **self, uint32_t cmdCode, uint32_t cmdSize,
                       void *cmdData, uint32_t *replySize, void *replyData);
    int32_t (*get_descriptor)(effect_interface_s **self, void *pDesc);
};

/* effect_command_e (AOSP hardware/audio_effect.h) */
enum {
    EFFECT_CMD_INIT = 0,
    EFFECT_CMD_SET_CONFIG = 1,
    EFFECT_CMD_GET_CONFIG = 2,
    EFFECT_CMD_RESET = 3,
    EFFECT_CMD_ENABLE = 4,
    EFFECT_CMD_DISABLE = 5,
    EFFECT_CMD_SET_VOLUME = 6,
    EFFECT_CMD_SET_DEVICE = 7,
    EFFECT_CMD_SET_PARAM = 8,
    EFFECT_CMD_SET_PARAM_COMMIT = 9,
    EFFECT_CMD_SET_PARAM_RELEASE = 10,
    EFFECT_CMD_GET_PARAM = 11,
    EFFECT_CMD_SET_AUDIO_MODE = 12,
    EFFECT_CMD_SET_AUDIO_HAC = 19,
};

/* audio_format_t */
#define AUDIO_FORMAT_PCM_16_BIT 0x00000001u
#define AUDIO_FORMAT_PCM_32_BIT 0x00000003u
#define AUDIO_FORMAT_PCM_FLOAT  0x00000005u

#endif
