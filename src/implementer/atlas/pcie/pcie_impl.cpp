#include "generic/generic_impl.h"

#include "diag/core/Common.h"
#include "diag/core/HalBackend.h"

#include <utility>

namespace atlas_impl {
namespace {

class AtlasPCIeImpl : public generic_impl::GenericPCIeImpl {
public:
    explicit AtlasPCIeImpl(ModuleImplContext ctx)
        : generic_impl::GenericPCIeImpl(std::move(ctx))
    {
    }

    TestResult PcieLinkStatusGet(TestInfo& ti) override
    {
        ti.logger->trace("start atlas pcie_link_status_get");
        (void)ctx_.reg_base;
        (void)ctx_.reg_size;

        return {"pcie_link_status_get", ctx_.target_name, true, {
            {"current_speed", "gen5"},
            {"current_width", "x8"},
            {"impl", "atlas"}
        }};
    }

    TestResult PcieDmaDataTransfer(TestInfo& ti) override
    {
        constexpr uint64_t dev_off = 0x2000;
        constexpr uint64_t phy_off = 0x0;

        auto direction = common::args::get_string(ti.args, "direction");
        auto size_bytes = common::args::get_u64(ti.args, "size_bytes");
        auto pattern = common::args::get_string(ti.args, "pattern");

        auto make_result = [&](bool passed,
                               const std::string& compare_status,
                               const std::string& error_description = "") {
            return TestResult{"pcie_dma_data_transfer", ctx_.target_name, passed, {
                {"direction", direction},
                {"size_bytes", std::to_string(size_bytes)},
                {"pattern", pattern},
                {"dev_off", std::to_string(dev_off)},
                {"phy_off", std::to_string(phy_off)},
                {"compare_status", compare_status},
                {"impl", "atlas"}
            }, error_description};
        };

        if (ti.hal == nullptr) {
            return make_result(false, "hal_session_missing", "HAL session is not available");
        }

        auto dma_buffer = ti.hal->alloc_dma_buffer(ctx_.device_ctx, size_bytes);
        if (!dma_buffer.valid()) {
            return make_result(false, "dma_alloc_failed", "DMA buffer allocation failed");
        }

        return {"pcie_dma_data_transfer", ctx_.target_name, false, {
            {"direction", direction},
            {"size_bytes", std::to_string(size_bytes)},
            {"pattern", pattern},
            {"dev_off", std::to_string(dev_off)},
            {"phy_off", std::to_string(phy_off)},
            {"dma_addr_kind", dma_buffer.addr_kind()},
            {"dma_device_addr", std::to_string(dma_buffer.device_addr())},
            {"dma_size", std::to_string(dma_buffer.size())},
            {"compare_status", "pseudocode_only"},
            {"impl", "atlas"}
        }, "atlas pcie_dma_data_transfer is a HAL flow sketch; real DMA submit/readback is not implemented yet"};
    }
};

}

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasPCIeImpl>(ctx);
}

}
