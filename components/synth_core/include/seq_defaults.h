#pragma once

#include "display_seq.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_SEQ_MELODIC_ENV_EG0_TYPE
#define CONFIG_SEQ_MELODIC_ENV_EG0_TYPE 0
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_ATTACK_MS
#define CONFIG_SEQ_MELODIC_ENV_ATTACK_MS 12
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_DECAY_MS
#define CONFIG_SEQ_MELODIC_ENV_DECAY_MS 220
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT
#define CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT 58
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_RELEASE_MS
#define CONFIG_SEQ_MELODIC_ENV_RELEASE_MS 280
#endif

static inline seq_env_t seq_default_melodic_env(void)
{
    return (seq_env_t) {
        .eg_type     = CONFIG_SEQ_MELODIC_ENV_EG0_TYPE,
        .attack_ms   = CONFIG_SEQ_MELODIC_ENV_ATTACK_MS,
        .decay_ms    = CONFIG_SEQ_MELODIC_ENV_DECAY_MS,
        .sustain_pct = CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT,
        .release_ms  = CONFIG_SEQ_MELODIC_ENV_RELEASE_MS,
    };
}

#ifdef __cplusplus
}
#endif
