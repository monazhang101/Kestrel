#include "diag/core/TestTarget.h"
#include "diag/core/HalContext.h"
#include "diag/device/TPUDevice.h"

TestStatus TestTarget::run_testcase(const std::string& test_name, const TestArgs& args,
                                    Logger* logger, HalContext* hal)
{
    return run_testcase_with(test_name, args, logger,
        [&](TestInfo& ti, const TestcaseDefinition& test) -> TestStatus {
            ti.hal = hal;
            if (hal == nullptr) {
                ti.logger->error("testcase requires a HAL context; run through DeviceManager");
                return PHAL_STATUS_ERROR;
            }
            if (!hal->has_device() || hal->device_context().bdf != ctx_.bdf) {
                ti.logger->error("HAL is not bound to testcase device: " + ctx_.bdf);
                return PHAL_STATUS_ERROR;
            }
            const auto driver = ti.args.find("drv");
            if (driver != ti.args.end()) {
                const auto status = hal->configure_host_dma(driver->second);
                if (status != PHAL_STATUS_OK) return status;
            }
            std::string context_error;
            auto testcase_scope =
                hal->begin_testcase(&context_error, test.ctx_resources);
            if (!testcase_scope) {
                ti.logger->error("context initialization failed: " + context_error);
                return PHAL_STATUS_ERROR;
            }
            ti.phal_ctx = hal->phal_context();
            return test.execute(ti);
        });
}

void TestTarget::set_device(TPUDevice* device)
{
    device_ = device;
    owner_name_ = device ? device->get_name() : std::string{};
}
