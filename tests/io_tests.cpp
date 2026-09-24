#include "diag/core/Common.h"
#include "diag/core/HalContext.h"
#include "diag/device/DeviceManager.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

extern "C" {
#include <phal/components/isi/isi.h>
#include <phal/components/pcie/pcie.h>
}

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

class ProbeTarget : public TestTarget {
public:
    ProbeTarget(const std::string& name, const DeviceContext& ctx, TestcaseFunc fn)
        : TestTarget(name, "probe", ctx)
    {
        _add_test("probe", {}, std::move(fn));
    }
};

void register_views()
{
    HalContext hal;
    auto device = hal.mmap_bar_space({});
    device.bdf = "reg_test";
    device.tpu_type = TPUType::Atlas;
    device.pcie_control_base = 0x800;
    PMUModule pmu("PMU_0_0", device, {0, 0, 0x100, 0x40});
    DDPModule ddp0("DDP_0_0", device, {0, 0, 0x200, 0x40});
    DDPModule ddp1("DDP_0_1", device, {1, 0, 0x300, 0x40});
    ISIModule isi0("ISI_0_0", device, {0, 0, 0x400, 0x40});
    ISIModule isi1("ISI_0_1", device, {1, 0, 0x500, 0x40});
    std::vector<TestTarget*> targets{&pmu, &ddp0, &ddp1, &isi0, &isi1};
    uint32_t expected = 10;
    for (auto* target : targets) {
        auto& ctx = target->get_context();
        CHECK(common::bar::write32(device, 2, ctx.reg_base_offset + 0x14, 0x22222222));
        CHECK(common::bar::write32(device, 4, ctx.reg_base_offset + 0x14, 0x44444444));
        CHECK(common::bar::write32(device, 0, ctx.reg_base_offset + 0x14, expected));
        auto* phal = hal.phal().get_context(ctx, ctx.reg_base_offset, PhalProject::Atlas);
        CHECK(phal != nullptr);
        phal_block_ctx_enter(phal, 0x10);
        phal_block_ctx_enter(phal, 4);
        uint32_t value = 0;
        CHECK(phal_read(phal, 0, &value) == PHAL_STATUS_OK && value == expected);
        CHECK(phal_write(phal, 0, expected + 100) == PHAL_STATUS_OK);
        CHECK(common::bar::read32(device, 0, ctx.reg_base_offset + 0x14, value));
        CHECK(value == expected + 100);
        CHECK(common::bar::read32(device, 2, ctx.reg_base_offset + 0x14, value) && value == 0x22222222);
        CHECK(common::bar::read32(device, 4, ctx.reg_base_offset + 0x14, value) && value == 0x44444444);
        phal_block_ctx_exit(phal, 4);
        phal_block_ctx_exit(phal, 0x10);
        CHECK(phal->env.base == ctx.reg_base_offset);
        ++expected;
    }
    auto& ctx = ddp0.get_context();
    auto* phal = hal.phal().get_context(ctx, ctx.reg_base_offset, PhalProject::Atlas);
    uint32_t untouched = 0xabcdef;
    CHECK(phal_read(nullptr, 0, &untouched) == PHAL_STATUS_INVALID);
    CHECK(phal_read(phal, 0, nullptr) == PHAL_STATUS_INVALID);
    CHECK(phal_read(phal, 0x10000, &untouched) == PHAL_STATUS_INVALID);
    CHECK(untouched == 0xabcdef);
    CHECK(phal_write(phal, 0x10000, 77) == PHAL_STATUS_INVALID);
}

