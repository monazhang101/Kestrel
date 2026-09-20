#include "diag/core/Common.h"
#include "diag/core/HalContext.h"
#include "diag/device/DeviceManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>

extern "C" {
#include <phal/components/isi/isi.h>
#include <phal/components/pcie/pcie.h>
}

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

class ProbeTarget : public TestTarget {
public:
    ProbeTarget(const std::string& name, const DeviceContext& ctx, TestcaseFunc fn,
                bool prepare = false) : TestTarget(name, "probe", ctx)
    {
        _add_test("probe", {}, std::move(fn), prepare);
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
        CHECK(common::bar::write32(device, 0, ctx.reg_base_offset + 0x14, expected));
        CHECK(block_ctx_enter(ctx, 0x10) == TestStatus::OK);
        CHECK(block_ctx_enter(ctx, 4) == TestStatus::OK);
        uint32_t value = 0;
        CHECK(reg_read(ctx, 0, &value) == TestStatus::OK && value == expected);
        CHECK(reg_write(ctx, 0, expected + 100) == TestStatus::OK);
        CHECK(common::bar::read32(device, 0, ctx.reg_base_offset + 0x14, value));
        CHECK(value == expected + 100);
        CHECK(block_ctx_exit(ctx, 4) == TestStatus::OK);
        CHECK(block_ctx_exit(ctx, 0x10) == TestStatus::OK && ctx.block_offset == 0);
        ++expected;
    }
    auto& ctx = ddp0.get_context();
    uint32_t untouched = 0xabcdef;
    CHECK(reg_read(ctx, 0, nullptr) == TestStatus::INVALID);
    CHECK(reg_read(ctx, 1, &untouched) == TestStatus::INVALID);
    CHECK(reg_read(ctx, 0x40, &untouched) == TestStatus::INVALID);
    CHECK(untouched == 0xabcdef);
    CHECK(reg_write(ctx, 0x3c, 42) == TestStatus::OK);
    CHECK(block_ctx_exit(ctx, 4) == TestStatus::INVALID);
    CHECK(block_ctx_enter(ctx, std::numeric_limits<uint64_t>::max()) == TestStatus::INVALID);
    ctx.reg_base_offset = std::numeric_limits<uint64_t>::max() - 3;
    CHECK(reg_read(ctx, 4, &untouched) == TestStatus::INVALID);
    ctx.reg_base_offset = 0x10000;
    CHECK(reg_read(ctx, 0, &untouched) == TestStatus::ERROR);
    CHECK(untouched == 0xabcdef);
    ctx.reg_base_offset = 0x200;
    ctx.bar_mappings[0].writable = false;
    CHECK(reg_write(ctx, 0, 77) == TestStatus::ERROR);
    CHECK(!common::bar::write32(ctx, 0, 0x200, 77));
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
    ctx.phal = hal.phal().get_context(ctx, 0, ctx.reg_base_offset, PhalProject::Atlas);
    ctx.pcie_phal = hal.phal().get_context(ctx, 0, ctx.pcie_control_base, PhalProject::Atlas);
    auto* control = static_cast<phal_ctx_t*>(ctx.pcie_phal);
    auto* module = static_cast<phal_ctx_t*>(ctx.phal);
    CHECK(module != control && module->env.base == 0x400 && control->env.base == 0x800);
    CHECK(block_ctx_enter(ctx, 0x20) == TestStatus::OK);
    CHECK(dmem_write(ctx, 4, 0x12345678) == TestStatus::OK);
    uint32_t value = 0;
    CHECK(dmem_read(ctx, 4, &value) == TestStatus::OK && value == 0x12345678);
    CHECK(module->env.base == 0x400 && control->env.base == 0x800);
    CHECK(control->apertures[4][0].target_addr == ctx.dmem_base);
    CHECK(common::bar::read32(ctx, 4, 4, value) && value == 0x12345678);
    CHECK(common::bar::read32(ctx, 4, 0x24, value) && value == 0);
    CHECK(block_ctx_exit(ctx, 0x20) == TestStatus::OK);

