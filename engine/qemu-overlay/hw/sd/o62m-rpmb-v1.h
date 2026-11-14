#ifndef O62M_RPMB_V1_H
#define O62M_RPMB_V1_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define O62M_RPMB_BLOCKS_V1 512u
#define O62M_RPMB_FRAME_V1 512u
#define O62M_RPMB_DATA_V1 256u
/* The MAC callback authenticates each frame's 284-byte protocol suffix. */
typedef bool (*O62MRpmbMacV1)(const uint8_t key[32], const uint8_t *frames,
                             size_t count, uint8_t output[32]);
typedef struct O62MRpmbV1 {
    uint8_t media[O62M_RPMB_BLOCKS_V1 * O62M_RPMB_DATA_V1];
    uint8_t key[32], request[512], result_frame[512];
    uint8_t response[O62M_RPMB_BLOCKS_V1 * O62M_RPMB_FRAME_V1];
    uint8_t read_nonce[16];
    uint32_t counter;
    uint16_t read_address, read_count, response_count;
    bool programmed, result_valid, read_pending, response_ready;
    uint64_t requests, key_programs, authenticated_writes, read_requests;
    uint64_t counter_requests, result_requests, failed_results, cancels;
} O62MRpmbV1;
static inline uint16_t o62m_rpmb_be16_v1(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static inline uint32_t o62m_rpmb_be32_v1(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void o62m_rpmb_put16_v1(uint8_t *p, uint16_t x)
{ p[0] = x >> 8; p[1] = x; }
static inline void o62m_rpmb_put32_v1(uint8_t *p, uint32_t x)
{ p[0] = x >> 24; p[1] = x >> 16; p[2] = x >> 8; p[3] = x; }
static inline void o62m_rpmb_wipe_v1(void *p, size_t n)
{ volatile uint8_t *v = p; while (n--) *v++ = 0; }
static inline void o62m_rpmb_cancel_v1(O62MRpmbV1 *s)
{
    if (s->read_pending || s->response_ready || s->result_valid) s->cancels++;
    s->read_pending = s->response_ready = s->result_valid = false;
    s->response_count = s->read_count = s->read_address = 0;
    o62m_rpmb_wipe_v1(s->request, sizeof(s->request));
    o62m_rpmb_wipe_v1(s->result_frame, sizeof(s->result_frame));
    o62m_rpmb_wipe_v1(s->response, sizeof(s->response));
    o62m_rpmb_wipe_v1(s->read_nonce, sizeof(s->read_nonce));
}
static inline void o62m_rpmb_response_v1(uint8_t *f, uint16_t type,
                                        uint16_t result, uint32_t counter)
{
    memset(f, 0, 512);
    o62m_rpmb_put32_v1(f + 500, counter);
    o62m_rpmb_put16_v1(f + 508, result);
    o62m_rpmb_put16_v1(f + 510, type);
}
static inline bool o62m_rpmb_sign_v1(O62MRpmbV1 *s, uint8_t *frames,
                                    size_t count, O62MRpmbMacV1 mac)
{
    uint8_t digest[32];
    bool ok = mac(s->key, frames, count, digest);
    if (ok) memcpy(frames + (count - 1) * 512 + 196, digest, 32);
    o62m_rpmb_wipe_v1(digest, sizeof(digest));
    return ok;
}
/* One reliable block is advertised. A request is processed only after its
 * complete frame has arrived through the card data transport. */
static inline bool o62m_rpmb_request_v1(O62MRpmbV1 *s, const uint8_t f[512],
                                      bool reliable, O62MRpmbMacV1 mac)
{
    uint16_t op = o62m_rpmb_be16_v1(f + 510), result = 0;
    uint16_t address = o62m_rpmb_be16_v1(f + 504);
    uint16_t count = o62m_rpmb_be16_v1(f + 506);
    uint8_t digest[32];
    uint8_t answer[512];
    if (!mac || s->read_pending || s->response_ready || op < 1 || op > 5 ||
        ((op == 1 || op == 3) != reliable) ||
        ((op == 1 || op == 3) && s->result_valid) ||
        (op == 5 && !s->result_valid)) return false;
    if (op == 1) {
        if (s->programmed) result = 1;
        o62m_rpmb_response_v1(answer, 0x100, result, 0);
        if (!result) {
            memcpy(s->key, f + 196, 32);
            s->programmed = true;
            s->key_programs++;
        }
        memcpy(s->result_frame, answer, 512);
        s->result_valid = true;
    } else if (op == 2) {
        result = s->programmed ? 0 : 7;
        o62m_rpmb_response_v1(answer, 0x200, result, s->counter);
        memcpy(answer + 484, f + 484, 16);
        if (s->programmed && !o62m_rpmb_sign_v1(s, answer, 1, mac)) return false;
        memcpy(s->response, answer, 512);
        s->response_count = 1;
        s->response_ready = true;
        s->counter_requests++;
    } else if (op == 3) {
        if (!s->programmed) result = 7;
        else if (count != 1 || address >= O62M_RPMB_BLOCKS_V1) result = 4;
        else if (s->counter == UINT32_MAX) result = 0x83;
        else if (o62m_rpmb_be32_v1(f + 500) != s->counter) result = 3;
        else {
            if (!mac(s->key, f, 1, digest)) return false;
            if (memcmp(digest, f + 196, 32)) result = 2;
            o62m_rpmb_wipe_v1(digest, sizeof(digest));
        }
        o62m_rpmb_response_v1(answer, 0x300, result,
                             s->counter + (result == 0));
        o62m_rpmb_put16_v1(answer + 504, address);
        if (s->programmed && !o62m_rpmb_sign_v1(s, answer, 1, mac)) return false;
        if (!result) {
            memcpy(s->media + address * 256u, f + 228, 256);
            s->counter++;
            s->authenticated_writes++;
        }
        memcpy(s->result_frame, answer, 512);
        s->result_valid = true;
    } else if (op == 4) {
        s->read_address = address;
        s->read_count = count;
        memcpy(s->read_nonce, f + 484, 16);
        s->read_pending = true;
        s->read_requests++;
    } else {
        memcpy(s->response, s->result_frame, 512);
        s->response_count = 1;
        s->response_ready = true;
        s->result_valid = false;
        o62m_rpmb_wipe_v1(s->result_frame, sizeof(s->result_frame));
        s->result_requests++;
    }
    s->requests++;
    if (result) s->failed_results++;
    o62m_rpmb_wipe_v1(answer, sizeof(answer));
    return true;
}
/* CMD18 supplies the actual response transfer length. Read request block_count
 * may be zero (legacy eMMC RPMB ABI), otherwise it must match that length. */
static inline bool o62m_rpmb_prepare_read_v1(O62MRpmbV1 *s, uint16_t count,
                                           O62MRpmbMacV1 mac)
{
    uint16_t result;
    if (!count || count > O62M_RPMB_BLOCKS_V1) return false;
    if (s->response_ready) return count == s->response_count;
    if (!s->read_pending || (s->read_count && s->read_count != count)) return false;
    result = !s->programmed ? 7 :
             (s->read_address >= O62M_RPMB_BLOCKS_V1 ||
              count > O62M_RPMB_BLOCKS_V1 - s->read_address) ? 4 : 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *f = s->response + i * 512u;
        o62m_rpmb_response_v1(f, 0x400, result, 0);
        memcpy(f + 484, s->read_nonce, 16);
        o62m_rpmb_put16_v1(f + 504, s->read_address);
        o62m_rpmb_put16_v1(f + 506, count);
        if (!result) memcpy(f + 228, s->media + (s->read_address + i) * 256u, 256);
    }
    if (s->programmed && !o62m_rpmb_sign_v1(s, s->response, count, mac)) {
        o62m_rpmb_wipe_v1(s->response, count * 512u);
        return false;
    }
    s->read_pending = false;
    s->response_ready = true;
    s->response_count = count;
    if (result) s->failed_results++;
    return true;
}
static inline void o62m_rpmb_read_done_v1(O62MRpmbV1 *s)
{
    o62m_rpmb_wipe_v1(s->response, s->response_count * 512u);
    o62m_rpmb_wipe_v1(s->read_nonce, sizeof(s->read_nonce));
    s->response_ready = false;
    s->response_count = 0;
}
#endif
