#include <phal/components/isi/isi.h>

phal_status_t phal_component_isi_linkup(phal_ctx_t* ctx, uint32_t timeout_us)
{
    (void)timeout_us;
    if (ctx == 0) return PHAL_STATUS_INVALID;
    /* Compile/link only. This mock cannot establish or validate a real link. */
    return PHAL_STATUS_ERROR;
}
