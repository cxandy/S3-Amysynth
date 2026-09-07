#include "song_import.h"
#include "sequencer_core.h"
#include "project_store.h"
#include "project_snapshot.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "song_import";

#define IMP_MAX_LAYERS    3        /* melodic layers above the fixed drum  */
#define IMP_TRACKS        4        /* SEQ_TRACKS mirror                    */
#define IMP_BPM_DEF       120
#define IMP_BPM_MIN       40
#define IMP_BPM_MAX       240

/* Staged build: parse the whole text first so a bad line never halves the
 * session. Tone entry per (melodic track, step): NULL rest / SENTINEL uses
 * the per-entry `ofs` sign, so we carry an explicit on-flag. */
#define IMP_REST  0x7F              /* sentinel for "no note this step" */

typedef struct {
    uint16_t patch;
    uint8_t  base[IMP_TRACKS];      /* MIDI base note per track        */
    bool     base_set[IMP_TRACKS];
    uint8_t  ofs[IMP_TRACKS][SEQ_MAX_STEPS]; /* IMP_REST = rest        */
    uint8_t  tracks;                /* number of notes rows consumed   */
} imp_mel_layer_t;

typedef struct {
    bool hit[IMP_TRACKS][SEQ_MAX_STEPS];
    uint8_t tracks;                 /* number of hit rows consumed     */
} imp_drum_t;

typedef struct {
    char           name[PROJECT_NAME_LEN];
    uint16_t       bpm;
    uint8_t        pattern;         /* 16 or SEQ_MAX_STEPS             */
    imp_mel_layer_t mel[IMP_MAX_LAYERS];
    uint8_t        mel_count;
    imp_drum_t     drum;
    bool           has_drum;
} imp_song_t;

/* ── tiny line/token helpers ──────────────────────────────────────────── */

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static char *next_token(char **pp)
{
    char *p = skip_ws(*pp);
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') return NULL;
    char *tok = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r'
           && *p != '#') p++;
    if (*p == '#' || *p == '\n' || *p == '\r') *p = '\0';
    else if (*p) { *p = '\0'; p++; }
    *pp = p;
    return tok;
}

/* Read the rest of the line after its first token already split off. */
static char *pp_rest(char **pp)
{
    char *p = *pp;
    /* Strip the leading token already consumed by check_keyword(). */
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* ── per-statement parsers ────────────────────────────────────────────── */

static bool parse_name_value(char *v, char *out, size_t outsz, int line)
{
    (void)line;
    /* Accept "Title" (optionally quoted). */
    v = skip_ws(v);
    size_t n = strlen(v);
    if (n == 0) return false;
    if (n > 0 && (v[0] == '"' || v[0] == '\'')) {
        v++;
        n--;
        if (n > 0 && (v[n - 1] == '"' || v[n - 1] == '\'')) n--;
    }
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, v, n);
    out[n] = '\0';
    return n > 0;
}

static int parse_signed(const char *tok, int *out)
{
    char *end = NULL;
    long v = strtol(tok, &end, 10);
    if (end == tok) return 0;
    *out = (int)v;
    return 1;
}

/* Parse one step token into (on, ofs). '.': rest; '0': base; +/-n: offset. */
static int parse_step_tok(const char *tok, bool *on, int *ofs)
{
    if (tok == NULL) return 0;
    if (tok[0] == '.') { *on = false; return 1; }
    if (tok[0] == '+') {
        int o; if (!parse_signed(tok + 1, &o)) return 0;
        *on = true; *ofs = o; return 1;
    }
    if (tok[0] == '-') {
        int o; if (!parse_signed(tok + 1, &o)) return 0;
        *on = true; *ofs = -o; return 1;
    }
    int o; if (!parse_signed(tok, &o)) return 0;   /* bare number = base note */
    *on = true;
    *ofs = o;
    return 1;
}