    // Exercise the real bridge callback, including PHAL's own base addition.
    CHECK(module->env.ops->write(&module->env, 8, 0x42) == PHAL_STATUS_OK);
    CHECK(common::bar::read32(ctx, 0, 0x408, value) && value == 0x42);
    CHECK(module->env.ops->read(&module->env, 8, &value) == PHAL_STATUS_OK && value == 0x42);

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
    CHECK(dmem_read(ctx, 4, &value) == TestStatus::ERROR && value == 99);
    CHECK(dmem_read(ctx, 2, &value) == TestStatus::INVALID);
    CHECK(dmem_read(ctx, 0, nullptr) == TestStatus::INVALID);
    CHECK(dmem_read(ctx, ctx.dmem_size, &value) == TestStatus::INVALID);
    ctx.pcie_phal = control;
    ctx.dmem_base = std::numeric_limits<uint64_t>::max() - 3;
    CHECK(dmem_write(ctx, 0, 5) == TestStatus::INVALID);
    CHECK(phal_component_isi_linkup(module, 100) != PHAL_STATUS_OK);
    CHECK(from_phal(PHAL_STATUS_INVALID) == TestStatus::INVALID);
    CHECK(from_phal(987) == TestStatus::ERROR);
    CHECK(test_status_name(TestStatus::TIMEOUT | TestStatus::ERROR) == "TIMEOUT|ERROR");
}

void examples_and_lifecycle()
{
    HalContext hal;
    auto ctx = hal.mmap_bar_space({});
    ctx.bdf = "example_test";
    ctx.tpu_type = TPUType::Atlas;
    ctx.pcie_control_base = 0x800;
    Logger logger;
    logger.set_level(LogLevel::Error);
    PMUModule pmu("PMU", ctx, {0, 0, 0x100, 0x40});
    DDPModule ddp("DDP", ctx, {0, 0, 0x200, 0x40});
    ISIModule isi("ISI", ctx, {0, 0, 0x300, 0x40});
    CHECK(pmu.run_testcase("example", {}, &logger) == TestStatus::ERROR);
    CHECK(pmu.run_testcase("example", {{"value", "0x100000000"}}, &logger, &hal) == TestStatus::INVALID);
    CHECK(pmu.run_testcase("example", {{"write_enable", "2"}}, &logger, &hal) == TestStatus::INVALID);
    CHECK(isi.run_testcase("example", {{"timeout_us", "0x100000000"}}, &logger, &hal) == TestStatus::INVALID);
    CHECK(common::bar::write32(ctx, 0, 0x110, 0xaabb));
    CHECK(common::bar::write32(ctx, 4, 0, 0xccdd));
    CHECK(pmu.run_testcase("example", {{"block_offset", "0x10"}, {"write_enable", "1"}}, &logger, &hal) == TestStatus::OK);
    CHECK(ddp.run_testcase("example", {}, &logger, &hal) == TestStatus::OK);
    CHECK(isi.run_testcase("example", {}, &logger, &hal) == TestStatus::ERROR); // native mock is honest
    uint32_t value = 0;
    CHECK(common::bar::read32(ctx, 0, 0x110, value) && value == 0xaabb);
    CHECK(common::bar::read32(ctx, 4, 0, value) && value == 0xccdd);
    CHECK(pmu.get_context().logger == nullptr && pmu.get_context().block_offset == 0);

    ProbeTarget* self = nullptr;
    ProbeTarget throwing("throw", ctx, [&](TestInfo&) -> TestStatus {
        self->get_context().block_offset = 0x20;
        throw std::runtime_error("injected failure");
    });
    self = &throwing;
    CHECK(throwing.run_testcase("probe", {}, &logger) == TestStatus::ERROR);
    CHECK(throwing.get_context().block_offset == 0 && throwing.get_context().logger == nullptr);

    std::atomic<int> active{0};
    std::atomic<bool> overlap{false};
    const auto work = [&](TestInfo&) {
        if (active.fetch_add(1) != 0) overlap = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        --active;
        return TestStatus::OK;
    };
    ProbeTarget first("first", ctx, work), second("second", ctx, work);
    std::thread a([&] { first.run_testcase("probe", {}, &logger); });
    std::thread b([&] { second.run_testcase("probe", {}, &logger); });
    a.join(); b.join();
    CHECK(!overlap);
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
    CHECK(names == (std::vector<std::string>{"bar_read32", "bar_scan32", "dma_data_transfer", "sequential_aperture_mapping"}));
}

int main()
{
    try {
        register_views();
        memory_and_phal();
        examples_and_lifecycle();
        topology_and_pcie_catalog();
        std::cout << "IO contracts passed (software only; no hardware validation)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
