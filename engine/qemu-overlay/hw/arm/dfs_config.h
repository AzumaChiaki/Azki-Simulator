#ifndef XRING_RESEARCH_DFS_CONFIG_H
#define XRING_RESEARCH_DFS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Bounded experiment: retained config + observed original boot writes only. */
enum dfs_config_domain {
    DFS_CONFIG_PUBAP = 0,
    DFS_CONFIG_PUBCP = 1,
    DFS_CONFIG_DOMAIN_COUNT = 2
};

enum dfs_config_seed_word {
    DFS_CONFIG_SEED_04 = 0,
    DFS_CONFIG_SEED_08 = 1,
    DFS_CONFIG_SEED_2C = 2,
    DFS_CONFIG_SEED_COUNT = 3
};

enum dfs_config_write_word {
    DFS_CONFIG_WRITE_04 = 0,
    DFS_CONFIG_WRITE_08 = 1,
    DFS_CONFIG_WRITE_0C = 2,
    DFS_CONFIG_WRITE_10 = 3,
    DFS_CONFIG_WRITE_14 = 4,
    DFS_CONFIG_WRITE_18 = 5,
    DFS_CONFIG_WRITE_00 = 6,
    DFS_CONFIG_WRITE_2C = 7,
    DFS_CONFIG_WRITE_COUNT = 8
};

enum dfs_config_result {
    DFS_CONFIG_OK = 0,
    DFS_CONFIG_INVALID_ARGUMENT,
    DFS_CONFIG_DISABLED,
    DFS_CONFIG_INVALID_DOMAIN,
    DFS_CONFIG_INVALID_WIDTH,
    DFS_CONFIG_MISALIGNED,
    DFS_CONFIG_UNSUPPORTED_OFFSET,
    DFS_CONFIG_MISSING_INITIAL,
    DFS_CONFIG_INITIAL_RANGE,
    DFS_CONFIG_VALUE_RANGE,
    DFS_CONFIG_OUT_OF_SEQUENCE,
    DFS_CONFIG_RMW_READ_REQUIRED,
    DFS_CONFIG_UNSUPPORTED_VALUE
};

struct dfs_config_initial_word {
    bool present;
    /* Wider transport permits rejecting an input outside the full u32 range. */
    uint64_t value;
};

struct dfs_config_inputs {
    /* Six independent presence flags; present=true,value=0 is explicit input. */
    struct dfs_config_initial_word word[DFS_CONFIG_DOMAIN_COUNT]
                                       [DFS_CONFIG_SEED_COUNT];
};

/* Public for audit/integration snapshots. Do not mutate directly after start. */
struct dfs_config_state {
    uint32_t enabled;
    /* Number of accepted original-boot writes, 0..16; 16 means observed. */
    uint32_t phase;
    uint32_t rmw_read_seen;
    uint32_t retained[DFS_CONFIG_DOMAIN_COUNT][DFS_CONFIG_SEED_COUNT];
    /* Raw guest values, meaningful only when the corresponding written bit is 1. */
    uint32_t observed[DFS_CONFIG_DOMAIN_COUNT][DFS_CONFIG_WRITE_COUNT];
    uint32_t written_mask[DFS_CONFIG_DOMAIN_COUNT];
};

/* Experiment lifecycle, NOT a claim about hardware reset or reset values. */
void dfs_config_reset(struct dfs_config_state *state);

/*
 * enabled=false clears/disables the experiment; inputs may then be NULL.
 * enabled=true requires all six explicit u32 inputs and starts a fresh sequence.
 * Invalid start leaves the existing state unchanged.
 */
enum dfs_config_result dfs_config_start(struct dfs_config_state *state,
                                        bool enabled,
                                        const struct dfs_config_inputs *inputs);

/*
 * Only domain+offset: no physical/secure aliases are defined by this API.
 * size_bytes must equal 4 and offset must be aligned. No access splitting.
 * read: only 04/08/2c; returns retained input or the accepted retained config.
 * write: exactly the original boot sequence described in CONTRACT.md.
 * Every error leaves state and read output unchanged.
 * Pointers must refer to separate, valid objects; read output must not alias state.
 */
enum dfs_config_result dfs_config_read(struct dfs_config_state *state,
                                       unsigned domain, uint32_t offset,
                                       unsigned size_bytes, uint32_t *value);
enum dfs_config_result dfs_config_write(struct dfs_config_state *state,
                                        unsigned domain, uint32_t offset,
                                        unsigned size_bytes, uint64_t value);

const char *dfs_config_result_name(enum dfs_config_result result);

#endif