static bool parse_song(const char *text, imp_song_t *s, char *err, size_t errsz)
{
    /* Staging is module state: song_import_apply is ui-task-only (single
     * thread), and a ~0.6 KB struct on the stack would brush the component's
     * -Wstack-usage=1024 budget inside a frame that also holds parse state. */
    static imp_song_t st;
    memset(&st, 0, sizeof st);
    st.bpm     = IMP_BPM_DEF;
    st.pattern = SEQ_STEPS;
    st.mel_count = 0;
    st.has_drum  = false;
    char name_fb[PROJECT_NAME_LEN];
    memset(name_fb, 0, sizeof name_fb);

    char *cur = (char *)text;
    int   line = 0;

    while (cur && *cur) {
        char *nl = strchr(cur, '\n');
        char *ln = cur;
        int   llen = (nl ? (int)(nl - cur) : (int)strlen(cur));
        /* Work on a mutable copy of the line. */
        char buf[256];
        if (llen >= (int)sizeof buf) llen = (int)sizeof buf - 1;
        memcpy(buf, ln, (size_t)llen);
        buf[llen] = '\0';
        line++;

        char *pp = buf;
        char *kw = next_token(&pp);
        if (!kw) { cur = nl ? nl + 1 : NULL; continue; }   /* blank/comment */

        imp_mel_layer_t *ml = st.mel_count > 0
                              ? &st.mel[st.mel_count - 1] : NULL;

        if (strcmp(kw, "amysong") == 0) {
            char *ver = next_token(&pp);
            if (!ver || strcmp(ver, "1") != 0) {
                snprintf(err, errsz, "line %d: unsupported amysong version", line);
                return false;
            }
        } else if (strcmp(kw, "name") == 0) {
            char *v = pp_rest(&pp);
            if (!parse_name_value(v, name_fb, sizeof name_fb, line)) {
                snprintf(err, errsz, "line %d: bad name", line);
                return false;
            }
        } else if (strcmp(kw, "bpm") == 0) {
            char *v = next_token(&pp);
            int b; if (!v || !parse_signed(v, &b) || b < IMP_BPM_MIN || b > IMP_BPM_MAX) {
                snprintf(err, errsz, "line %d: bpm must be %d..%d", line,
                         IMP_BPM_MIN, IMP_BPM_MAX);
                return false;
            }
            st.bpm = (uint16_t)b;
        } else if (strcmp(kw, "pattern") == 0) {
            char *v = next_token(&pp);
            int n; if (!v || !parse_signed(v, &n) ||
                       (n != (int)SEQ_STEPS && n != (int)SEQ_MAX_STEPS)) {
                snprintf(err, errsz, "line %d: pattern must be %d or %d", line,
                         (int)SEQ_STEPS, (int)SEQ_MAX_STEPS);
                return false;
            }
            st.pattern = (uint8_t)n;
        } else if (strcmp(kw, "layer") == 0) {
            char *typ = next_token(&pp);
            if (!typ) {
                snprintf(err, errsz, "line %d: layer needs a type", line);
                return false;
            }
            if (strcmp(typ, "melodic") == 0) {
                if (st.mel_count >= IMP_MAX_LAYERS) {
                    snprintf(err, errsz, "line %d: too many melodic layers (max %d)",
                             line, IMP_MAX_LAYERS);
                    return false;
                }
                char *pt = next_token(&pp);
                int p; if (!pt || !parse_signed(pt, &p) || p < 0 || p > 511) {
                    snprintf(err, errsz, "line %d: melodic layer needs a patch 0..511",
                             line);
                    return false;
                }
                imp_mel_layer_t *m = &st.mel[st.mel_count++];
                memset(m, 0, sizeof *m);
                m->patch = (uint16_t)p;
                for (int t = 0; t < IMP_TRACKS; t++) {
                    memset(m->ofs[t], IMP_REST, sizeof m->ofs[t]);
                    m->base[t] = 60;
                }
            } else if (strcmp(typ, "drum") == 0) {
                if (st.has_drum) {
                    snprintf(err, errsz, "line %d: duplicate drum layer", line);
                    return false;
                }
                st.has_drum = true;
                memset(&st.drum, 0, sizeof st.drum);
            } else {
                snprintf(err, errsz, "line %d: unknown layer type '%s'", line, typ);
                return false;
            }
        } else if (strcmp(kw, "base") == 0) {
            if (!ml) {
                snprintf(err, errsz, "line %d: base outside a melodic layer", line);
                return false;
            }
            if (ml->tracks >= IMP_TRACKS) {
                snprintf(err, errsz, "line %d: base after 4 tracks", line);
                return false;
            }
            char *v = next_token(&pp);
            int n; if (!v || !parse_signed(v, &n) || n < 0 || n > 127) {
                snprintf(err, errsz, "line %d: base note must be 0..127", line);
                return false;
            }
            ml->base[ml->tracks] = (uint8_t)n;
            ml->base_set[ml->tracks] = true;
        } else if (strcmp(kw, "notes") == 0) {
            if (!ml) {
                snprintf(err, errsz, "line %d: notes outside a melodic layer", line);
                return false;
            }
            if (ml->tracks >= IMP_TRACKS) {
                snprintf(err, errsz, "line %d: too many tracks in layer (max %d)",
                         line, IMP_TRACKS);
                return false;
            }
            uint8_t track = ml->tracks;
            uint8_t step = 0;
            char *tok;
            while ((tok = next_token(&pp)) != NULL && step < st.pattern) {
                bool on; int ofs;
                if (!parse_step_tok(tok, &on, &ofs)) {
                    snprintf(err, errsz, "line %d: bad note token '%s'", line, tok);
                    return false;
                }
                if (on) {
                    ml->ofs[track][step] = ofs >= 24 ? 24
                                          : (ofs <= -24 ? (uint8_t)(256 - 24)
                                                        : (uint8_t)(int8_t)ofs);
                }
                step++;
            }
            ml->tracks++;
        } else if (strcmp(kw, "hit") == 0) {
            if (!st.has_drum) {
                snprintf(err, errsz, "line %d: hit outside the drum layer", line);
                return false;
            }
            if (st.drum.tracks >= IMP_TRACKS) {
                snprintf(err, errsz, "line %d: too many drum tracks (max %d)",
                         line, IMP_TRACKS);
                return false;
            }
            char *tv = next_token(&pp);
            int trk; if (!tv || !parse_signed(tv, &trk) || trk < 0 || trk >= IMP_TRACKS) {
                snprintf(err, errsz, "line %d: drum track must be 0..3", line);
                return false;
            }
            uint8_t track = (uint8_t)trk;
            uint8_t step = 0;
            char *tok;
            while ((tok = next_token(&pp)) != NULL && step < st.pattern) {
                if (tok[0] == 'x' || tok[0] == 'X' || tok[0] == '1') {
                    st.drum.hit[track][step] = true;
                } else if (tok[0] == '.') {
                    /* off */
                } else {
                    snprintf(err, errsz, "line %d: bad hit token '%s'", line, tok);
                    return false;
                }
                step++;
            }
            st.drum.tracks++;
        } else {
            snprintf(err, errsz, "line %d: unknown keyword '%s'", line, kw);
            return false;
        }

        cur = nl ? nl + 1 : NULL;
    }

    if (st.mel_count == 0 && !st.has_drum) {
        snprintf(err, errsz, "no layers: nothing to import");
        return false;
    }
    memcpy(st.name, name_fb, sizeof st.name);
    memcpy(s, &st, sizeof st);
    if (err && errsz) err[0] = '\0';
    return true;
}

