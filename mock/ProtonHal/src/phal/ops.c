#include <phal/phal.h>

phal_status_t phal_write(phal_ctx_t* ctx, uintptr_t addr, uint32_t data)
{
    if (ctx == 0 || ctx->env.ops == 0 || ctx->env.ops->write == 0)
        return PHAL_STATUS_INVALID;
    return ctx->env.ops->write(&ctx->env, addr, data);
}

phal_status_t phal_read(phal_ctx_t* ctx, uintptr_t addr, uint32_t* data)
{
    if (ctx == 0 || ctx->env.ops == 0 || ctx->env.ops->read == 0 || data == 0)
        return PHAL_STATUS_INVALID;
    return ctx->env.ops->read(&ctx->env, addr, data);
}

phal_status_t phal_field_write(phal_ctx_t* ctx, uintptr_t addr,
                               uint32_t bit_mask, uint32_t bit_position,
                               uint32_t value)
{
    uint32_t data = 0;
    const phal_status_t status = phal_read(ctx, addr, &data);
    if (status != PHAL_STATUS_OK) return status;
    data = (data & ~bit_mask) | ((value << bit_position) & bit_mask);
    return phal_write(ctx, addr, data);
}
