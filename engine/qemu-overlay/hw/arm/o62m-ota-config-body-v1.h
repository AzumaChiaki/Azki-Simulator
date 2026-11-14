#ifndef O62M_OTA_CONFIG_BODY_V1_H
#define O62M_OTA_CONFIG_BODY_V1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "o62m-ota-config-catalog-v1.h"

/* Original board81 software configuration stream, retained words only.
 * No physical pad, electrical, mux, security, SET-alias, clock or ready effect.
 * The BSP independently requires actual slot24 ownership and the matching
 * open envelope bank. Initial words are explicit inputs, never reset facts.
 */
typedef enum O62MOtaConfigBodyResultV1 {
    O62M_OTA_CONFIG_BODY_OK = 0,
    O62M_OTA_CONFIG_BODY_BAD_ARGUMENT,
    O62M_OTA_CONFIG_BODY_BAD_COUNT,
    O62M_OTA_CONFIG_BODY_UNSUPPORTED_BOARD,
    O62M_OTA_CONFIG_BODY_DISABLED,
    O62M_OTA_CONFIG_BODY_INVALID_STATE,
    O62M_OTA_CONFIG_BODY_UNSUPPORTED_ACCESS,
    O62M_OTA_CONFIG_BODY_BAD_SEQUENCE,
    O62M_OTA_CONFIG_BODY_STREAM_COMPLETE
} O62MOtaConfigBodyResultV1;

typedef struct O62MOtaConfigBodyV1 {
    uint32_t word[O62M_OTA_CONFIG_BODY_WORDS_V1];
    uint32_t board_code, cursor, reads, writes;
    bool initialized, pending_write;
} O62MOtaConfigBodyV1;

static inline const char *o62m_ota_config_body_reason_v1(O62MOtaConfigBodyResultV1 r)
{
    switch(r) {
    case O62M_OTA_CONFIG_BODY_OK: return "ok";
    case O62M_OTA_CONFIG_BODY_BAD_ARGUMENT: return "bad-argument";
    case O62M_OTA_CONFIG_BODY_BAD_COUNT: return "150-explicit-initial-fullwords-required";
    case O62M_OTA_CONFIG_BODY_UNSUPPORTED_BOARD: return "only-original-board81-catalog-supported";
    case O62M_OTA_CONFIG_BODY_DISABLED: return "explicit-initial-vector-required";
    case O62M_OTA_CONFIG_BODY_INVALID_STATE: return "invalid-configuration-observation-state";
    case O62M_OTA_CONFIG_BODY_UNSUPPORTED_ACCESS: return "only-exact-original32bit-body-accesses-supported";
    case O62M_OTA_CONFIG_BODY_BAD_SEQUENCE: return "ordered-original-read-conditional-write-required";
    case O62M_OTA_CONFIG_BODY_STREAM_COMPLETE: return "original150word-stream-complete";
    }
    return "invalid-result";
}

static inline bool o62m_ota_config_body_address_v1(uint64_t address)
{
    for(unsigned i=0;i<O62M_OTA_CONFIG_BODY_WORDS_V1;++i) {
        if(address==o62m_ota_config_words_v1[i].address) { return true; }
    }
    return false;
}

/* Explicit host construction, not a guest rearm or hardware reset command.
 * The temporary also makes rejected initialization atomic and permits a
 * caller's existing word vector to be copied before destination assignment.
 */
static inline O62MOtaConfigBodyResultV1 o62m_ota_config_body_init_v1(
        O62MOtaConfigBodyV1 *model, const uint32_t *initial, size_t count,
        uint32_t board_code)
{
    O62MOtaConfigBodyV1 next={0};
    if(!model || !initial) { return O62M_OTA_CONFIG_BODY_BAD_ARGUMENT; }
    if(count!=O62M_OTA_CONFIG_BODY_WORDS_V1) { return O62M_OTA_CONFIG_BODY_BAD_COUNT; }
    if(board_code!=0x81) { return O62M_OTA_CONFIG_BODY_UNSUPPORTED_BOARD; }
    memcpy(next.word,initial,sizeof(next.word));
    next.board_code=board_code;
    next.initialized=true;
    *model=next;
    return O62M_OTA_CONFIG_BODY_OK;
}

static inline bool o62m_ota_config_body_valid_v1(const O62MOtaConfigBodyV1 *model)
{
    if(model->board_code!=0x81 || model->cursor>O62M_OTA_CONFIG_BODY_WORDS_V1 ||
       model->reads>O62M_OTA_CONFIG_BODY_WORDS_V1 || model->writes>model->cursor ||
       (model->pending_write && model->cursor==O62M_OTA_CONFIG_BODY_WORDS_V1) ||
       model->reads!=model->cursor+(model->pending_write?1u:0u)) { return false; }
    for(unsigned i=0;i<model->cursor;++i) {
        if(model->word[i]!=o62m_ota_config_words_v1[i].desired) { return false; }
    }
    if(model->pending_write &&
       model->word[model->cursor]==o62m_ota_config_words_v1[model->cursor].desired) {
        return false;
    }
    return true;
}

/* value must be a separately stored uint64_t, not overlapping model storage.
 * Exact original read returns the retained fullword; an unequal word requires
 * the original exact desired write before advancement. Equal skips the write.
 * Any rejection leaves every model/value byte unchanged. Only a new explicit
 * host experiment may initialize another stream; no guest rearm is supplied.
 */
static inline O62MOtaConfigBodyResultV1 o62m_ota_config_body_access_v1(
        O62MOtaConfigBodyV1 *model, uint64_t pc, uint64_t address,
        unsigned size, bool write, uint64_t *value)
{
    O62MOtaConfigBodyV1 next;
    const O62MOtaConfigWordV1 *definition;
    uint64_t result;
    if(!model || !value) { return O62M_OTA_CONFIG_BODY_BAD_ARGUMENT; }
    if(!model->initialized) { return O62M_OTA_CONFIG_BODY_DISABLED; }
    if(!o62m_ota_config_body_valid_v1(model)) { return O62M_OTA_CONFIG_BODY_INVALID_STATE; }
    if(size!=4 || !o62m_ota_config_body_address_v1(address) ||
       (write?pc!=UINT64_C(0x605e8300):pc!=UINT64_C(0x605e82f8))) {
        return O62M_OTA_CONFIG_BODY_UNSUPPORTED_ACCESS;
    }
    if(model->cursor==O62M_OTA_CONFIG_BODY_WORDS_V1) {
        return O62M_OTA_CONFIG_BODY_STREAM_COMPLETE;
    }
    definition=&o62m_ota_config_words_v1[model->cursor];
    if(address!=definition->address || write!=model->pending_write ||
       (write && *value!=definition->desired)) {
        return O62M_OTA_CONFIG_BODY_BAD_SEQUENCE;
    }
    next=*model;
    result=*value;
    if(write) {
        next.word[next.cursor]=definition->desired;
        next.pending_write=false;
        next.writes++;
        next.cursor++;
    } else {
        result=next.word[next.cursor];
        next.reads++;
        if(result==definition->desired) { next.cursor++; }
        else { next.pending_write=true; }
    }
    *model=next;
    *value=result;
    return O62M_OTA_CONFIG_BODY_OK;
}

#endif
