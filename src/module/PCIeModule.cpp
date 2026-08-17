#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"

PCIeModule::PCIeModule(const std::string& name,
                       const DeviceContext& ctx,
                       uint64_t reg_offset,
                       uint64_t reg_size)
    : BaseDevice(name, ctx), reg_offset_(reg_offset), reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("pcie_link_status_get", [this](TestInfo& ti) { return PcieLinkStatusGet(ti); });
    _add_test("pcie_dma_data_transfer", [this](TestInfo& ti) { return PcieDmaDataTransfer(ti); });
    /*
    _add_test("pcie_enum_check", [this](TestInfo& ti) { return PcieEnumCheck(ti); });
    _add_test("pcie_cap_list_check", [this](TestInfo& ti) { return PcieCapListCheck(ti); });
    _add_test("pcie_ext_cap_list_check", [this](TestInfo& ti) { return PcieExtCapListCheck(ti); });
    _add_test("pcie_reg_scan", [this](TestInfo& ti) { return PcieRegScan(ti); });
    _add_test("pcie_pmu_reg_scan", [this](TestInfo& ti) { return PciePmuRegScan(ti); });
    _add_test("pcie_bar_size_get", [this](TestInfo& ti) { return PcieBarSizeGet(ti); });
    _add_test("pcie_dmem_mmio_scan", [this](TestInfo& ti) { return PcieDmemMmioScan(ti); });
    _add_test("pcie_dmem_hdma_scan", [this](TestInfo& ti) { return PcieDmemHdmaScan(ti); });
    _add_test("pcie_dmem_reg_scan", [this](TestInfo& ti) { return PcieDmemRegScan(ti); });
    _add_test("pcie_vfio_hdma_intr_setup", [this](TestInfo& ti) { return PcieVfioHdmaIntrSetup(ti); });
    _add_test("pcie_vfio_wait_msi_intr", [this](TestInfo& ti) { return PcieVfioWaitMsiIntr(ti); });
    _add_test("pcie_parallel_dma_with_compare", [this](TestInfo& ti) { return PcieParallelDmaWithCompare(ti); });
    _add_test("pcie_mc_intr_is_set", [this](TestInfo& ti) { return PcieMcIntrIsSet(ti); });
    _add_test("pcie_pmu_intr_trigger", [this](TestInfo& ti) { return PciePmuIntrTrigger(ti); });
    _add_test("pcie_pmu_intr_is_set", [this](TestInfo& ti) { return PciePmuIntrIsSet(ti); });
    _add_test("pcie_isi_intr_is_set", [this](TestInfo& ti) { return PcieIsiIntrIsSet(ti); });
    _add_test("pcie_link_speed_change", [this](TestInfo& ti) { return PcieLinkSpeedChange(ti); });
    */
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

    ti.logger->info("pcie_link_status_check current speed is " + current_speed +
                    ", current width is " + current_width);

    TestMetrics metrics = {
        {"current_speed", current_speed},
        {"current_width", current_width},
    };

    ti.logger->trace("Complete pcie_link_status_check");
    return {"pcie_link_status_check", get_name(), true, metrics};
}

// pcie_dma_data_transfer : To run PCIe DMA data movement and compare the transferred pattern.
// @input: "direction": h2d/d2h/both, "size_bytes", "pattern"
// @output: TestResult metrics include direction, size_bytes, pattern, data_compare.
TestResult PCIeModule::PcieDmaDataTransfer(TestInfo& ti)
{
    auto direction = common::args::get_string(ti.args, "direction", "h2d");
    auto size_bytes = common::args::get_string(ti.args, "size_bytes", "128");
    auto pattern = common::args::get_string(ti.args, "pattern", "incremental");

    // alloc_host_mem(2 * 1024 * 1024)
    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    std::vector<uint8_t> actual(static_cast<size_t>(size_bytes), 0);

    if(direction == "d2h"){
        /*
        auto write_ok = common::devmem::write(dev_mem_cpu_base,
                                              dev_mem_size,
                                              dev_mem_offset,
                                              expected.data(),
                                              expected.size());
        dma_copy_d2h(dev_mem_dma_addr, dst_host=actual.data(), size_bytes);
        */
    }
    else if(direction == "h2d"){
        /*
        dma_copy_h2d(expected.data(), dev_mem_dma_addr, size_bytes);
        auto read_ok = common::devmem::read(dev_mem_cpu_base,
                                            dev_mem_size,
                                            dev_mem_offset,
                                            actual.data(),
                                            actual.size());
        */
    }

    auto compare_ok = common::pattern::compare(expected, actual);

    return {"pcie_dma_data_transfer", get_name(), compare_ok, {
        {"direction", direction},
        {"size_bytes", size_bytes},
        {"pattern", pattern},
        {"compare_status", compare_ok ? "matched" : "mismatch"}
    }};
}

