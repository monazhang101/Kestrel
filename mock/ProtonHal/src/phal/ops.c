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
