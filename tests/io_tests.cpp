#include "diag/core/Common.h"
#include "diag/core/HalContext.h"
#include "diag/device/DeviceManager.h"

#include <algorithm>
#include <atomic>
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
    ctx.tpu_type = TPUType::Atlas;
    ctx.reg_base_offset = 0x400;
    ctx.reg_size = 0x100;
    ctx.pcie_control_base = 0x800;
    ctx.phal = hal.phal().get_context(ctx, ctx.reg_base_offset, PhalProject::Atlas);
    ctx.pcie_phal = hal.phal().get_context(ctx, ctx.pcie_control_base, PhalProject::Atlas);
    auto* control = ctx.pcie_phal;
    auto* module = ctx.phal;
    CHECK(module != control && module->env.base == 0x400 && control->env.base == 0x800);
    phal_block_ctx_enter(module, 0x20);
    CHECK(dmem_write(ctx, 4, 0x12345678) == PHAL_STATUS_OK);
    uint32_t value = 0;
    CHECK(dmem_read(ctx, 4, &value) == PHAL_STATUS_OK && value == 0x12345678);
    CHECK(module->env.base == 0x420 && control->env.base == 0x800);
    CHECK(control->apertures[4][0].target_addr == ctx.dmem_base);
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

    TestInfo ti;
    ti.hal = &hal;
    common::devmem::Window window{4, 0, 0, ctx.dmem_base, ctx.dmem_size, 0};
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
    CHECK(dmem_read(ctx, 4, &value) == PHAL_STATUS_ERROR && value == 99);
    CHECK(dmem_read(ctx, 2, &value) == PHAL_STATUS_INVALID);
    CHECK(dmem_read(ctx, 0, nullptr) == PHAL_STATUS_INVALID);
    CHECK(dmem_read(ctx, ctx.dmem_size, &value) == PHAL_STATUS_INVALID);
    ctx.pcie_phal = control;
    ctx.dmem_base = std::numeric_limits<uint64_t>::max() - 3;
    CHECK(dmem_write(ctx, 0, 5) == PHAL_STATUS_INVALID);
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
    std::vector<uint8_t> memory(0x200000, 0x5a);
    auto& bar = ctx.bar_mappings[2];
    CHECK(bar.bar_index == 4);
    bar.mapped_base = memory.data();
    bar.mapped_size = bar.size = memory.size();
    ctx.pcie_phal = hal.phal().get_context(ctx, 0x800, PhalProject::Atlas);
    CHECK(ctx.dmem_size == 0x10000000);
    ctx.phal = hal.phal().get_context(ctx, ctx.reg_base_offset, PhalProject::Atlas);
    phal_block_ctx_enter(ctx.phal, 0x800); // DMEM must use the PCIe root, not this block.
    const std::vector<uint8_t> data{1, 2, 3, 4, 5, 6, 7};
    std::vector<uint8_t> actual(data.size()), empty;
    CHECK(dmem_write(ctx, 0x100001, data) == PHAL_STATUS_OK); // beyond old 1 MiB limit, unaligned bytes
    CHECK(dmem_read(ctx, 0x100001, &actual) == PHAL_STATUS_OK && actual == data);
    CHECK(memory[0x100000] == 0x5a && memory[0x100008] == 0x5a);
    CHECK(ctx.pcie_phal->apertures[4][0].size == 0x10000000);
    CHECK(ctx.pcie_phal->env.base == 0x800);
    phal_block_ctx_exit(ctx.phal, 0x800);
    CHECK(dmem_read(ctx, 0, &empty) == PHAL_STATUS_INVALID);
    CHECK(dmem_write(ctx, 0, empty) == PHAL_STATUS_INVALID);
    CHECK(dmem_read(ctx, 0, static_cast<std::vector<uint8_t>*>(nullptr)) == PHAL_STATUS_INVALID);
    CHECK(dmem_read(ctx, ctx.dmem_size - 4, &actual) == PHAL_STATUS_INVALID && actual == data);
    CHECK(dmem_read(ctx, memory.size() - 4, &actual) == PHAL_STATUS_ERROR && actual == data);
    CHECK(dmem_write(ctx, memory.size() - 4, data) == PHAL_STATUS_ERROR);
    bar.writable = false;
    CHECK(dmem_write(ctx, 0, data) == PHAL_STATUS_ERROR && memory[0] == 0x5a);
    bar.writable = true;
    ctx.pcie_phal = nullptr;
    CHECK(dmem_read(ctx, 0, &actual) == PHAL_STATUS_ERROR && actual == data);

    // Case sequencing only: a test double copies bytes, not HQC or firmware.
    auto impl = std::make_unique<DmaProbeImpl>();
    std::vector<DmaTransferRequest> requests;
    std::vector<uint8_t> intermediate(256);
    size_t fail_on_call = 0;
    bool no_copy = false;
    impl->copy = [&](const DmaTransferRequest& req) {
        requests.push_back(req);
        CHECK(req.host_buffer == nullptr && req.size_bytes == 128);
        if (requests.size() == fail_on_call) return PHAL_STATUS_TIMEOUT;
        if (no_copy) return PHAL_STATUS_OK;
        const bool forward = req.type == DmaTransferType::D2I || req.type == DmaTransferType::D2S;
        const bool ok = forward
            ? common::bar::read(ctx, 4, req.src_offset, intermediate.data() + req.dst_offset, req.size_bytes)
            : common::bar::write(ctx, 4, req.dst_offset, intermediate.data() + req.src_offset, req.size_bytes);
        return ok ? PHAL_STATUS_OK : PHAL_STATUS_ERROR;
    };
    PCIeModule pcie("PCIE", ctx, {0, 0, 0x800, 0x100}, std::move(impl));
    Logger logger;
    logger.set_level(LogLevel::Error);
    TestArgs args{{"direction", "d2i2d"}, {"size_bytes", "128"}, {"pattern", "zero"},
                  {"device_offset", "0x100020"}, {"return_offset", "0x100200"}};
    CHECK(pcie.run_testcase("dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(requests.size() == 2 && requests[0].type == DmaTransferType::D2I && requests[1].type == DmaTransferType::I2D);
    CHECK(requests[0].src_offset == 0x100020 && requests[1].dst_offset == 0x100200);
    args["direction"] = "d2s2d";
    requests.clear();
    CHECK(pcie.run_testcase("dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(requests.size() == 2 && requests[0].type == DmaTransferType::D2S && requests[1].type == DmaTransferType::S2D);
    for (size_t fail_at : {1u, 2u}) {
        requests.clear();
        fail_on_call = fail_at;
        CHECK(pcie.run_testcase("dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_TIMEOUT);
        CHECK(requests.size() == fail_at);
    }
    requests.clear();
    fail_on_call = 0;
    no_copy = true;
    CHECK(pcie.run_testcase("dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(requests.size() == 2); // Poison detects a successful completion with no copy.
    requests.clear();
    pcie.get_context().bar_mappings[2].writable = false;
    CHECK(pcie.run_testcase("dma_data_transfer", args, &logger, &hal) == PHAL_STATUS_ERROR);
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
    Logger logger;
    logger.set_level(LogLevel::Error);
    PMUModule pmu("PMU", ctx, {0, 0, 0x100, 0x2108030});
    DDPModule ddp("DDP", ctx, {0, 0, 0x1000, 0x90000});
    DDPModule ddp1("DDP1", ctx, {1, 0, 0x90000, 0x90000});
    PCIeModule pcie("PCIE", ctx, {0, 0, 0x800, 0x10231c}, nullptr);
    ISIModule isi("ISI", ctx, {0, 0, 0x300, 0x40});
    CHECK(pmu.run_testcase("example", {}, &logger) == PHAL_STATUS_ERROR);
    CHECK(pmu.run_testcase("example", {{"block_offset", "0x10"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(pmu.run_testcase("example", {{"write_enable", "1"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(ddp.run_testcase("example", {{"block_offset", "0"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(isi.run_testcase("example", {{"reg_offset", "0"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(common::bar::write32(ctx, 0, 0x2106100, 0x01010001));
    CHECK(common::bar::write32(ctx, 0, 0x81800, 0xabcd16c3));
    CHECK(common::bar::write32(ctx, 0, 0x110800, 0xabcd16c3));
    CHECK(common::bar::write32(ctx, 0, 0x100800, 0xabcd16c3));
    CHECK(common::bar::write32(ctx, 0, 0x31c, 0x202020));
    CHECK(common::bar::write32(ctx, 4, 0, 0xccdd));
    // Register examples must work even with a read-only CSR BAR.
    pmu.get_context().bar_mappings[0].writable = false;
    ddp.get_context().bar_mappings[0].writable = false;
    ddp1.get_context().bar_mappings[0].writable = false;
    isi.get_context().bar_mappings[0].writable = false;
    pcie.get_context().bar_mappings[0].writable = false;
    std::ostringstream raw_read_output;
    logger.set_level(LogLevel::Info);
    auto* previous_output = std::cout.rdbuf(raw_read_output.rdbuf());
    const auto raw_read_status = pcie.run_testcase(
        "bar_read32_abs", {{"offset", "0x110800"}}, &logger, &hal);
    std::cout.rdbuf(previous_output);
    logger.set_level(LogLevel::Error);
    CHECK(raw_read_status == PHAL_STATUS_OK);
    CHECK(pcie.get_context().phal != nullptr && pcie.get_context().pcie_phal != nullptr);
    CHECK(raw_read_output.str().find("bar0_offset=0x110800 value=0xabcd16c3") != std::string::npos);
    CHECK(pcie.run_testcase("bar_read32_abs", {}, &logger) == PHAL_STATUS_INVALID);
    CHECK(pcie.run_testcase("bar_read32_abs", {{"offset", "0x110801"}}, &logger, &hal) == PHAL_STATUS_INVALID);
    CHECK(pcie.run_testcase("bar_read32_abs", {{"offset", "0x2107000"}}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(pmu.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(pmu.get_context().phal->env.base == pmu.get_context().reg_base_offset);
    CHECK(ddp.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(common::bar::write32(ctx, 0, 0x2106100, 0));
    CHECK(pmu.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(ddp1.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(pcie.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_OK);
    pcie.get_context().bar_mappings[2].writable = false;
    CHECK(pcie.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    pcie.get_context().bar_mappings[2].writable = true;
    CHECK(common::bar::write32(ctx, 0, 0x100800, 0xdeadbeef));
    CHECK(pcie.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(common::bar::write32(ctx, 0, 0x110800, 0xdeadbeef));
    CHECK(ddp1.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(ddp.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_OK);
    CHECK(ddp.get_context().phal->env.base == ddp.get_context().reg_base_offset);
    CHECK(ddp1.get_context().phal->env.base == ddp1.get_context().reg_base_offset);
    CHECK(isi.run_testcase("example", {}, &logger, &hal) == PHAL_STATUS_ERROR); // native mock is honest
    uint32_t value = 0;
    CHECK(common::bar::read32(ctx, 4, 0, value) && value == 0xccdd);
    CHECK(pmu.get_context().logger == nullptr);

    ProbeTarget* self = nullptr;
    ProbeTarget throwing("throw", ctx, [&](TestInfo&) -> TestStatus {
        phal_block_ctx_enter(self->get_context().phal, 0x20);
        throw std::runtime_error("injected failure");
    });
    self = &throwing;
    CHECK(throwing.run_testcase("probe", {}, &logger, &hal) == PHAL_STATUS_ERROR);
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
    std::thread a([&] { first.run_testcase("probe", {}, &logger, &hal); });
    std::thread b([&] { second.run_testcase("probe", {}, &logger, &hal); });
    a.join(); b.join();
    CHECK(!overlap);
}

void aperture_mapping()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "aperture_test";
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
    CHECK(pcie.run_testcase("sequential_aperture_mapping", {}, &logger) == PHAL_STATUS_ERROR);
    CHECK(pcie.run_testcase("sequential_aperture_mapping", {}, &logger, &hal) == PHAL_STATUS_OK);
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
    CHECK(pcie.run_testcase("sequential_aperture_mapping", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(restored());

    // Fail the first BAR4 write after all BAR2 writes; BAR2 must still be restored.
    data_bar.mapped_base = bar4.data();
    data_bar.writable = false;
    CHECK(pcie.run_testcase("sequential_aperture_mapping", {}, &logger, &hal) == PHAL_STATUS_ERROR);
    CHECK(restored());
}

void topology_and_pcie_catalog()
{
    DeviceManager manager;
    const auto tree = manager.discover();
    CHECK(tree.devices.size() == 15); // parent + PCIe + PMU + 4 DDP + 8 ISI
    for (const auto& item : tree.devices) CHECK(item.type != "dmc");
    CHECK(manager.get_target("ATLAS_0")->get_registered_test_names().empty());
    CHECK(manager.get_target("DDP_0_3")->get_context().reg_base_offset == 0x35000000);
    CHECK(manager.get_target("ISI_0_7")->get_context().reg_base_offset == 0x37e00000);
    CHECK(manager.get_target("PMU_0_0")->get_context().reg_base_offset == 0x38000000);
    for (const auto& name : {"PMU_0_0", "DDP_0_0", "ISI_0_0"}) {
        CHECK(manager.get_target(name)->get_registered_test_names() == std::vector<std::string>{"example"});
    }
    auto names = manager.get_target("PCIE_0_0")->get_registered_test_names();
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"bar_read32", "bar_read32_abs", "bar_scan32", "dma_data_transfer", "example", "sequential_aperture_mapping"}));
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
        std::cout << "IO contracts passed (software only; no hardware validation)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
