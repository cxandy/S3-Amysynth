#include "sdkconfig.h"
#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "seq_chords.h"
#include "seq_core_config.h"   /* SEQ_MEL_NOTE_MIN/MAX - same-component limits */
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  CHORDS PAGE (Main Menu -> "Chords")
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the chord-preset editor; page state and input routing live
 * in ui_screen_menu.c (same split as the FX/NoteFX/Projects pages).
 *
 * Two levels inside one page: the slot list (CH1..CH8 + Back) and, after
 * clicking a slot, the edit view (4 note positions + Clear + Back). Every edit
 * commits immediately through seq_chords_set(), so the engine sweep (re-emit,
 * voice reconfig, delete fallback) rides each commit and closing the menu
 * mid-edit can never lose a voicing. The edit view keeps a raw working copy so
 * display order stays stable; storage normalizes (sorted, zero-packed).
 *
 * Changes audition through the selected melodic track's actual patch; falls
 * back to the first melodic track on a drum layer, silent with none. */

typedef enum { CHORDS_MODE_LIST = 0, CHORDS_MODE_EDIT } chords_mode_t;

static chords_mode_t s_mode = CHORDS_MODE_LIST;
static uint8_t       s_slot = 0;
static seq_chord_t   s_edit;                 /* working copy (edit view) */

#define CHORDS_LIST_COUNT (SEQ_CHORD_SLOTS + 1)          /* slots + Back  */
#define CHORDS_EDIT_COUNT (SEQ_CHORD_MAX_NOTES + 2)      /* notes + Clear + Back */
#define CHORDS_EDIT_CLEAR (SEQ_CHORD_MAX_NOTES)
#define CHORDS_EDIT_BACK  (SEQ_CHORD_MAX_NOTES + 1)

static menu_item_view_t s_items[CHORDS_LIST_COUNT];

/* Audition target: the selected track when its layer is melodic, else the
 * first melodic layer's first track. Returns false with no melodic layer. */
static bool chords_audition_target(uint8_t *li, uint8_t *track)
{
    uint8_t a = seq_state.active_layer_idx;
    if (a < seq_state.num_layers &&
        seq_state.layers[a].type == SEQ_LAYER_MELODIC) {
        *li = a;
        *track = seq_state.selected_track;
        return true;
    }
    for (uint8_t i = 0; i < seq_state.num_layers; i++) {
        if (seq_state.layers[i].type == SEQ_LAYER_MELODIC) {
            *li = i;
            *track = 0;
            return true;
        }
    }
    return false;
}

static void chords_audition(const seq_chord_t *c)
{
    uint8_t li, track;
    if (!chords_audition_target(&li, &track)) return;
    sequencer_core_audition_chord(li, track, c);
}

const char *chords_menu_title(void)
{
    static char s_title[16];
    if (s_mode == CHORDS_MODE_EDIT) {
        snprintf(s_title, sizeof(s_title), "CHORD CH%u", (unsigned)(s_slot + 1u));
        return s_title;
    }
    return "CHORDS";
}

const menu_item_view_t *chords_menu_build_items(void)
{
    char nb[4];
    if (s_mode == CHORDS_MODE_LIST) {
        for (uint8_t i = 0; i < SEQ_CHORD_SLOTS; i++) {
            snprintf(s_items[i].label, MENU_LABEL_LEN, "CH%u", (unsigned)(i + 1u));
            seq_chord_t c;
            if (seq_chords_get(i, &c) && c.count > 0) {
                /* Compact voicing summary: lowest tone + tone count. */
                ui_note_name(c.notes[0], nb);
                snprintf(s_items[i].value, MENU_VALUE_LEN, "%s x%u",
                         nb, (unsigned)c.count);
            } else {
                snprintf(s_items[i].value, MENU_VALUE_LEN, "--");
            }
        }
        snprintf(s_items[SEQ_CHORD_SLOTS].label, MENU_LABEL_LEN, "< Back");
        s_items[SEQ_CHORD_SLOTS].value[0] = '\0';
        return s_items;
    }

    for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
        snprintf(s_items[i].label, MENU_LABEL_LEN, "Note %u", (unsigned)(i + 1u));
        if (s_edit.notes[i] == 0) {
            snprintf(s_items[i].value, MENU_VALUE_LEN, "--");
        } else {
            ui_note_name(s_edit.notes[i], nb);
            snprintf(s_items[i].value, MENU_VALUE_LEN, "%s", nb);
        }
    }
    snprintf(s_items[CHORDS_EDIT_CLEAR].label, MENU_LABEL_LEN, "Clear");
    s_items[CHORDS_EDIT_CLEAR].value[0] = '\0';
    snprintf(s_items[CHORDS_EDIT_BACK].label, MENU_LABEL_LEN, "< Back");
    s_items[CHORDS_EDIT_BACK].value[0] = '\0';
    return s_items;
}

uint8_t chords_menu_item_count(void)
{
    return (s_mode == CHORDS_MODE_LIST) ? CHORDS_LIST_COUNT : CHORDS_EDIT_COUNT;
}

bool chords_menu_item_is_back(uint8_t idx)
{
    /* Only the LIST-level Back leaves the page; the edit view's Back returns
     * to the list inside handle_click. */
    return s_mode == CHORDS_MODE_LIST && idx == SEQ_CHORD_SLOTS;
}

bool chords_menu_item_is_value(uint8_t idx)
{
    return s_mode == CHORDS_MODE_EDIT && idx < SEQ_CHORD_MAX_NOTES;
}

/* Returns the new menu_editing state (mirrors projects_menu_handle_click). */
bool chords_menu_handle_click(uint8_t idx)
{
    if (s_mode == CHORDS_MODE_LIST) {
        if (idx < SEQ_CHORD_SLOTS) {
            s_slot = idx;
            if (!seq_chords_get(s_slot, &s_edit)) {
                memset(&s_edit, 0, sizeof(s_edit));
            }
            s_mode = CHORDS_MODE_EDIT;
            seq_state.menu_cursor = 0;
            if (s_edit.count > 0) chords_audition(&s_edit);  /* hear it on open */
        }
        return false;
    }

    if (idx < SEQ_CHORD_MAX_NOTES) {
        return !seq_state.menu_editing;   /* toggle note-position editing */
    }
    if (idx == CHORDS_EDIT_CLEAR) {
        memset(&s_edit, 0, sizeof(s_edit));
        seq_chords_clear(s_slot);
        return false;
    }
    /* Back: voicing is already committed per edit; just pop to the list. */
    s_mode = CHORDS_MODE_LIST;
    seq_state.menu_cursor = s_slot;
    return false;
}

void chords_menu_edit_value(uint8_t idx, int delta)
{
    if (s_mode != CHORDS_MODE_EDIT || idx >= SEQ_CHORD_MAX_NOTES || delta == 0) {
        return;
    }

    /* Position domain: -- (empty, stored 0) below C1, then C1..C7. */
    int dir = (delta > 0) ? 1 : -1;
    int n = (int)s_edit.notes[idx];
    for (int k = (delta > 0 ? delta : -delta); k > 0; k--) {
        if (n == 0) {
            if (dir > 0) n = SEQ_MEL_NOTE_MIN;
        } else {
            n += dir;
            if (n < SEQ_MEL_NOTE_MIN) n = 0;
            if (n > SEQ_MEL_NOTE_MAX) n = SEQ_MEL_NOTE_MAX;
        }
    }
    s_edit.notes[idx] = (uint8_t)n;

    /* Commit + audition; s_edit keeps raw order so the cursor doesn't jump. */
    seq_chords_set(s_slot, &s_edit);
    chords_audition(&s_edit);
}

void chords_menu_reset(void)
{
    s_mode = CHORDS_MODE_LIST;
}