/* ── apply ── */

static void clear_melodic_layers(void)
{
    while (sequencer_core_get_num_layers() > 1) {
        sequencer_core_delete_layer((uint8_t)(sequencer_core_get_num_layers() - 1));
    }
}

static void apply_song(const imp_song_t *s, uint8_t slot, const char *name_fb)
{
    /* Replace all melodic layers; layer 0 (drum) keeps its kit. */
    clear_melodic_layers();

    for (uint8_t l = 0; l < s->mel_count; l++) {
        const imp_mel_layer_t *m = &s->mel[l];
        uint8_t li = sequencer_core_add_layer(SEQ_LAYER_MELODIC, s->pattern);
        if (li == 0xFF) break;   /* keep whatever fit */
        sequencer_core_set_layer_patch(li, m->patch);
        for (uint8_t t = 0; t < m->tracks && t < IMP_TRACKS; t++) {
            sequencer_core_set_track_midi_note(li, t, m->base[t]);
            for (uint8_t st = 0; st < s->pattern; st++) {
                if (m->ofs[t][st] == IMP_REST) continue;
                sequencer_core_set_step(li, t, st, true);
                if (m->ofs[t][st] != 0) {
                    sequencer_core_set_step_pitch_ofs(li, t, st,
                        (int8_t)m->ofs[t][st]);
                }
            }
        }
    }

    if (s->has_drum) {
        for (uint8_t t = 0; t < IMP_TRACKS; t++) {
            for (uint8_t st = 0; st < s->pattern; st++) {
                sequencer_core_set_step(0, t, st, s->drum.hit[t][st]);
            }
        }
    }

    sequencer_core_set_bpm(s->bpm);
    sequencer_core_set_playing(true);

    /* Persist as a project so a later Load replays it exactly. */
    char proj_name[PROJECT_NAME_LEN];
    snprintf(proj_name, sizeof proj_name, "%s",
             s->name[0] != '\0' ? s->name : (name_fb ? name_fb : "SONG"));
    if (project_snapshot_save(slot, proj_name)) {
        ESP_LOGI(TAG, "saved slot %u as '%s' (%u layer(s), %u bpm)",
                 (unsigned)slot, proj_name, (unsigned)s->mel_count + (s->has_drum ? 1u : 0u),
                 (unsigned)s->bpm);
    } else {
        ESP_LOGE(TAG, "save to slot %u failed", (unsigned)slot);
    }
}

bool song_import_apply(uint8_t slot, const char *text, const char *name_fb,
                       char *err, size_t errsz)
{
    imp_song_t s;
    memset(&s, 0, sizeof s);
    if (!parse_song(text, &s, err, errsz)) return false;

    apply_song(&s, slot, name_fb);
    return true;
}