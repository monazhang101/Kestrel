#include <phal/phal.h>

#include <string.h>

phal_status_t phal_init(phal_ctx_t* ctx, const phal_config_t* config)
{
    if (ctx == 0 || config == 0) {
        return PHAL_STATUS_INVALID;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->env.base = config->env_config.base;
    ctx->env.priv = config->env_config.user_data;
    ctx->env.ops = config->env_config.hw_ops;
    ctx->project_config = config->project_config;
    return PHAL_STATUS_OK;
}

void phal_deinit(phal_ctx_t* ctx)
{
    if (ctx != 0) {
        memset(ctx, 0, sizeof(*ctx));
    }
}