void memory_and_phal()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "memory_test";
    ctx.module_type = "ddp";
    ctx.tpu_type = TPUType::Atlas;
    ctx.reg_base_offset = 0x400;
    ctx.reg_size = 0x100;
    ctx.pcie_control_base = 0x800;
    ctx.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    const auto* dmem = find_df_region(ctx.memory_regions.df, MemoryRegion::DMEM);
    CHECK(dmem != nullptr && dmem->supported);
    ctx.phal = hal.phal().get_context(ctx, ctx.reg_base_offset, PhalProject::Atlas);
    ctx.pcie_phal = hal.phal().get_context(ctx, ctx.pcie_control_base, PhalProject::Atlas);
    auto* control = ctx.pcie_phal;
    auto* module = ctx.phal;
    CHECK(module != control && module->env.base == 0x400 && control->env.base == 0x800);
    phal_block_ctx_enter(module, 0x20);
    CHECK(mem_write(ctx, MemoryRegion::DMEM, 4, 0x12345678) == PHAL_STATUS_OK);
    uint32_t value = 0;
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 4, &value) == PHAL_STATUS_OK && value == 0x12345678);
    CHECK(module->env.base == 0x420 && control->env.base == 0x800);
    CHECK(control->apertures[4][0].target_addr == dmem->base);
    CHECK(common::bar::read32(ctx, 4, 4, value) && value == 0x12345678);
    CHECK(common::bar::read32(ctx, 4, 0x24, value) && value == 0);
    phal_block_ctx_exit(module, 0x20);
    CHECK(module->env.base == 0x400);

    // Exercise the real bridge callback, including PHAL's own base addition.
    CHECK(common::bar::write32(ctx, 2, 0x408, 0x22222222));
    CHECK(common::bar::write32(ctx, 4, 0x408, 0x44444444));
    CHECK(phal_write(module, 8, 0x42) == PHAL_STATUS_OK);
    CHECK(common::bar::read32(ctx, 0, 0x408, value) && value == 0x42);
    CHECK(phal_read(module, 8, &value) == PHAL_STATUS_OK && value == 0x42);
    CHECK(common::bar::read32(ctx, 2, 0x408, value) && value == 0x22222222);
    CHECK(common::bar::read32(ctx, 4, 0x408, value) && value == 0x44444444);

    // Direct RCF regions use the current module's BAR0 base and do not touch
    // the DMEM aperture configuration.
    std::vector<uint8_t> rcf_memory(0x00c21000, 0);
    auto& bar0 = ctx.bar_mappings[0];
    CHECK(bar0.bar_index == 0);
    bar0.mapped_base = rcf_memory.data();
    bar0.mapped_size = bar0.size = rcf_memory.size();
    const auto aperture_target = control->apertures[4][0].target_addr;
    CHECK(mem_write(ctx, MemoryRegion::DDP_ILM, 4, 0xaabbccdd) == PHAL_STATUS_OK);
    CHECK(mem_read(ctx, MemoryRegion::DDP_ILM, 4, &value) == PHAL_STATUS_OK &&
          value == 0xaabbccdd);
    CHECK(common::bar::read32(ctx, 0, 0x400 + 0x00c00800 + 4, value) &&
          value == 0xaabbccdd);
    CHECK(control->apertures[4][0].target_addr == aperture_target);
    CHECK(mem_read(ctx, MemoryRegion::DDP_ILM, 2, &value) == PHAL_STATUS_INVALID);
    CHECK(mem_read(ctx, MemoryRegion::DDP_ILM, 0x20000, &value) == PHAL_STATUS_INVALID);
    bar0.writable = false;
    CHECK(mem_write(ctx, MemoryRegion::DDP_ILM, 8, 1) == PHAL_STATUS_ERROR);
    bar0.writable = true;
    CHECK(mem_read(ctx, MemoryRegion::AIC_SMEM, 4, &value) == PHAL_STATUS_UNIMPLEMENTED);
    auto atlas_regions = ctx.memory_regions;
    ctx.memory_regions = {};
    CHECK(mem_read(ctx, MemoryRegion::DDP_ILM, 4, &value) == PHAL_STATUS_UNIMPLEMENTED);
    ctx.memory_regions = atlas_regions;

    TestInfo ti;
    ti.hal = &hal;
    common::devmem::Window window{
        4, 0, 0, dmem->base, dmem->aperture_size, 0};
    uint32_t buffer[]{1, 2, 3, 4};
    CHECK(common::devmem::write(ti, ctx, window, 0x40, buffer, sizeof(buffer)));
    uint32_t copy[4]{};
    CHECK(common::devmem::read(ti, ctx, window, 0x40, copy, sizeof(copy)));
    CHECK(std::equal(std::begin(buffer), std::end(buffer), std::begin(copy)));
    window.aperture_index = 255; // Mock rejects set; no payload write may occur.
    CHECK(!common::devmem::write(ti, ctx, window, 4, buffer, sizeof(uint32_t)));
    CHECK(common::bar::read32(ctx, 4, 4, value) && value == 0x12345678);
    ctx.pcie_phal = nullptr;
    value = 99;
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 4, &value) == PHAL_STATUS_ERROR && value == 99);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 2, &value) == PHAL_STATUS_INVALID);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 0, nullptr) == PHAL_STATUS_INVALID);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, dmem->aperture_size, &value) == PHAL_STATUS_INVALID);
    ctx.pcie_phal = control;
    const DirectRcfRegionMap saved_rcf_regions = ctx.memory_regions.rcf;
    const DfRegion invalid_dmem[] = {{
        MemoryRegion::DMEM, "DMEM", std::numeric_limits<uint64_t>::max() - 3,
        0x10000000, 0x10000000, true}};
    ctx.memory_regions = {saved_rcf_regions, {invalid_dmem, 1}};
    CHECK(mem_write(ctx, MemoryRegion::DMEM, 0, 5) == PHAL_STATUS_INVALID);
    CHECK(phal_component_isi_linkup(module, 100) != PHAL_STATUS_OK);
    TestStatus combined = phal_read(nullptr, 0, &value);
    combined |= PHAL_STATUS_TIMEOUT;
    CHECK(combined == (PHAL_STATUS_INVALID | PHAL_STATUS_TIMEOUT));
    CHECK(test_status_name(PHAL_STATUS_TIMEOUT | PHAL_STATUS_ERROR) == "TIMEOUT|ERROR");
}