#if 0
// Reference implementation sketch for pcie_dma_data_transfer.
//
// Python/CLI should only pass test intent:
//   direction: h2d, d2h, or both
//   size_bytes: transfer size
//   pattern: zero, incremental, or random
//
// C++/HAL should own all low-level resources:
//   host buffers
//   device scratch memory
//   DMA descriptor programming
//   completion wait
//   readback and compare
TestResult PCIeModule::PcieDmaDataTransfer(TestInfo& ti)
{
    auto direction = common::args::get_string(ti.args, "direction", "h2d");
    auto size_bytes = common::args::get_u64(ti.args, "size_bytes", 4096);
    auto pattern = common::args::get_string(ti.args, "pattern", "incremental");

    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    std::vector<uint8_t> actual(static_cast<size_t>(size_bytes), 0);
    if (expected.empty()) {
        return {"pcie_dma_data_transfer", get_name(), false, {
            {"direction", direction},
            {"size_bytes", std::to_string(size_bytes)},
            {"pattern", pattern},
            {"compare_status", "invalid_pattern_or_size"}
        }};
    }

    // Yes, this test needs device memory unless the platform already provides
    // a reserved DMA scratch window. Python should not provide this address.
    //
    // Missing HAL resource:
    //   auto dev_mem = hal_or_umd_alloc_dma_scratch(size_bytes);
    //   dev_mem.cpu_base: CPU-visible mapped pointer for devmem read/write
    //   dev_mem.device_addr: DMA-visible device/bus/IOVA address
    //   dev_mem.size: allocation size
    //
    // Temporary placeholders until a real HAL/UMD allocator exists:
    void* dev_mem_cpu_base = nullptr;
    uint64_t dev_mem_size = size_bytes;
    uint64_t dev_mem_offset = 0;
    uint64_t dev_mem_dma_addr = 0;

    auto run_h2d = [&]() -> bool {
        std::fill(actual.begin(), actual.end(), 0);

        // Missing DMA operation:
        //   dma_copy_h2d(src_host=expected.data(),
        //                dst_device_addr=dev_mem_dma_addr,
        //                len=size_bytes);
        //   wait_dma_complete();
        (void)dev_mem_dma_addr;

        // Read device memory back into host actual buffer.
        auto read_ok = common::devmem::read(dev_mem_cpu_base,
                                            dev_mem_size,
                                            dev_mem_offset,
                                            actual.data(),
                                            actual.size());
        return read_ok && common::pattern::compare(expected, actual);
    };

    auto run_d2h = [&]() -> bool {
        std::fill(actual.begin(), actual.end(), 0);

        // Seed device memory with the expected data before running D2H DMA.
        // In real hardware this could also be a device-side fill command.
        auto write_ok = common::devmem::write(dev_mem_cpu_base,
                                             dev_mem_size,
                                             dev_mem_offset,
                                             expected.data(),
                                             expected.size());
        if (!write_ok) {
            return false;
        }

        // Missing DMA operation:
        //   dma_copy_d2h(src_device_addr=dev_mem_dma_addr,
        //                dst_host=actual.data(),
        //                len=size_bytes);
        //   wait_dma_complete();

        return common::pattern::compare(expected, actual);
    };

    bool compare_ok = false;
    if (direction == "h2d") {
        compare_ok = run_h2d();
    } else if (direction == "d2h") {
        compare_ok = run_d2h();
    } else if (direction == "both") {
        compare_ok = run_h2d() && run_d2h();
    } else {
        return {"pcie_dma_data_transfer", get_name(), false, {
            {"direction", direction},
            {"size_bytes", std::to_string(size_bytes)},
            {"pattern", pattern},
            {"compare_status", "invalid_direction"}
        }};
    }

    // Missing HAL cleanup:
    //   hal_or_umd_free_dma_scratch(dev_mem);

    return {"pcie_dma_data_transfer", get_name(), compare_ok, {
        {"direction", direction},
        {"size_bytes", std::to_string(size_bytes)},
        {"pattern", pattern},
        {"device_memory_offset", std::to_string(dev_mem_offset)},
        {"compare_status", compare_ok ? "matched" : "mismatch"}
    }};
}
#endif
