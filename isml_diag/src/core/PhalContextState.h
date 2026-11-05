#pragma once
#include "diag/core/PhalBridge.h"
#include "diag/core/TestInfo.h"

class PhalBridge::ScopedContext::State {
public:
    Logger logger;
    DeviceContext device_ctx;
    IhalIO ihalIO;
    phal_ctx_t ctx{};
    bool initialized = false;

    ~State()
    {
        if (initialized) {
            phal_deinit(&ctx);
        }
    }
};
