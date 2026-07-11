#pragma once
#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Filter type indices — same numeric values as AMY's FILTER_* defines. */
#define FGRAPH_FILTER_NONE  0
#define FGRAPH_FILTER_LPF   1
#define FGRAPH_FILTER_BPF   2
#define FGRAPH_FILTER_HPF   3
#define FGRAPH_FILTER_LPF24 4
#define FGRAPH_FILTER_COUNT 5

/* Frequency + resonance mapping constants (used by synth_ui for en/de-normalise). */
#define FGRAPH_CUTOFF_HZ_MIN   20.0f
#define FGRAPH_CUTOFF_HZ_MAX 8000.0f
#define FGRAPH_RES_MIN  0.51f
#define FGRAPH_RES_MAX  8.0f

/* Data passed from synth_ui (which owns synth_core access) to the renderer.
 * All values are normalised or primitive — the display component stays free
 * of any synth_core or AMY dependency. */
typedef struct {
    uint8_t filter_type;     /* FGRAPH_FILTER_* */
    float   cutoff_norm;     /* 0..1 log-mapped over [FGRAPH_CUTOFF_HZ_MIN, MAX] */
    float   resonance_norm;  /* 0..1 mapped from [FGRAPH_RES_MIN, FGRAPH_RES_MAX] */
    uint8_t cursor;          /* 0=cutoff, 1=resonance, 2=type, 3=enable */
    bool    editing;         /* cursor is currently being adjusted */
    bool    enabled;         /* false → draw flat line + "OFF" */
    char    label[16];       /* left side of top bar, e.g. "L1 T2" or "ARP" */
} filter_graph_t;

/* Draw the full-screen filter editor (clears + sends the buffer).
 * Top bar Y 0-15, divider, frequency response plot Y 16-63. */
void filter_graph_draw(u8g2_t *u8g2, const filter_graph_t *fg);

#ifdef __cplusplus
}
#endif
