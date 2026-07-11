#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* TLV (Tag-Length-Value) container primitives for serializing project state.
 * Encoding: little-endian throughout.
 * Section format: {tag u32, version u8, length u32, payload}.
 * All integers written/read via explicit byte shifts (no unaligned pointer casts).
 * Floats via memcpy to/from uint32_t then the u32 path.
 */

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool err;
} tlv_writer_t;

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
    bool err;
} tlv_reader_t;

/* Writer API */
void tlv_writer_init(tlv_writer_t *w, uint8_t *buf, size_t cap);
bool tlv_put_u8(tlv_writer_t *w, uint8_t v);
bool tlv_put_u16(tlv_writer_t *w, uint16_t v);
bool tlv_put_u32(tlv_writer_t *w, uint32_t v);
bool tlv_put_i8(tlv_writer_t *w, int8_t v);
bool tlv_put_i16(tlv_writer_t *w, int16_t v);
bool tlv_put_f32(tlv_writer_t *w, float v);
bool tlv_put_bytes(tlv_writer_t *w, const void *p, size_t n);
size_t tlv_begin_section(tlv_writer_t *w, uint32_t tag, uint8_t version);
bool   tlv_end_section(tlv_writer_t *w, size_t handle);

/* Reader API */
void tlv_reader_init(tlv_reader_t *r, const uint8_t *buf, size_t len);
bool tlv_get_u8(tlv_reader_t *r, uint8_t *v);
bool tlv_get_u16(tlv_reader_t *r, uint16_t *v);
bool tlv_get_u32(tlv_reader_t *r, uint32_t *v);
bool tlv_get_i8(tlv_reader_t *r, int8_t *v);
bool tlv_get_i16(tlv_reader_t *r, int16_t *v);
bool tlv_get_f32(tlv_reader_t *r, float *v);
bool tlv_get_bytes(tlv_reader_t *r, void *p, size_t n);
/* Iterate sections: fills tag/version and a bounded sub-reader over the
 * section payload; advances the outer reader past it. False at end/error. */
bool tlv_next_section(tlv_reader_t *r, uint32_t *tag, uint8_t *version,
                      tlv_reader_t *body);

/* CRC checksum */
uint32_t project_crc32(const void *p, size_t n);

#ifdef __cplusplus
}
#endif
