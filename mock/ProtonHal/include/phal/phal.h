#ifndef MOCK_PROTONHAL_PHAL_H
#define MOCK_PROTONHAL_PHAL_H

/* Compile and local-smoke compatibility only; this is not a hardware model. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum phal_status {
    PHAL_STATUS_OK = 0,
    PHAL_STATUS_ERROR = 1,
    PHAL_STATUS_INVALID = 2,
} phal_status_t;

typedef enum phal_project {
    PHAL_PROJECT_GENERIC = 0,
    PHAL_PROJECT_ATLAS = 1,
    PHAL_PROJECT_ATLAS_M = 2,
} phal_project_t;

typedef struct env env_t;

typedef struct hw_ops {
    phal_status_t (*write)(env_t* env, uintptr_t addr, uint32_t data);
    phal_status_t (*read)(env_t* env, uintptr_t addr, uint32_t* data);
} hw_ops_t;

struct env {
    uintptr_t base;
    void* priv;
    const hw_ops_t* ops;
};

typedef struct env_config {
    uintptr_t base;
    void* user_data;
    const hw_ops_t* hw_ops;
} env_config_t;

typedef struct phal_config {
    env_config_t env_config;
    phal_project_t project_config;
} phal_config_t;

enum {
    PHAL_MOCK_BAR_COUNT = 6,
    PHAL_MOCK_APERTURE_COUNT = 8,
};

typedef struct phal_mock_aperture_slot {
    uint8_t identity;
    uint64_t target_addr;
    uint64_t size;
} phal_mock_aperture_slot_t;

typedef struct phal_ctx {
    env_t env;
    phal_project_t project_config;
    phal_mock_aperture_slot_t
        apertures[PHAL_MOCK_BAR_COUNT][PHAL_MOCK_APERTURE_COUNT];
} phal_ctx_t;

phal_status_t phal_init(phal_ctx_t* ctx, const phal_config_t* config);
void phal_deinit(phal_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif
