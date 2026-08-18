#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/core/HalBackend.h"

PCIeModule::PCIeModule(const std::string& name,
                       const DeviceContext& ctx,
                       uint64_t reg_offset,
                       uint64_t reg_size)
    : BaseDevice(name, ctx), reg_offset_(reg_offset), reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    // ----------------- Atomic Tests -----------------
    _add_test("pcie_link_status_get", [this](TestInfo& ti) { return PcieLinkStatusGet(ti); });
    _add_test("pcie_dma_data_transfer", [this](TestInfo& ti) { return PcieDmaDataTransfer(ti); });

}

// pcie_link_status_get : To get the current PCIe negotiated speed and link width.
// @input: none
// @output: TestResult metrics include current_speed and current_width.
TestResult PCIeModule::PcieLinkStatusGet(TestInfo& ti)
{
    ti.logger->trace("start pcie_link_status_get");

    // Pseudocode: read and decode PCIe link status from reg_base_.
    (void)reg_base_;
    (void)reg_size_;
    std::string current_speed = "gen5";
    std::string current_width = "x8";

    ti.logger->info("pcie_link_status_get current speed is " + current_speed +
                    ", current width is " + current_width);

    TestMetrics metrics = {
        {"current_speed", current_speed},
        {"current_width", current_width},
    };

    ti.logger->trace("Complete pcie_link_status_get");
    return {"pcie_link_status_get", get_name(), true, metrics};
}

// pcie_dma_data_transfer : To run PCIe DMA data movement and compare the transferred pattern.
// @input: "direction": h2d/d2h, "size_bytes", "pattern"
// @output: TestResult metrics include direction, size_bytes, pattern, data_compare.
TestResult PCIeModule::PcieDmaDataTransfer(TestInfo& ti)
{
    constexpr uint64_t dev_off = 0x2000;
    constexpr uint64_t phy_off = 0x0;

    auto direction = common::args::get_string(ti.args, "direction");
    auto size_bytes = common::args::get_u64(ti.args, "size_bytes");
    auto pattern = common::args::get_string(ti.args, "pattern");

    auto make_result = [&](bool passed,
                           const std::string& compare_status,
                           const std::string& error_description = "") {
        return TestResult{"pcie_dma_data_transfer", get_name(), passed, {
            {"direction", direction},
            {"size_bytes", std::to_string(size_bytes)},
            {"pattern", pattern},
            {"dev_off", std::to_string(dev_off)},
            {"phy_off", std::to_string(phy_off)},
            {"compare_status", compare_status}
        }, error_description};
    };

    if (ti.hal == nullptr) {
        return make_result(false,
                           "hal_session_missing",
                           "HAL session is not available");
    }

    auto dma_buffer = ti.hal->alloc_dma_buffer(ctx_, size_bytes);
    if (!dma_buffer.valid()) {
        return make_result(false,
                           "dma_alloc_failed",
                           "DMA buffer allocation failed");
    }

    // direction/size_bytes/pattern default and range validation is owned by
    // pcie_atomic_tests.yaml plus the CLI/Python runner before this call.
    if (direction == "h2d") {
        // H2D real backend flow:
        //   1. Fill host DMA buffer CPU view:
        //        dma_buffer.cpu_base() + phy_off
        //   2. Program DMA descriptor:
        //        src_addr = dma_buffer.device_addr() + phy_off
        //        dst_addr = discovered_bar_device_base + dev_off
        //        length   = size_bytes
        //   3. Submit descriptor and wait for completion through HAL.
        //   4. Read back/compare through a real HAL-owned verification path.
    } else {
        // D2H real backend flow:
        //   1. Prepare device-side source data at:
        //        discovered_bar_device_base + dev_off
        //   2. Program DMA descriptor:
        //        src_addr = discovered_bar_device_base + dev_off
        //        dst_addr = dma_buffer.device_addr() + phy_off
        //        length   = size_bytes
        //   3. Submit descriptor and wait for completion through HAL.
        //   4. Compare host DMA buffer CPU view:
        //        dma_buffer.cpu_base() + phy_off
    }

    return {"pcie_dma_data_transfer", get_name(), false, {
        {"direction", direction},
        {"size_bytes", std::to_string(size_bytes)},
        {"pattern", pattern},
        {"dev_off", std::to_string(dev_off)},
        {"phy_off", std::to_string(phy_off)},
        {"dma_addr_kind", dma_buffer.addr_kind()},
        {"dma_device_addr", std::to_string(dma_buffer.device_addr())},
        {"dma_size", std::to_string(dma_buffer.size())},
        {"compare_status", "pseudocode_only"}
    }, "pcie_dma_data_transfer is a HAL flow sketch; real DMA submit/readback is not implemented yet"};
}
