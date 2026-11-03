#ifndef MOCK_PROTONHAL_COMPONENTS_PCIE_PCIE_H
#define MOCK_PROTONHAL_COMPONENTS_PCIE_PCIE_H

#include <phal/phal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct phal_pcie_aperture {
    uint8_t identity;
    uint64_t target_addr;
    uint64_t size;
} phal_pcie_aperture_t;

phal_status_t phal_pcie_aperture_set(phal_ctx_t* ctx,
                                     uint32_t bar_index,
                                     uint8_t aperture_index,
                                     const phal_pcie_aperture_t* aperture);

phal_status_t phal_pcie_aperture_get(phal_ctx_t* ctx,
                                     uint32_t bar_index,
                                     uint8_t aperture_index,
                                     phal_pcie_aperture_t* aperture);

#ifdef __cplusplus
}
#endif

#endif
