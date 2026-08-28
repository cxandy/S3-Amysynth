#include "custompatches/fm_graph.h"
#include <string.h>

/* AMY algorithms.c FmOperatorFlags (mirrored: the enum is file-private
 * upstream; amy.h documents the values next to amy_set_custom_algorithm). */
#define OUT_BUS_ONE 0x01
#define OUT_BUS_TWO 0x02
#define OUT_BUS_ADD 0x04
#define IN_BUS_ONE  0x10
#define IN_BUS_TWO  0x20
#define FB_IN       0x40
#define FB_OUT      0x80

enum { BUS_NONE = 0, BUS_ONE = 1, BUS_TWO = 2, BUS_OUT = 3 };

void fm_graph_decode(const uint8_t ops[FM_GRAPH_OPS], fm_graph_view_t *out)
{
    uint8_t writers[3] = { 0, 0, 0 };   /* bitmask of ops currently on bus 1/2 */
    memset(out, 0, sizeof(*out));
    out->fb_op = FM_OP_NONE;
    for (uint8_t s = 0; s < FM_GRAPH_OPS; s++) {
        uint8_t b = ops[s];
        uint8_t in = (b & IN_BUS_ONE) ? writers[BUS_ONE]
                   : (b & IN_BUS_TWO) ? writers[BUS_TWO] : 0;
        for (uint8_t w = 0; w < FM_GRAPH_OPS; w++) {
            if (in & (1u << w)) out->out_mask[w] |= (uint8_t)(1u << s);
        }
        if (b & FB_IN) out->fb_op = s;
        uint8_t bus = (b & OUT_BUS_ONE) ? BUS_ONE : (b & OUT_BUS_TWO) ? BUS_TWO : BUS_NONE;
        if (bus == BUS_NONE) {
            out->out_mask[s] |= FM_OUT_BIT;
        } else if (b & OUT_BUS_ADD) {
            writers[bus] |= (uint8_t)(1u << s);
        } else {
            writers[bus] = (uint8_t)(1u << s);
        }
    }
}

void fm_graph_from_forest(const uint8_t op_to[FM_GRAPH_OPS], uint8_t fb_op,
                          fm_graph_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fb_op = (fb_op < FM_GRAPH_OPS) ? fb_op : FM_OP_NONE;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        out->out_mask[i] = (op_to[i] < FM_GRAPH_OPS) ? (uint8_t)(1u << op_to[i])
                                                     : FM_OUT_BIT;
    }
}

/* Follow op_to[] upward from `op`; true if `target` is met (or op == target). */
static bool forest_reaches(const uint8_t op_to[FM_GRAPH_OPS], uint8_t op, uint8_t target)
{
    uint8_t guard = 0;
    while (op < FM_GRAPH_OPS && guard++ < FM_GRAPH_OPS) {
        if (op == target) return true;
        op = op_to[op];
    }
    return false;
}

bool fm_graph_edge_allowed(const uint8_t op_to[FM_GRAPH_OPS], uint8_t op, uint8_t target)
{
    if (op >= FM_GRAPH_OPS) return false;
    if (target == FM_TO_OUT) return true;
    if (target >= FM_GRAPH_OPS || target == op) return false;
    /* A cycle would exist if op already sits on target's path to the output. */
    return !forest_reaches(op_to, target, op);
}

void fm_graph_to_forest(const fm_graph_view_t *g, uint8_t op_to[FM_GRAPH_OPS])
{
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        uint8_t m = g->out_mask[i];
        op_to[i] = FM_TO_OUT;
        for (uint8_t t = 0; t < FM_GRAPH_OPS; t++) {
            if (m & (1u << t)) { op_to[i] = t; break; }
        }
    }
    /* Break cycles / orphan chains: anything that never reaches the output
     * becomes a carrier. */
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        uint8_t op = i, guard = 0;
        while (op_to[op] != FM_TO_OUT && guard++ < FM_GRAPH_OPS) op = op_to[op];
        if (op_to[op] != FM_TO_OUT) op_to[i] = FM_TO_OUT;
    }
}

