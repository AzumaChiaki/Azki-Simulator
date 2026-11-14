#include "dfs_config.h"

#include <stddef.h>
#include <string.h>

struct write_step {
    unsigned domain;
    uint32_t offset;
    enum dfs_config_write_word word;
    int seed; /* -1 means an observation-only full write. */
    uint32_t preserve_mask;
    uint32_t fixed_bits;
};

static const struct write_step steps[] = {
    { DFS_CONFIG_PUBAP, 0x04, DFS_CONFIG_WRITE_04, DFS_CONFIG_SEED_04,
      UINT32_C(0xffff0000), UINT32_C(0x0fff) },
    { DFS_CONFIG_PUBAP, 0x08, DFS_CONFIG_WRITE_08, DFS_CONFIG_SEED_08,
      UINT32_C(0xffff0000), UINT32_C(0x0fff) },
    { DFS_CONFIG_PUBAP, 0x0c, DFS_CONFIG_WRITE_0C, -1,
      0, UINT32_C(0x0010babe) },
    { DFS_CONFIG_PUBAP, 0x10, DFS_CONFIG_WRITE_10, -1, 0, 0 },
    { DFS_CONFIG_PUBAP, 0x14, DFS_CONFIG_WRITE_14, -1, 0, 0 },
    { DFS_CONFIG_PUBAP, 0x18, DFS_CONFIG_WRITE_18, -1, 0, 0 },
    { DFS_CONFIG_PUBAP, 0x00, DFS_CONFIG_WRITE_00, -1, 0, 1 },
    { DFS_CONFIG_PUBCP, 0x04, DFS_CONFIG_WRITE_04, DFS_CONFIG_SEED_04,
      UINT32_C(0xffff0000), UINT32_C(0x0fff) },
    { DFS_CONFIG_PUBCP, 0x08, DFS_CONFIG_WRITE_08, DFS_CONFIG_SEED_08,
      UINT32_C(0xffff0000), UINT32_C(0x0fff) },
    { DFS_CONFIG_PUBCP, 0x0c, DFS_CONFIG_WRITE_0C, -1,
      0, UINT32_C(0x0010babe) },
    { DFS_CONFIG_PUBCP, 0x10, DFS_CONFIG_WRITE_10, -1, 0, 0 },
    { DFS_CONFIG_PUBCP, 0x14, DFS_CONFIG_WRITE_14, -1, 0, 0 },
    { DFS_CONFIG_PUBCP, 0x18, DFS_CONFIG_WRITE_18, -1, 0, 0 },
    { DFS_CONFIG_PUBCP, 0x00, DFS_CONFIG_WRITE_00, -1, 0, 1 },
    { DFS_CONFIG_PUBAP, 0x2c, DFS_CONFIG_WRITE_2C, DFS_CONFIG_SEED_2C,
      UINT32_C(0xfffffff0), 1 },
    { DFS_CONFIG_PUBCP, 0x2c, DFS_CONFIG_WRITE_2C, DFS_CONFIG_SEED_2C,
      UINT32_C(0xfffffff0), 1 }
};

#define STEP_COUNT (sizeof steps / sizeof steps[0])

static int seed_for_offset(uint32_t offset)
{
    switch (offset) {
    case 0x04: return DFS_CONFIG_SEED_04;
    case 0x08: return DFS_CONFIG_SEED_08;
    case 0x2c: return DFS_CONFIG_SEED_2C;
    default: return -1;
    }
}

static bool write_offset_supported(uint32_t offset)
{
    switch (offset) {
    case 0x00:
    case 0x04:
    case 0x08:
    case 0x0c:
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x2c:
        return true;
    default:
        return false;
    }
}

static enum dfs_config_result validate_access(
    const struct dfs_config_state *state, unsigned domain,
    uint32_t offset, unsigned size_bytes)
{
    if (state == NULL) {
        return DFS_CONFIG_INVALID_ARGUMENT;
    }
    if (!state->enabled) {
        return DFS_CONFIG_DISABLED;
    }
    if (domain >= DFS_CONFIG_DOMAIN_COUNT) {
        return DFS_CONFIG_INVALID_DOMAIN;
    }
    if (size_bytes != 4) {
        return DFS_CONFIG_INVALID_WIDTH;
    }
    if ((offset & UINT32_C(3)) != 0) {
        return DFS_CONFIG_MISALIGNED;
    }
    return DFS_CONFIG_OK;
}

void dfs_config_reset(struct dfs_config_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof *state);
    }
}

