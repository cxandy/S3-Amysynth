/* smf-to-AMYSONG converter - exact mirror of tools/midi2amysong.py so the
 * phone can upload a .mid straight to the device and skip the PC entirely.
 * See midi_import.h for the contract. */

#include "midi_import.h"

#include <stdio.h>
#include <string.h>

#define MAX_STEPS        32
#define MAX_MEL_LAYERS   3
#define MAX_CHANNELS     16

typedef struct {
    const uint8_t *tr;
    size_t         len;
    size_t         i;
    uint32_t       tick;
    uint8_t        running;
    uint32_t       tempo;         /* last set-tempo found, us/quarter */
    uint32_t       div;
    int            steps;
    int            span;          /* loop length in ticks */
    int            total_notes;
    int            any_drum;
    uint8_t        has_melodic[MAX_CHANNELS];
    uint8_t        grid[MAX_CHANNELS * MAX_STEPS];   /* note + 1, 0 = empty */
    uint8_t        drum[4 * MAX_STEPS];
} conv_t;

static void set_err(char *err, size_t err_cap, const char *fmt, int a)
{
    if (!err_cap) return;
    int n = snprintf(err, err_cap, fmt, a);
    if (n < 0) err[0] = '\0';
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int vlq(const uint8_t *p, size_t len, size_t *i, uint32_t *out)
{
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        if (*i >= len) return -1;
        uint8_t b = p[(* i)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

static void note_on(conv_t *s, uint32_t tick, uint8_t ch, uint8_t note)
{
    s->total_notes++;
    if ((int)tick >= s->span) return;
    int step = (int)(((uint64_t)tick * 4u + (uint32_t)s->div / 2u) / (uint32_t)s->div);
    if (step >= s->steps) return;

    if ((ch % 16) == 10) {                     /* drum channel (GM 10) */
        int tr = 3;
        if (note == 35 || note == 36)      tr = 0;
        else if (note == 37 || note == 38 || note == 40) tr = 1;
        else if (note == 42 || note == 44 || note == 46) tr = 2;
        s->drum[tr * MAX_STEPS + step] = 1;
        s->any_drum = 1;
    } else {
        s->has_melodic[ch] = 1;
        uint8_t *cell = &s->grid[ch * MAX_STEPS + step];
        if ((uint8_t)(note + 1) > *cell) *cell = (uint8_t)(note + 1);
    }
}

static int decode_track(conv_t *s, const uint8_t *p, size_t len,
                        char *err, size_t err_cap)
{
    s->tr = p;
    s->len = len;
    s->i = 0;
    s->tick = 0;
    s->running = 0;

    while (s->i < s->len) {
        uint32_t dt;
        if (vlq(s->tr, s->len, &s->i, &dt)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
        s->tick += dt;
        if (s->i >= s->len) break;

        uint8_t b = s->tr[s->i];
        if (b == 0xFF) {                       /* meta */
            s->i++;
            if (s->i >= s->len) break;
            uint8_t mt = s->tr[s->i];
            s->i++;
            uint32_t ln;
            if (vlq(s->tr, s->len, &s->i, &ln)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
            if (mt == 0x51 && ln == 3 && s->i + 3 <= s->len) {
                s->tempo = ((uint32_t)s->tr[s->i] << 16) |
                           ((uint32_t)s->tr[s->i + 1] << 8) | s->tr[s->i + 2];
            }
            s->i += ln;
            continue;
        }
        if (b == 0xF0 || b == 0xF7) {          /* sysex */
            uint32_t ln;
            if (vlq(s->tr, s->len, &s->i, &ln)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
            s->i += ln;
            continue;
        }
        if (b == 0xF8 || b >= 0xF1) continue;  /* realtime interleave */

        uint8_t st;
        if (b & 0x80) {
            st = b;
            s->running = b;
            s->i++;
        } else {
            if (s->running == 0) { set_err(err, err_cap, "running status without prior status", 0); return -1; }
            st = s->running;
        }
        uint8_t kind = st >> 4;
        uint8_t ch   = st & 0x0F;
        int used = (kind == 0xC || kind == 0xD) ? 1 : 2;
        if (s->i + (size_t)used > s->len) break;
        if (kind == 0x9 && s->tr[s->i + 1] > 0) {
            note_on(s, s->tick, ch, s->tr[s->i]);
        }
        s->i += (size_t)used;
    }
    return 0;
}

int midi_amysong_convert(const uint8_t *data, size_t len,
                         int bars, int patch, const char *name,
                         char *out, size_t out_cap,
                         char *err, size_t err_cap)
{
    if (err_cap) err[0] = '\0';
    if (bars != 1 && bars != 2) bars = 2;
    if (len < 14 || memcmp(data, "MThd", 4) != 0) {
        set_err(err, err_cap, "not a MIDI file", 0);
        return -1;
    }

    uint32_t hlen = rd32(data + 4);
    uint16_t fmt  = rd16(data + 8);
    uint16_t div  = rd16(data + 12);
    if (fmt > 1)     { set_err(err, err_cap, "format %d not supported", fmt); return -1; }
    if (div & 0x8000){ set_err(err, err_cap, "SMPTE time division not supported", 0); return -1; }
    if (div == 0)    { set_err(err, err_cap, "bad time division", 0); return -1; }

    conv_t c;
    memset(&c, 0, sizeof c);
    c.div = div;
    c.tempo = 500000;                          /* default 120 bpm */
    c.steps = bars * 16;
    c.span  = 4 * (int)div * bars;

    size_t off = 8 + hlen;
    while (off + 8 <= len) {
        if (memcmp(data + off, "MTrk", 4) != 0) { off++; continue; }
        uint32_t tlen = rd32(data + off + 4);
        if (off + 8 + tlen > len) { off++; continue; }
        if (decode_track(&c, data + off + 8, tlen, err, err_cap)) return -1;
        off += 8 + tlen;
    }

    int bpm = (int)((60000000u + c.tempo / 2u) / c.tempo);
    if (bpm < 1) bpm = 1;

    int mlayers[MAX_MEL_LAYERS];
    int nm = 0;
    for (int ch = 0; ch < MAX_CHANNELS && nm < MAX_MEL_LAYERS; ch++) {
        if (c.has_melodic[ch]) mlayers[nm++] = ch;
    }
    if (nm == 0 && !c.any_drum) {
        set_err(err, err_cap, "no notes within the first %d bar(s)", bars);
        return -1;
    }

    size_t olen = 0;
#define OUT(...) do { \
        int _w = snprintf(out + olen, out_cap - olen, __VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= out_cap - olen) { \
            set_err(err, err_cap, "output too large", 0); \
            return -1; \
        } \
        olen += (size_t)_w; \
    } while (0)

    OUT("amysong 1\n");
    if (name && name[0]) OUT("name \"%.15s\"\n", name);
    OUT("bpm %d\n", bpm);
    OUT("pattern %d\n", c.steps);

    const char base = 60;
    if (c.any_drum) {
        OUT("layer drum\n");
        for (int dr = 0; dr < 4; dr++) {
            char toks[MAX_STEPS * 3];
            char *w = toks;
            for (int s = 0; s < c.steps; s++) {
                *w++ = c.drum[dr * MAX_STEPS + s] ? 'x' : '.';
                *w++ = (s + 1 < c.steps) ? ' ' : '\0';
            }
            OUT("hit %d %s\n", dr, toks);
        }
    }

    for (int k = 0; k < nm; k++) {
        int ch = mlayers[k];
        OUT("layer melodic %d\n", patch);
        OUT("base %d\n", (int)base);
        char toks[MAX_STEPS * 6];
        char *w = toks;
        for (int s = 0; s < c.steps; s++) {
            uint8_t cell = c.grid[ch * MAX_STEPS + s];
            if (cell == 0) {
                *w++ = '.';
            } else {
                int ofs = (int)(cell - 1) - (int)base;
                if (ofs > 0) *w++ = '+';
                w += snprintf(w, 16, "%d", ofs);
            }
            *w++ = (s + 1 < c.steps) ? ' ' : '\0';
        }
        OUT("notes %s\n", toks);
    }
    return 0;
#undef OUT
}