#include <phal/components/pcie/pcie.h>

static int aperture_index_valid(uint32_t bar_index, uint8_t aperture_index)
{
    return bar_index < PHAL_MOCK_BAR_COUNT &&
           aperture_index < PHAL_MOCK_APERTURE_COUNT;
}

phal_status_t phal_pcie_aperture_set(phal_ctx_t* ctx,
                                     uint32_t bar_index,
                                     uint8_t aperture_index,
                                     const phal_pcie_aperture_t* aperture)
{
    phal_mock_aperture_slot_t* slot;

    if (ctx == 0 || aperture == 0 ||
        !aperture_index_valid(bar_index, aperture_index)) {
        return PHAL_STATUS_INVALID;
    }

    slot = &ctx->apertures[bar_index][aperture_index];
    slot->identity = aperture->identity;
    slot->target_addr = aperture->target_addr;
    slot->size = aperture->size;
    return PHAL_STATUS_OK;
}

phal_status_t phal_pcie_aperture_get(phal_ctx_t* ctx,
                                     uint32_t bar_index,
                                     uint8_t aperture_index,
                                     phal_pcie_aperture_t* aperture)
{
    const phal_mock_aperture_slot_t* slot;

    if (ctx == 0 || aperture == 0 ||
        !aperture_index_valid(bar_index, aperture_index)) {
        return PHAL_STATUS_INVALID;
    }

    slot = &ctx->apertures[bar_index][aperture_index];
    aperture->identity = slot->identity;
    aperture->target_addr = slot->target_addr;
    aperture->size = slot->size;
    return PHAL_STATUS_OK;
}