void phal_logging()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "phal_log_test";
    ctx.tpu_type = TPUType::Atlas;
    phal_ctx_t* phal = nullptr;
    {
        Logger logger;
        logger.set_level(LogLevel::Debug);
        ctx.logger = &logger;
        phal = hal.phal().get_context(ctx, 0x800, PhalProject::Atlas);
    }
    ctx.logger = nullptr; // The cached logger must not borrow the destroyed object.
    std::ostringstream output;
    struct RestoreOutput {
        std::streambuf* previous;
        ~RestoreOutput() { std::cout.rdbuf(previous); }
    } restore{std::cout.rdbuf(output.rdbuf())};
    CHECK(phal != nullptr);
    phal_block_ctx_enter(phal, 0x40);
    CHECK(phal_write(phal, 8, 0xffffffff) == PHAL_STATUS_OK);
    uint32_t value = 0;
    CHECK(phal_read(phal, 8, &value) == PHAL_STATUS_OK && value == 0xffffffff);
    CHECK(output.str().find("[DEBUG] phal_write bdf=phal_log_test") != std::string::npos);
    CHECK(output.str().find("[DEBUG] phal_read bdf=phal_log_test") != std::string::npos);
    CHECK(output.str().find("env_base=0x840 offset=0x8 bar0_offset=0x848 value=0xffffffff") != std::string::npos);
    phal_block_ctx_exit(phal, 0x40);
    CHECK(hal.phal().get_context(ctx, 0x800, PhalProject::Atlas) == phal);
    output.str("");
    CHECK(phal_read(phal, 8, &value) == PHAL_STATUS_OK);
    CHECK(output.str().empty()); // Cache reuse refreshes the log level to info.
    CHECK(phal_read(phal, 0x10000, &value) != PHAL_STATUS_OK);
    CHECK(output.str().find("[ERROR] phal_read") != std::string::npos);
}

class DmaProbeImpl : public PCIeImpl {
public:
    std::function<TestStatus(const DmaTransferRequest&)> copy;
    TestStatus bar_read32(TestInfo&) override { return PHAL_STATUS_UNIMPLEMENTED; }
    TestStatus bar_scan32(TestInfo&) override { return PHAL_STATUS_UNIMPLEMENTED; }
    TestStatus dma_copy(TestInfo&, const DmaTransferRequest& req) override { return copy(req); }
};