/* ── compiler ─────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *op_to;
    uint8_t fb_op;
    fm_program_t *prog;
    uint8_t n;               /* slots emitted so far */
    uint8_t emitted;         /* bitmask of ops already placed (cycle guard) */
} compile_ctx_t;

static uint8_t subtree_size(const compile_ctx_t *c, uint8_t node, uint8_t depth)
{
    uint8_t size = 1;
    if (depth >= FM_GRAPH_OPS) return size;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (c->op_to[i] == node) size = (uint8_t)(size + subtree_size(c, i, (uint8_t)(depth + 1)));
    }
    return size;
}

/* Emit `node` so that its output lands on `out` (BUS_ONE/BUS_TWO/BUS_OUT),
 * accumulating when `add`. `live` = buses holding data that must survive. */
static bool emit(compile_ctx_t *c, uint8_t node, uint8_t out, bool add, uint8_t live)
{
    if (c->emitted & (1u << node)) return false;     /* cycle */
    c->emitted |= (uint8_t)(1u << node);

    /* Children, largest subtree first: only the first child may still use a
     * bus that later children would find live. */
    uint8_t kids[FM_GRAPH_OPS], nk = 0;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (c->op_to[i] == node) kids[nk++] = i;
    }
    for (uint8_t a = 1; a < nk; a++) {           /* insertion sort, desc size */
        uint8_t k = kids[a], sk = subtree_size(c, k, 0);
        int b = a - 1;
        while (b >= 0 && subtree_size(c, kids[b], 0) < sk) { kids[b + 1] = kids[b]; b--; }
        kids[b + 1] = k;
    }

    uint8_t in = BUS_NONE;
    if (nk > 0) {
        uint8_t busy = live;
        if (add && out != BUS_OUT) busy |= (uint8_t)(1u << out);
        for (uint8_t cand = BUS_ONE; cand <= BUS_TWO; cand++) {
            if (busy & (1u << cand)) continue;
            /* Reading and overwriting the same bus is the 0x11 scratch case
             * AMY only implements for bus one, and never with ADD. */
            if (cand == out && (add || out != BUS_ONE)) continue;
            in = cand;
            break;
        }
        if (in == BUS_NONE) return false;
        for (uint8_t k = 0; k < nk; k++) {
            uint8_t child_live = busy;
            if (k > 0) child_live |= (uint8_t)(1u << in);
            if (!emit(c, kids[k], in, k > 0, child_live)) return false;
        }
    }

    if (c->n >= FM_GRAPH_OPS) return false;
    uint8_t byte = 0;
    if (in == BUS_ONE) byte |= IN_BUS_ONE;
    if (in == BUS_TWO) byte |= IN_BUS_TWO;
    if (out == BUS_OUT) {
        byte |= OUT_BUS_ADD;                 /* carriers always sum into buf */
    } else {
        byte |= (out == BUS_ONE) ? OUT_BUS_ONE : OUT_BUS_TWO;
        if (add) byte |= OUT_BUS_ADD;
    }
    if (node == c->fb_op) byte |= FB_IN | FB_OUT;
    c->prog->ops[c->n]     = byte;
    c->prog->slot_op[c->n] = node;
    c->n++;
    return true;
}

bool fm_graph_compile(const uint8_t op_to[FM_GRAPH_OPS], uint8_t fb_op, fm_program_t *out)
{
    compile_ctx_t c = { .op_to = op_to, .fb_op = fb_op, .prog = out, .n = 0, .emitted = 0 };
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (op_to[i] != FM_TO_OUT) continue;
        if (!emit(&c, i, BUS_OUT, true, 0)) return false;
    }
    return c.n == FM_GRAPH_OPS;      /* every op reachable from a carrier */
}
