/* smf-to-AMYSONG converter - exact mirror of tools/midi2amysong.py so the
 * phone can upload a .mid straight to the device and skip the PC entirely.
 * See midi_import.h for the contract. Moved here from wifi_importer so the
 * WebSerial importer can reuse it without pulling in the WiFi stack. */

#include "midi_import.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

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
        if (b == 0xF0 || b == 0xF7) {          /* sysex: skip status, THEN len */
            s->i++;
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

/* ── whole-song arrangement ─────────────────────────────────────────────
 * Marker-driven sections (0xFF 0x06 text / 0xFF 0x07 marker meta) split the
 * file into scenes. Each section's first one/two bars become a loop cell:
 * at most 3 melodic cells map onto the melodic layers in first-appearance
 * order (the 4th+ sections reuse the cell of an earlier section via scene
 * mask), the section with the densest drums owns the single drum cell, and
 * the scene table replays the section order. Two decode passes avoid a notes
 * buffer: pass one finds markers/tempo/name/program changes, pass two fills
 * the per-section grids with the sections already known. The workspace is
 * module-static and single-threaded (ui task), mirroring song_import. */

#define ARR_MAX_MARKERS   64
#define ARR_MAX_SCENES    16
#define ARR_MAX_STEPS     32
#define ARR_MAX_CELLS     3
#define ARR_NAME_MAX      16
#define ARR_DRUM_CH       10

typedef struct {
    uint8_t grid[ARR_MAX_STEPS];   /* note + 1, 0 = empty          */
    uint16_t patch;
    bool     used;
} arr_cell_t;

typedef struct {
    uint8_t  mel_grid[ARR_MAX_STEPS];
    uint8_t  drum_grid[4][ARR_MAX_STEPS];
    uint8_t  cell;                 /* 0..ARR_MAX_CELLS-1, 0xFF none */
    uint8_t  first_mel_ch;
    uint8_t  bars;
    bool     mel;
    bool     drums;
    uint16_t drum_cnt;
} arr_sec_t;

static arr_sec_t  s_secs[ARR_MAX_MARKERS];
static arr_cell_t s_cells[ARR_MAX_CELLS];

static int arr_find_sec(const uint32_t *starts, int nsecs, uint32_t tick)
{
    int s = 0;
    for (int k = 0; k < nsecs; k++) {
        if (tick >= starts[k]) s = k;
        else break;
    }
    return s;
}

static void arr_fill_note(arr_sec_t *secs, const uint32_t *starts, int nsecs,
                          uint32_t div, uint8_t steps, uint32_t win,
                          uint32_t tick, uint8_t ch, uint8_t note)
{
    int s = arr_find_sec(starts, nsecs, tick);
    if (s < 0 || s >= nsecs) return;
    uint32_t w0 = starts[s];
    if (tick < w0 || tick >= w0 + win) return;   /* keep the leading loop only */
    int step = (int)(((uint64_t)tick * 4u + (uint64_t)div / 2u) / (uint64_t)div);
    if (step < 0 || step >= (int)steps) return;

    arr_sec_t *sec = &secs[s];
    if ((ch % 16) == ARR_DRUM_CH) {
        int tr = 3;
        if (note == 35 || note == 36) tr = 0;
        else if (note == 37 || note == 38 || note == 40) tr = 1;
        else if (note == 42 || note == 44 || note == 46) tr = 2;
        if (!sec->drums) {
            sec->drums = true;
            sec->drum_cnt = 0;
        }
        sec->drum_grid[tr][step] = 1;
        sec->drum_cnt++;
    } else {
        if (!sec->mel) {
            sec->mel = true;
            sec->first_mel_ch = ch;
        }
        if ((uint8_t)(note + 1) > sec->mel_grid[step]) sec->mel_grid[step] = (uint8_t)(note + 1);
    }
}

int midi_amysong_arrange_convert(const uint8_t *data, size_t len, int patch,
                                 const char *name,
                                 char *out, size_t out_cap,
                                 char *err, size_t err_cap)
{
    if (err_cap) err[0] = '\0';
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

    uint32_t tpb = 4u * (uint32_t)div;            /* one 4/4 bar in ticks */
    uint32_t mrk[ARR_MAX_MARKERS];
    int      nmrk = 0;

    /* pass one: markers, tempo, program changes, song name, end tick */
    uint32_t tempo = 500000;
    uint32_t song_end = 0;
    uint8_t  prg[MAX_CHANNELS], prg_ok[MAX_CHANNELS];
    memset(prg, 0, sizeof prg);
    memset(prg_ok, 0, sizeof prg_ok);
    char song_name[ARR_NAME_MAX] = { 0 };

    size_t off = 8 + hlen;
    while (off + 8 <= len) {
        if (memcmp(data + off, "MTrk", 4) != 0) { off++; continue; }
        uint32_t tlen = rd32(data + off + 4);
        if (off + 8 + tlen > len) { off++; continue; }
        const uint8_t *p = data + off + 8;
        size_t i = 0, l = tlen;
        uint32_t tick = 0;
        uint8_t running = 0;
        while (i < l) {
            uint32_t dt;
            if (vlq(p, l, &i, &dt)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
            tick += dt;
            if (tick > song_end) song_end = tick;
            if (i >= l) break;
            uint8_t b = p[i];
            if (b == 0xFF) {
                i++;
                if (i >= l) break;
                uint8_t mt = p[i];
                i++;
                uint32_t ln;
                if (vlq(p, l, &i, &ln)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
                if (i + ln > l) break;
                if (mt == 0x51 && ln == 3) {
                    tempo = ((uint32_t)p[i] << 16) | ((uint32_t)p[i+1] << 8) | p[i+2];
                } else if ((mt == 0x06 || mt == 0x07) && ln > 0) {
                    if (nmrk < ARR_MAX_MARKERS) mrk[nmrk++] = tick;
                } else if (mt == 0x03 && ln > 0 && song_name[0] == '\0') {
                    uint32_t n = ln < (uint32_t)(ARR_NAME_MAX - 1)
                                 ? ln : (uint32_t)(ARR_NAME_MAX - 1);
                    memcpy(song_name, p + i, n);
                    song_name[n] = '\0';
                }
                i += ln;
                continue;
            }
            if (b == 0xF0 || b == 0xF7) {
                i++;
                uint32_t ln;
                if (vlq(p, l, &i, &ln)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
                i += ln;
                continue;
            }
            if (b == 0xF8 || b >= 0xF1) continue;

            uint8_t st;
            if (b & 0x80) {
                st = b;
                running = b;
                i++;
            } else {
                if (running == 0) { set_err(err, err_cap, "running status without prior status", 0); return -1; }
                st = running;
            }
            uint8_t kind = st >> 4;
            uint8_t ch   = st & 0x0F;
            int used = (kind == 0xC || kind == 0xD) ? 1 : 2;
            if (i + (size_t)used > l) break;
            if (kind == 0xC) {
                prg[ch] = p[i];
                prg_ok[ch] = 1;
            }
            i += (size_t)used;
        }
        off += 8 + tlen;
    }

    /* marker -> sections (drop zero-length trailing markers) */
    for (int a = 1; a < nmrk; a++) {
        uint32_t key = mrk[a];
        int j = a - 1;
        while (j >= 0 && mrk[j] > key) { mrk[j + 1] = mrk[j]; j--; }
        mrk[j + 1] = key;
    }
    int nsecs = 0;
    for (int k = 0; k < nmrk; k++) {
        if (k > 0 && mrk[k] == mrk[k - 1]) continue;
        if (mrk[k] >= song_end) continue;
        mrk[nsecs++] = mrk[k];
    }
    if (nsecs < 2) {
        set_err(err, err_cap,
                "need at least 2 section markers (text/marker meta) to arrange", 0);
        return -1;
    }

    int max_bars = 1;
    memset(s_secs, 0, sizeof s_secs);
    for (int s = 0; s < nsecs; s++) {
        uint32_t st = mrk[s];
        uint32_t en = (s + 1 < nsecs) ? mrk[s + 1] : song_end;
        uint32_t raw = (en - st + tpb / 2u) / tpb;
        if (raw < 1) raw = 1;
        if (raw > 255) raw = 255;
        s_secs[s].bars = (uint8_t)raw;
        if ((int)raw > max_bars) max_bars = (int)raw;
    }
    uint8_t steps = (max_bars > 1) ? 32 : 16;
    uint32_t win = tpb * ((uint32_t)steps / 16u);

    /* pass two: fill per-section grids */
    off = 8 + hlen;
    while (off + 8 <= len) {
        if (memcmp(data + off, "MTrk", 4) != 0) { off++; continue; }
        uint32_t tlen = rd32(data + off + 4);
        if (off + 8 + tlen > len) { off++; continue; }
        const uint8_t *p = data + off + 8;
        size_t i = 0, l = tlen;
        uint32_t tick = 0;
        uint8_t running = 0;
        while (i < l) {
            uint32_t dt;
            if (vlq(p, l, &i, &dt)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
            tick += dt;
            if (i >= l) break;
            uint8_t b = p[i];
            if (b == 0xFF) {
                i++;
                if (i >= l) break;
                uint8_t mt = p[i];
                i++;
                uint32_t ln;
                if (vlq(p, l, &i, &ln)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
                if (i + ln > l) break;
                i += ln;
                continue;
            }
            if (b == 0xF0 || b == 0xF7) {
                i++;
                uint32_t ln;
                if (vlq(p, l, &i, &ln)) { set_err(err, err_cap, "bad VLQ", 0); return -1; }
                i += ln;
                continue;
            }
            if (b == 0xF8 || b >= 0xF1) continue;

            uint8_t st;
            if (b & 0x80) {
                st = b;
                running = b;
                i++;
            } else {
                if (running == 0) { set_err(err, err_cap, "running status without prior status", 0); return -1; }
                st = running;
            }
            uint8_t kind = st >> 4;
            uint8_t ch   = st & 0x0F;
            int used = (kind == 0xC || kind == 0xD) ? 1 : 2;
            if (i + (size_t)used > l) break;
            if (kind == 0x9 && p[i + 1] > 0) {
                arr_fill_note(s_secs, mrk, nsecs, div, steps, win,
                              tick, ch, p[i]);
            }
            i += (size_t)used;
        }
        off += 8 + tlen;
    }

    /* melodic cells: first-appearance order into the 3 melodic layers */
    int drum_winner = -1;
    uint16_t drum_best = 0;
    int mel_ordinal = 0;
    bool any_mel = false, any_drum = false;
    memset(s_cells, 0, sizeof s_cells);
    for (int s = 0; s < nsecs; s++) {
        arr_sec_t *sec = &s_secs[s];
        if (sec->drums) {
            any_drum = true;
            if (sec->drum_cnt > drum_best) { drum_best = sec->drum_cnt; drum_winner = s; }
        }
        if (sec->mel) {
            any_mel = true;
            int cell = mel_ordinal % ARR_MAX_CELLS;
            sec->cell = (uint8_t)cell;
            if (!s_cells[cell].used) {
                memcpy(s_cells[cell].grid, sec->mel_grid, sizeof sec->mel_grid);
                s_cells[cell].patch = prg_ok[sec->first_mel_ch]
                                      ? (uint16_t)prg[sec->first_mel_ch]
                                      : (uint16_t)patch;
                s_cells[cell].used = true;
            }
            mel_ordinal++;
        }
    }
    if (!any_mel && !any_drum) {
        set_err(err, err_cap, "no notes within section windows", 0);
        return -1;
    }

    int bpm = (int)((60000000u + tempo / 2u) / tempo);
    if (bpm < 1) bpm = 1;

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
    const char *nm = (song_name[0] != '\0') ? song_name : name;
    if (nm && nm[0]) OUT("name \"%.15s\"\n", nm);
    OUT("bpm %d\n", bpm);
    OUT("pattern %d\n", (int)steps);

    const char base = 60;
    if (any_drum) {
        const arr_sec_t *w = &s_secs[drum_winner];
        OUT("layer drum\n");
        for (int dr = 0; dr < 4; dr++) {
            char toks[ARR_MAX_STEPS * 3];
            char *wp = toks;
            for (int st = 0; st < steps; st++) {
                *wp++ = w->drum_grid[dr][st] ? 'x' : '.';
                *wp++ = (st + 1 < steps) ? ' ' : '\0';
            }
            OUT("hit %d %s\n", dr, toks);
        }
    }

    for (int k = 0; k < ARR_MAX_CELLS; k++) {
        if (!s_cells[k].used) continue;
        OUT("layer melodic %d\n", (int)s_cells[k].patch);
        OUT("base %d\n", (int)base);
        char toks[ARR_MAX_STEPS * 6];
        char *wp = toks;
        for (int st = 0; st < steps; st++) {
            uint8_t cell = s_cells[k].grid[st];
            if (cell == 0) {
                *wp++ = '.';
            } else {
                int ofs = (int)(cell - 1) - (int)base;
                if (ofs > 0) *wp++ = '+';
                wp += snprintf(wp, 16, "%d", ofs);
            }
            *wp++ = (st + 1 < steps) ? ' ' : '\0';
        }
        OUT("notes %s\n", toks);
    }

    /* scene table: merge adjacent identical masks, cap at 16 */
    {
        uint8_t sb[ARR_MAX_SCENES], sm[ARR_MAX_SCENES];
        int nscene = 0;
        for (int s = 0; s < nsecs; s++) {
            const arr_sec_t *sec = &s_secs[s];
            if (!sec->mel && !sec->drums) continue;   /* silent gap */
            uint8_t mask = 0;
            if (sec->drums) mask |= (uint8_t)(1u << 0);
            if (sec->mel)   mask |= (uint8_t)(1u << (sec->cell + 1));
            if (nscene > 0 && sm[nscene - 1] == mask) {
                uint32_t bars = (uint32_t)sb[nscene - 1] + (uint32_t)sec->bars;
                sb[nscene - 1] = (uint8_t)(bars > 255 ? 255 : bars);
            } else {
                if (nscene >= ARR_MAX_SCENES) {
                    set_err(err, err_cap,
                            "too many sections (max %d after merge)", ARR_MAX_SCENES);
                    return -1;
                }
                sb[nscene] = sec->bars;
                sm[nscene] = mask;
                nscene++;
            }
        }
        if (nscene == 0) {
            set_err(err, err_cap, "no notes within section windows", 0);
            return -1;
        }
        OUT("song enabled 1\n");
        OUT("song loop 1\n");
        for (int i = 0; i < nscene; i++) {
            OUT("scene %d %d\n", (int)sb[i], (int)sm[i]);
        }
        return 0;
    }
#undef OUT
}