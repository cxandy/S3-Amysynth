#include "project_tlv.h"
#include "esp_rom_crc.h"
#include <string.h>

void tlv_writer_init(tlv_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf; w->cap = cap; w->len = 0; w->err = (buf == NULL);
}

static bool wr(tlv_writer_t *w, const uint8_t *p, size_t n)
{
    /* Subtraction form: len <= cap is an invariant, so cap - len cannot
     * underflow, and a huge n cannot wrap the comparison. */
    if (w->err || n > w->cap - w->len) { w->err = true; return false; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
    return true;
}

bool tlv_put_u8(tlv_writer_t *w, uint8_t v)  { return wr(w, &v, 1); }

bool tlv_put_u16(tlv_writer_t *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return wr(w, b, 2);
}

bool tlv_put_u32(tlv_writer_t *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return wr(w, b, 4);
}

bool tlv_put_i8(tlv_writer_t *w, int8_t v)   { return tlv_put_u8(w, (uint8_t)v); }
bool tlv_put_i16(tlv_writer_t *w, int16_t v) { return tlv_put_u16(w, (uint16_t)v); }

bool tlv_put_f32(tlv_writer_t *w, float v)
{
    uint32_t u; memcpy(&u, &v, 4); return tlv_put_u32(w, u);
}

bool tlv_put_bytes(tlv_writer_t *w, const void *p, size_t n)
{
    return wr(w, (const uint8_t *)p, n);
}

size_t tlv_begin_section(tlv_writer_t *w, uint32_t tag, uint8_t version)
{
    tlv_put_u32(w, tag);
    tlv_put_u8(w, version);
    size_t handle = w->len;          /* offset of the length field */
    tlv_put_u32(w, 0);               /* placeholder */
    return handle;
}

bool tlv_end_section(tlv_writer_t *w, size_t handle)
{
    if (w->err) return false;
    uint32_t body_len = (uint32_t)(w->len - handle - 4);
    w->buf[handle + 0] = (uint8_t)body_len;
    w->buf[handle + 1] = (uint8_t)(body_len >> 8);
    w->buf[handle + 2] = (uint8_t)(body_len >> 16);
    w->buf[handle + 3] = (uint8_t)(body_len >> 24);
    return true;
}

/* ── Reader ── */

void tlv_reader_init(tlv_reader_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf; r->len = len; r->pos = 0; r->err = (buf == NULL);
}

static bool rd(tlv_reader_t *r, uint8_t *p, size_t n)
{
    /* Subtraction form: pos <= len is an invariant, so len - pos cannot
     * underflow, and a huge n cannot wrap the comparison. */
    if (r->err || n > r->len - r->pos) { r->err = true; return false; }
    memcpy(p, r->buf + r->pos, n);
    r->pos += n;
    return true;
}

bool tlv_get_u8(tlv_reader_t *r, uint8_t *v) { return rd(r, v, 1); }

bool tlv_get_u16(tlv_reader_t *r, uint16_t *v)
{
    uint8_t b[2]; if (!rd(r, b, 2)) return false;
    *v = (uint16_t)(b[0] | (b[1] << 8)); return true;
}

bool tlv_get_u32(tlv_reader_t *r, uint32_t *v)
{
    uint8_t b[4]; if (!rd(r, b, 4)) return false;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
       | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

bool tlv_get_i8(tlv_reader_t *r, int8_t *v)   { return tlv_get_u8(r, (uint8_t *)v); }
bool tlv_get_i16(tlv_reader_t *r, int16_t *v) { return tlv_get_u16(r, (uint16_t *)v); }

bool tlv_get_f32(tlv_reader_t *r, float *v)
{
    uint32_t u; if (!tlv_get_u32(r, &u)) return false;
    memcpy(v, &u, 4); return true;
}

bool tlv_get_bytes(tlv_reader_t *r, void *p, size_t n)
{
    return rd(r, (uint8_t *)p, n);
}

bool tlv_next_section(tlv_reader_t *r, uint32_t *tag, uint8_t *version,
                      tlv_reader_t *body)
{
    if (r->err || r->pos >= r->len) return false;
    uint32_t t, blen; uint8_t ver;
    if (!tlv_get_u32(r, &t) || !tlv_get_u8(r, &ver) || !tlv_get_u32(r, &blen))
        return false;
    /* blen comes off persisted (untrusted) data - subtraction form so a
     * near-UINT32_MAX length cannot wrap past the bounds check. */
    if (blen > r->len - r->pos) { r->err = true; return false; }
    tlv_reader_init(body, r->buf + r->pos, blen);
    r->pos += blen;
    *tag = t; *version = ver;
    return true;
}

uint32_t project_crc32(const void *p, size_t n)
{
    return esp_rom_crc32_le(0, (const uint8_t *)p, n);
}