enum dfs_config_result dfs_config_start(struct dfs_config_state *state,
                                        bool enabled,
                                        const struct dfs_config_inputs *inputs)
{
    struct dfs_config_state candidate;
    unsigned domain;
    unsigned seed;

    if (state == NULL) {
        return DFS_CONFIG_INVALID_ARGUMENT;
    }
    if (!enabled) {
        dfs_config_reset(state);
        return DFS_CONFIG_OK;
    }
    if (inputs == NULL) {
        return DFS_CONFIG_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof candidate);
    for (domain = 0; domain < DFS_CONFIG_DOMAIN_COUNT; ++domain) {
        for (seed = 0; seed < DFS_CONFIG_SEED_COUNT; ++seed) {
            if (!inputs->word[domain][seed].present) {
                return DFS_CONFIG_MISSING_INITIAL;
            }
            if (inputs->word[domain][seed].value > UINT32_MAX) {
                return DFS_CONFIG_INITIAL_RANGE;
            }
            candidate.retained[domain][seed] =
                (uint32_t) inputs->word[domain][seed].value;
        }
    }
    candidate.enabled = 1;
    *state = candidate;
    return DFS_CONFIG_OK;
}

enum dfs_config_result dfs_config_read(struct dfs_config_state *state,
                                       unsigned domain, uint32_t offset,
                                       unsigned size_bytes, uint32_t *value)
{
    enum dfs_config_result result;
    int seed;

    if (value == NULL) {
        return DFS_CONFIG_INVALID_ARGUMENT;
    }
    result = validate_access(state, domain, offset, size_bytes);
    if (result != DFS_CONFIG_OK) {
        return result;
    }
    seed = seed_for_offset(offset);
    if (seed < 0) {
        return DFS_CONFIG_UNSUPPORTED_OFFSET;
    }
    *value = state->retained[domain][seed];
    if (state->phase < STEP_COUNT &&
        steps[state->phase].domain == domain &&
        steps[state->phase].offset == offset) {
        /* Experiment trace validation only, never a hardware read effect. */
        state->rmw_read_seen = 1;
    }
    return DFS_CONFIG_OK;
}

enum dfs_config_result dfs_config_write(struct dfs_config_state *state,
                                        unsigned domain, uint32_t offset,
                                        unsigned size_bytes, uint64_t value)
{
    enum dfs_config_result result;
    const struct write_step *step;
    uint32_t expected;

    result = validate_access(state, domain, offset, size_bytes);
    if (result != DFS_CONFIG_OK) {
        return result;
    }
    if (!write_offset_supported(offset)) {
        return DFS_CONFIG_UNSUPPORTED_OFFSET;
    }
    if (value > UINT32_MAX) {
        return DFS_CONFIG_VALUE_RANGE;
    }
    if (state->phase >= STEP_COUNT) {
        return DFS_CONFIG_OUT_OF_SEQUENCE;
    }
    step = &steps[state->phase];
    if (step->domain != domain || step->offset != offset) {
        return DFS_CONFIG_OUT_OF_SEQUENCE;
    }
    if (step->seed >= 0 && !state->rmw_read_seen) {
        return DFS_CONFIG_RMW_READ_REQUIRED;
    }
    expected = step->fixed_bits;
    if (step->seed >= 0) {
        expected |= state->retained[domain][step->seed] & step->preserve_mask;
    }
    if (value != expected) {
        return DFS_CONFIG_UNSUPPORTED_VALUE;
    }

    /* All rejections precede the first mutation. Record the actual raw write. */
    state->observed[domain][step->word] = (uint32_t) value;
    state->written_mask[domain] |= UINT32_C(1) << step->word;
    if (step->seed >= 0) {
        state->retained[domain][step->seed] = (uint32_t) value;
    }
    state->rmw_read_seen = 0;
    ++state->phase;
    return DFS_CONFIG_OK;
}

const char *dfs_config_result_name(enum dfs_config_result result)
{
    switch (result) {
    case DFS_CONFIG_OK: return "ok";
    case DFS_CONFIG_INVALID_ARGUMENT: return "invalid-argument";
    case DFS_CONFIG_DISABLED: return "disabled";
    case DFS_CONFIG_INVALID_DOMAIN: return "invalid-domain";
    case DFS_CONFIG_INVALID_WIDTH: return "invalid-width";
    case DFS_CONFIG_MISALIGNED: return "misaligned";
    case DFS_CONFIG_UNSUPPORTED_OFFSET: return "unsupported-offset";
    case DFS_CONFIG_MISSING_INITIAL: return "missing-initial";
    case DFS_CONFIG_INITIAL_RANGE: return "initial-range";
    case DFS_CONFIG_VALUE_RANGE: return "value-range";
    case DFS_CONFIG_OUT_OF_SEQUENCE: return "out-of-sequence";
    case DFS_CONFIG_RMW_READ_REQUIRED: return "rmw-read-required";
    case DFS_CONFIG_UNSUPPORTED_VALUE: return "unsupported-value";
    }
    return "unknown-result";
}