void buffer_and_dma()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "buffer_test";
    ctx.tpu_type = TPUType::Atlas;
    ctx.pcie_control_base = 0x800;
    ctx.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    const auto* dmem = find_df_region(ctx.memory_regions.df, MemoryRegion::DMEM);
    CHECK(dmem != nullptr && dmem->supported);
    std::vector<uint8_t> memory(0x200000, 0x5a);
    auto& bar = ctx.bar_mappings[2];
    CHECK(bar.bar_index == 4);
    bar.mapped_base = memory.data();
    bar.mapped_size = bar.size = memory.size();
    ctx.pcie_phal = hal.phal().get_context(ctx, 0x800, PhalProject::Atlas);
    CHECK(dmem->base == 0x10000000 && dmem->aperture_size == 0x10000000);
    ctx.phal = hal.phal().get_context(ctx, ctx.reg_base_offset, PhalProject::Atlas);
    phal_block_ctx_enter(ctx.phal, 0x800); // DMEM must use the PCIe root, not this block.
    const std::vector<uint8_t> data{1, 2, 3, 4, 5, 6, 7};
    std::vector<uint8_t> actual(data.size()), empty;
    // Beyond the old 1 MiB limit; byte vectors allow unaligned offsets.
    CHECK(mem_write(ctx, MemoryRegion::DMEM, 0x100001, data) == PHAL_STATUS_OK);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 0x100001, &actual) == PHAL_STATUS_OK && actual == data);
    CHECK(memory[0x100000] == 0x5a && memory[0x100008] == 0x5a);
    CHECK(ctx.pcie_phal->apertures[4][0].size == 0x10000000);
    CHECK(ctx.pcie_phal->env.base == 0x800);
    phal_block_ctx_exit(ctx.phal, 0x800);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 0, &empty) == PHAL_STATUS_INVALID);
    CHECK(mem_write(ctx, MemoryRegion::DMEM, 0, empty) == PHAL_STATUS_INVALID);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 0, static_cast<std::vector<uint8_t>*>(nullptr)) == PHAL_STATUS_INVALID);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, dmem->aperture_size - 4, &actual) == PHAL_STATUS_INVALID && actual == data);
    CHECK(mem_read(ctx, MemoryRegion::DMEM, memory.size() - 4, &actual) == PHAL_STATUS_ERROR && actual == data);
    CHECK(mem_write(ctx, MemoryRegion::DMEM, memory.size() - 4, data) == PHAL_STATUS_ERROR);
    bar.writable = false;
    CHECK(mem_write(ctx, MemoryRegion::DMEM, 0, data) == PHAL_STATUS_ERROR && memory[0] == 0x5a);
    bar.writable = true;
    ctx.pcie_phal = nullptr;
    CHECK(mem_read(ctx, MemoryRegion::DMEM, 0, &actual) == PHAL_STATUS_ERROR && actual == data);

    // Case sequencing only: a test double copies bytes, not HQC or firmware.
    auto impl = std::make_unique<DmaProbeImpl>();
    std::vector<DmaTransferRequest> requests;
    std::vector<uint8_t> intermediate(256);
    size_t fail_on_call = 0;
    bool corrupt_copy = false;
    impl->copy = [&](const DmaTransferRequest& req) {
        requests.push_back(req);
        CHECK(req.host_buffer == nullptr && req.size_bytes == 128);
        if (requests.size() == fail_on_call) return PHAL_STATUS_TIMEOUT;
        const bool forward = req.type == DmaTransferType::D2I || req.type == DmaTransferType::D2S;
        if (!forward && corrupt_copy) intermediate[req.src_offset] ^= 1;
        const bool ok = forward
            ? common::bar::read(ctx, 4, req.src_offset, intermediate.data() + req.dst_offset, req.size_bytes)
            : common::bar::write(ctx, 4, req.dst_offset, intermediate.data() + req.src_offset, req.size_bytes);
        return ok ? PHAL_STATUS_OK : PHAL_STATUS_ERROR;
    };
    PCIeModule pcie("PCIE", ctx, {0, 0, 0x800, 0x100}, std::move(impl));
    Logger logger;
    logger.set_level(LogLevel::Error);
    TestArgs args{{"direction", "d2i2d"}, {"size_bytes", "128"}, {"pattern", "zero"}};
    CHECK(pcie.run_testcase("pcie_dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(requests.size() == 2 && requests[0].type == DmaTransferType::D2I && requests[1].type == DmaTransferType::I2D);
    CHECK(requests[0].src_offset == 0 && requests[1].dst_offset == 128);
    args["direction"] = "d2s2d";
    requests.clear();
    CHECK(pcie.run_testcase("pcie_dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(requests.size() == 2 && requests[0].type == DmaTransferType::D2S && requests[1].type == DmaTransferType::S2D);
    for (size_t fail_at : {1u, 2u}) {
        requests.clear();
        fail_on_call = fail_at;
        CHECK(pcie.run_testcase("pcie_dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_TIMEOUT);
        CHECK(requests.size() == fail_at);
    }
    requests.clear();
    fail_on_call = 0;
    corrupt_copy = true;
    CHECK(pcie.run_testcase("pcie_dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(requests.size() == 2); // Final verification catches corrupted DMA data.
    requests.clear();
    pcie.get_context().bar_mappings[2].writable = false;
    CHECK(pcie.run_testcase("pcie_dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(requests.empty()); // Never submit DMA if preparation failed.
}

void examples_and_lifecycle()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    std::vector<uint8_t> bar0(0x2107000);
    ctx.bar_mappings[0].mapped_base = bar0.data();
    ctx.bar_mappings[0].mapped_size = ctx.bar_mappings[0].size = bar0.size();
    ctx.bdf = "example_test";
    ctx.tpu_type = TPUType::Atlas;
    ctx.pcie_control_base = 0x800;
    ctx.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    Logger logger;
    logger.set_level(LogLevel::Error);
    TPUDeviceConfig config;
    config.pcie_modules = {{0, 0, 0x800, 0x100}};
    config.ddp_modules = {{0, 0, 0x1000, 0x100}, {1, 0, 0x90000, 0x100}};
    config.pmu_modules = {{0, 0, 0x100, 0x2108030}};
    TPUDevice tpu("TPU0", ctx, config, nullptr);
    auto& pmu = *tpu.pmu();
    DDPModule ddp("DDP", ctx, {0, 0, 0x1000, 0x90000});
    auto& ddp1 = *tpu.ddp(1);
    ddp.set_device(&tpu);
    PCIeModule pcie("PCIE", ctx, {0, 0, 0x800, 0x10231c}, nullptr);
    ISIModule isi("ISI", ctx, {0, 0, 0x300, 0x40});
    CHECK(pmu.run_testcase("pmu_example", {}, &logger) == PHAL_STATUS_ERROR);
    CHECK(pmu.run_testcase("pmu_example", {{"block_offset", "0x10"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(pmu.run_testcase("pmu_example", {{"write_enable", "1"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(ddp.run_testcase("ddp_example", {{"block_offset", "0"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(isi.run_testcase("isi_example", {{"reg_offset", "0"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(common::bar::write32(ctx, 0, 0x2100900, 0x00141e01));
    CHECK(common::bar::write32(ctx, 0, 0x81800, 0xabcd16c3));
    CHECK(common::bar::write32(ctx, 0, 0x110800, 0xabcd16c3));
    CHECK(common::bar::write32(ctx, 0, 0x100800, 0xabcd16c3));
    CHECK(common::bar::write32(ctx, 0, 0x31c, 0x202020));
    CHECK(common::bar::write32(ctx, 0, 0xd00800, 0x55aa55aa));
    CHECK(common::bar::write32(ctx, 4, 0, 0xccdd));
    // PMU requires an unlock write; the other register examples are read-only.
    pmu.get_context().bar_mappings[0].writable = true;
    ddp.get_context().bar_mappings[0].writable = false;
    ddp1.get_context().bar_mappings[0].writable = false;
    isi.get_context().bar_mappings[0].writable = false;
    pcie.get_context().bar_mappings[0].writable = false;
    std::ostringstream raw_read_output;
    logger.set_level(LogLevel::Info);
    auto* previous_output = std::cout.rdbuf(raw_read_output.rdbuf());
    const auto raw_read_status = pcie.run_testcase(
        "pcie_bar_read32_abs", {{"offset", "0x110800"}}, &logger, &hal);
    std::cout.rdbuf(previous_output);
    logger.set_level(LogLevel::Error);
    CHECK(raw_read_status == PHAL_STATUS_OK);
    CHECK(pcie.get_context().phal != nullptr && pcie.get_context().pcie_phal != nullptr);
    CHECK(raw_read_output.str().find("bar0_offset=0x110800 value=0xabcd16c3") != std::string::npos);
    CHECK(pcie.run_testcase("pcie_bar_read32_abs", {}, &logger) == PHAL_STATUS_INVALID);
    CHECK(pcie.run_testcase("pcie_bar_read32_abs", {{"offset", "0x110801"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(pcie.run_testcase("pcie_bar_read32_abs", {{"offset", "0x2107000"}}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(pmu.run_testcase("pmu_example", {}, &logger, &hal) == PHAL_STATUS_OK);
    uint32_t pmu_unlock = 0;
    CHECK(common::bar::read32(ctx, 0, 0x100, pmu_unlock));
    CHECK(pmu_unlock == 0x5a5a5a5a);
    CHECK(pmu.get_context().phal->env.base == pmu.get_context().reg_base_offset);
    CHECK(ddp.run_testcase("ddp_example", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(common::bar::write32(ctx, 0, 0x2100900, 0));
    CHECK(pmu.run_testcase("pmu_example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    pcie.get_context().bar_mappings[0].writable = true;
    CHECK(pcie.run_testcase("pcie_example", {}, &logger, &hal) == PHAL_STATUS_OK);
    uint32_t hqc_sram_value = 0;
    CHECK(common::bar::read32(ctx, 0, 0xd00800, hqc_sram_value));
    CHECK(hqc_sram_value == 0x23232323); // PCIe restored the word written by PMU.
    pcie.get_context().bar_mappings[2].writable = false;
    CHECK(pcie.run_testcase("pcie_example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    pcie.get_context().bar_mappings[2].writable = true;
    CHECK(common::bar::write32(ctx, 0, 0x100800, 0xdeadbeef));
    CHECK(pcie.run_testcase("pcie_example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(common::bar::write32(ctx, 0, 0x110800, 0xdeadbeef));
    CHECK(ddp.run_testcase("ddp_example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(common::bar::write32(ctx, 0, 0x110800, 0xabcd16c3));
    CHECK(ddp.run_testcase("ddp_example", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(ddp.get_context().phal->env.base == ddp.get_context().reg_base_offset);
    CHECK(ddp1.get_context().phal->env.base == ddp1.get_context().reg_base_offset);
    CHECK(isi.run_testcase("isi_example", {}, &logger, &hal) == PHAL_STATUS_ERROR); // native mock is honest
    uint32_t value = 0;
    CHECK(common::bar::read32(ctx, 4, 0, value) && value == 0xccdd);
    CHECK(pmu.get_context().logger == nullptr);

    auto* soc = tpu.module("soc", 0);
    CHECK(soc != nullptr);
    ddp1.get_context().bar_mappings[0].writable = true;
    CHECK(soc->run_testcase("soc_example", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(soc->get_context().phal == nullptr && soc->get_context().pcie_phal == nullptr);
    for (auto* module : tpu.child_targets()) CHECK(module->get_context().logger == nullptr);

    ProbeTarget* self = nullptr;
    ProbeTarget throwing("throw", ctx, [&](TestInfo&) -> TestStatus {
        phal_block_ctx_enter(self->get_context().phal, 0x20);
        throw std::runtime_error("injected failure");
    });
    self = &throwing;
    CHECK(throwing.run_testcase("probe_probe", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(throwing.get_context().phal->env.base == throwing.get_context().reg_base_offset);
    CHECK(throwing.get_context().logger == nullptr);

    std::atomic<int> active{0};
    std::atomic<bool> overlap{false};
    const auto work = [&](TestInfo&) {
        if (active.fetch_add(1) != 0) overlap = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        --active;
        return PHAL_STATUS_OK;
    };
    ProbeTarget first("first", ctx, work), second("second", ctx, work);
    std::thread a([&] { first.run_testcase("probe_probe", {}, &logger, &hal); });
    std::thread b([&] { second.run_testcase("probe_probe", {}, &logger, &hal); });
    a.join(); b.join();
    CHECK(!overlap);
}

void aperture_mapping()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "aperture_test";
    ctx.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    ctx.tpu_type = TPUType::Atlas;
    ctx.pcie_control_base = 0x800;
    // Keep the real case's BAR offsets. These buffers do not model translation.
    std::vector<uint8_t> bar2(0x10800000, 0x5a), bar4(0x800000, 0x5a);
    for (auto& bar : ctx.bar_mappings) {
        if (bar.bar_index == 2) {
            bar.mapped_base = bar2.data();
            bar.mapped_size = bar.size = 0x10700000;
        } else if (bar.bar_index == 4) {
            bar.mapped_base = bar4.data();
            bar.mapped_size = bar.size = bar4.size();
        }
    }
    Logger logger;
    logger.set_level(LogLevel::Error);
    PCIeModule pcie("PCIE", ctx, {0, 0, 0x800, 0x100}, nullptr);
    CHECK(pcie.run_testcase("pcie_sequential_aperture_mapping", {}, &logger) == PHAL_STATUS_ERROR);
    CHECK(pcie.run_testcase("pcie_sequential_aperture_mapping", {}, &logger, &hal) == PHAL_STATUS_OK);
    auto* phal = pcie.get_context().pcie_phal;
    CHECK(phal != nullptr && phal->env.base == 0x800);
    CHECK(phal->apertures[2][0].size == 0); // reserved aperture remains untouched
    CHECK(phal->apertures[2][1].target_addr == 0x10000000);
    CHECK(phal->apertures[4][7].target_addr == 0x10e00000);
    const auto restored = [&] {
        return std::all_of(bar2.begin(), bar2.end(), [](uint8_t b) { return b == 0x5a; }) &&
               std::all_of(bar4.begin(), bar4.end(), [](uint8_t b) { return b == 0x5a; });
    };
    CHECK(restored());

    auto& data_bar = pcie.get_context().bar_mappings[2];
    CHECK(data_bar.bar_index == 4);
    // Alias two BAR windows: per-window write/read would miss this interference.
    data_bar.mapped_base = bar2.data() + 0x10000000;
    CHECK(pcie.run_testcase("pcie_sequential_aperture_mapping", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(restored());

    // Fail the first BAR4 write after all BAR2 writes; BAR2 must still be restored.
    data_bar.mapped_base = bar4.data();
    data_bar.writable = false;
    CHECK(pcie.run_testcase("pcie_sequential_aperture_mapping", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(restored());
}

void topology_and_pcie_catalog()
{
    DeviceManager manager;
    const auto tree = manager.discover();
    CHECK(tree.devices.size() == 16); // parent + SoC + PCIe + PMU + 4 DDP + 8 ISI
    for (const auto& item : tree.devices) CHECK(item.type != "dmc");
    CHECK(manager.get_target_names() == std::vector<std::string>{"TPU0"});
    CHECK(manager.get_target("TPU0")->get_registered_test_names().size() == 10);
    CHECK(manager.run_testcase("DDP_0_0", "ddp_example") == PHAL_STATUS_INVALID);
    CHECK(manager.run_testcase("TPU9", "ddp_example") == PHAL_STATUS_INVALID);
    CHECK(manager.run_testcase("TPU0", "example") == PHAL_STATUS_INVALID);
    CHECK(manager.run_testcase("TPU0", "pcie_bar_read32_abs", {{"offset", "4"}}) == PHAL_STATUS_OK);
    CHECK(manager.run_testcase("TPU0", "pcie_dma_data_transfer", {{"size_bytes", "16"}}) == PHAL_STATUS_INVALID);
    CHECK(manager.get_target("DDP_0_3")->get_context().reg_base_offset == 0x35000000);
    CHECK(manager.get_target("ISI_0_7")->get_context().reg_base_offset == 0x37e00000);
    CHECK(manager.get_target("PMU_0_0")->get_context().reg_base_offset == 0x38000000);
    for (const auto& name : {"PMU_0_0", "DDP_0_0", "ISI_0_0"}) {
        CHECK(manager.get_target(name)->get_registered_test_names() == std::vector<std::string>{manager.get_target(name)->get_target_type() + "_example"});
    }
    auto names = manager.get_target("PCIE_0_0")->get_registered_test_names();
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"pcie_bar_read32", "pcie_bar_read32_abs", "pcie_bar_scan32", "pcie_dma_data_transfer", "pcie_example", "pcie_sequential_aperture_mapping"}));
}

void module_lookup()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "module_lookup";
    ctx.tpu_type = TPUType::Atlas;
    ctx.pcie_control_base = 0x800;
    TPUDeviceConfig config;
    config.pcie_modules = {{0, 0, 0x800, 0x100}};
    config.pmu_modules = {{0, 0, 0x100, 0x100}};
    // Policy IDs intentionally differ from vector positions.
    config.ddp_modules = {{2, 0, 0x400, 0x100}, {0, 0, 0x200, 0x100}};
    TPUDevice device("TPU0", ctx, config, nullptr);
    CHECK(device.ddp(0)->get_context().reg_base_offset == 0x200);
    CHECK(device.ddp(2)->get_context().reg_base_offset == 0x400);
    CHECK(device.ddp(1) == nullptr);
    CHECK(device.module("soc", 0)->get_context().phal == nullptr);
    Logger logger;
    logger.set_level(LogLevel::Error);
    ProbeTarget test("cross_module", ctx, [&](TestInfo& ti) {
        CHECK(ti.target_name == "TPU0");
        auto& first = ti.module("ddp", 0);
        auto& second = ti.module("ddp", 2);
        auto& pmu = ti.module("pmu", 0);
        auto& pcie = ti.module("pcie", 0);
        CHECK(first.logger == ti.logger && second.logger == ti.logger);
        CHECK(pcie.phal == first.pcie_phal);
        CHECK(phal_write(first.phal, 0, 0x11) == PHAL_STATUS_OK);
        phal_block_ctx_enter(second.phal, 0x10);
        CHECK(&ti.module("ddp", 2) == &second);
        CHECK(second.phal->env.base == 0x410); // Lookup must preserve block scope.
        CHECK(phal_write(second.phal, 0, 0x22) == PHAL_STATUS_OK);
        CHECK(phal_write(pmu.phal, 0, 0x33) == PHAL_STATUS_OK);
        throw std::runtime_error("exercise sibling cleanup");
        return PHAL_STATUS_OK;
    });
    test.set_device(&device);
    CHECK(test.run_testcase("probe_probe", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    uint32_t value = 0;
    CHECK(common::bar::read32(ctx, 0, 0x200, value) && value == 0x11);
    CHECK(common::bar::read32(ctx, 0, 0x410, value) && value == 0x22);
    CHECK(common::bar::read32(ctx, 0, 0x100, value) && value == 0x33);
    for (auto* child : device.child_targets()) {
        const auto& context = child->get_context();
        CHECK(context.logger == nullptr);
        if (context.phal) CHECK(context.phal->env.base == context.reg_base_offset);
    }
    ProbeTarget missing("missing", ctx, [&](TestInfo& ti) {
        ti.module("ddp", 1);
        return PHAL_STATUS_OK;
    });
    missing.set_device(&device);
    CHECK(missing.run_testcase("probe_probe", {}, &logger, &hal) == PHAL_STATUS_INVALID);

    auto other_ctx = hal.mmap_bar_space({});
    other_ctx.bdf = "other_tpu";
    other_ctx.tpu_type = TPUType::Atlas;
    other_ctx.pcie_control_base = 0x800;
    TPUDevice other("TPU1", other_ctx, config, nullptr);
    ProbeTarget other_test("other", other_ctx, [&](TestInfo& ti) {
        auto& ddp = ti.module("ddp", 0);
        CHECK(&ddp == &other.ddp(0)->get_context());
        return phal_write(ddp.phal, 0, 0x44);
    });
    other_test.set_device(&other);
    CHECK(other_test.run_testcase("probe_probe", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(common::bar::read32(other_ctx, 0, 0x200, value) && value == 0x44);
    CHECK(common::bar::read32(ctx, 0, 0x200, value) && value == 0x11);
}

void device_allocations()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "allocator";
    ctx.tpu_type = TPUType::Atlas;
    ctx.module_type = "pcie";
    ctx.reg_base_offset = ctx.pcie_control_base = 0x800;
    ctx.phal = ctx.pcie_phal = hal.phal().get_context(ctx, 0x800, PhalProject::Atlas);
    const DfRegion df[] = {{DMEM, "DMEM", 0x10000000, 64, 0x10000000, true}};
    const DirectRcfRegion rcf[] = {
        {HQC_SRAM_0, "HQC_SRAM_0", "pcie", 0x100, 64, true},
        {DDP_ILM, "DDP_ILM", "ddp", 0x200, 64, true}};
    ctx.memory_regions = {{rcf, 2}, {df, 1}};
    CHECK(!mem_alloc(ctx, DMEM, 0).valid());
    CHECK(!mem_alloc(ctx, DMEM, 3).valid());
    CHECK(!mem_alloc(ctx, DMEM, 68).valid());
    CHECK(!mem_alloc(ctx, AIC_SMEM, 4).valid());
    CHECK(!mem_alloc(ctx, DDP_ILM, 4).valid());
    {
        auto a = mem_alloc(ctx, DMEM, 16);
        auto pmu = ctx;
        pmu.module_type = "pmu";
        auto b = mem_alloc(pmu, DMEM, 16);
        auto c = mem_alloc(ctx, DMEM, 32);
        CHECK(a.valid() && b.valid() && c.valid());
        CHECK(a.offset() == 0 && b.offset() == 16 && c.offset() == 32);
        CHECK(!mem_alloc(ctx, DMEM, 4).valid());
        b.release();
        uint32_t value = 0;
        CHECK(mem_read(b, 0, &value) == PHAL_STATUS_INVALID);
        CHECK(!mem_alloc(ctx, DMEM, 20).valid()); // Fragmentation, not total bytes.
        auto reused = mem_alloc(ctx, DMEM, 16);
        CHECK(reused.valid() && reused.offset() == 16);
        CHECK(mem_write(a, 0, 0x12345678) == PHAL_STATUS_OK);
        CHECK(mem_read(a, 0, &value) == PHAL_STATUS_OK && value == 0x12345678);
        CHECK(mem_write(a, 16, 1) == PHAL_STATUS_INVALID);
        CHECK(mem_write(a, 2, 1) == PHAL_STATUS_INVALID);
        CHECK(mem_write(a, 0, std::vector<uint8_t>(5)) == PHAL_STATUS_INVALID);
        CHECK(mem_read(a, 0, static_cast<uint32_t*>(nullptr)) == PHAL_STATUS_INVALID);
        auto moved = std::move(a);
        CHECK(moved.valid() && !a.valid());
        reused = std::move(moved); // Releases old interval at 16.
        CHECK(!moved.valid() && reused.offset() == 0);
        CHECK(mem_alloc(ctx, DMEM, 16).offset() == 16);
    }
    CHECK(mem_alloc(ctx, DMEM, 64).valid()); // All ordinary allocations reclaimed.
    auto sram = mem_alloc(ctx, HQC_SRAM_0, 64);
    CHECK(sram.valid());
    CHECK(mem_write(sram, 4, 0x55aa) == PHAL_STATUS_OK);
    uint32_t value = 0;
    CHECK(common::bar::read32(ctx, 0, 0x904, value) && value == 0x55aa);
    auto ddp0 = ctx;
    ddp0.module_type = "ddp";
    ddp0.reg_base_offset = 0x1000;
    auto ddp1 = ddp0;
    ddp1.module_index = 1;
    ddp1.reg_base_offset = 0x2000;
    auto a = mem_alloc(ddp0, DDP_ILM, 64);
    auto b = mem_alloc(ddp1, DDP_ILM, 64);
    CHECK(a.valid() && b.valid() && a.offset() == b.offset());
    CHECK(mem_write(a, 0, 11) == PHAL_STATUS_OK);
    CHECK(mem_write(b, 0, 22) == PHAL_STATUS_OK);
    CHECK(common::bar::read32(ctx, 0, 0x1200, value) && value == 11);
    CHECK(common::bar::read32(ctx, 0, 0x2200, value) && value == 22);
    CHECK(mem_write(ddp0, HQC_SRAM_0, 0, 1) == PHAL_STATUS_INVALID);

    ctx.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    constexpr size_t THREADS = 8, PER_THREAD = 32;
    std::array<std::vector<MemBuffer>, THREADS> held;
    std::vector<std::thread> workers;
    std::atomic<bool> ok{true};
    for (size_t i = 0; i < THREADS; ++i) workers.emplace_back([&, i] {
        for (size_t j = 0; j < PER_THREAD; ++j) {
            auto buffer = mem_alloc(ctx, DMEM, 64);
            uint32_t expected = static_cast<uint32_t>(i * PER_THREAD + j + 1), actual = 0;
            if (!buffer.valid() || mem_write(buffer, 0, expected) != PHAL_STATUS_OK ||
                mem_read(buffer, 0, &actual) != PHAL_STATUS_OK || actual != expected) ok = false;
            held[i].push_back(std::move(buffer));
        }
    });
    for (auto& worker : workers) worker.join();
    CHECK(ok);
    std::vector<uint64_t> offsets;
    for (const auto& list : held)
        for (const auto& buffer : list) offsets.push_back(buffer.offset());
    std::sort(offsets.begin(), offsets.end());
    for (size_t i = 1; i < offsets.size(); ++i) CHECK(offsets[i] >= offsets[i - 1] + 64);
    workers.clear();
    for (size_t i = 0; i < THREADS; ++i)
        workers.emplace_back([&, i] { held[i].clear(); });
    for (auto& worker : workers) worker.join();
    CHECK(mem_alloc(ctx, DMEM, 0x10000000).valid());
    auto uncertain = mem_alloc(ctx, DMEM, 64);
    const auto retained_offset = uncertain.offset();
    uncertain.keep_allocated();
    CHECK(!uncertain.valid());
    auto next = mem_alloc(ctx, DMEM, 64);
    CHECK(next.valid() && next.offset() != retained_offset);
}

int main()
{
    try {
        register_views();
        memory_and_phal();
        phal_logging();
        buffer_and_dma();
        examples_and_lifecycle();
        aperture_mapping();
        topology_and_pcie_catalog();
        module_lookup();
        device_allocations();
        std::cout << "IO contracts passed (software only; no hardware validation)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
