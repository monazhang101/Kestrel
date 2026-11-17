#ifndef MOCK_PROTONHAL_ISI_H
#define MOCK_PROTONHAL_ISI_H

#include <phal/phal.h>

#ifdef __cplusplus
extern "C" {
#endif

phal_status_t phal_component_isi_linkup(phal_ctx_t* ctx, uint32_t timeout_us);

#ifdef __cplusplus
}
#endif
#endif
