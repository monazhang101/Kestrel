#include "diag/core/Common.h"
#include "diag/core/HalContext.h"
#include "diag/core/PhalBridge.h"
#include "diag/core/TestTarget.h"
#include "diag/implementer/Implementer.h"
#include "diag/implementer/AtlasResource.h"
#include "diag/device/DeviceManager.h"

#include <atlas_csr_sw/src/h/TOP.h>

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace {

DeviceContext make_device(HalContext& hal, const char* bdf = "test",
                          Logger* logger = nullptr)
{
    DeviceContext ctx;
    ctx.bdf = bdf;
    ctx.tpu_type = TPUType::Atlas;
    ctx.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    auto mapped = hal.mmap_bar_space(std::move(ctx));
    hal.bind_device(mapped, logger);
    return mapped;
}

std::string capture_stdout(const std::function<void()>& operation)
{
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    try {
        operation();
    } catch (...) {
        std::cout.rdbuf(previous);
        throw;
    }
    std::cout.rdbuf(previous);
    return output.str();
}

void phal_is_chip_scoped()
{
    HalContext hal;
    auto device = make_device(hal);
    auto scope = hal.begin_testcase();
    CHECK(scope);
    auto* root = hal.phal_context();
    CHECK(root != nullptr && root->env.base == PhalBridge::CHIP_RCF_BASE);

    phal_block_ctx_enter(root, TOP_U_PCIE_0_OFFSET);
    CHECK(root->env.base == PhalBridge::CHIP_RCF_BASE + TOP_U_PCIE_0_OFFSET);
    CHECK(hal.with_pcie_context([&](phal_ctx_t* pcie) {
        CHECK(pcie != root);
        CHECK(pcie->env.base == PhalBridge::CHIP_RCF_BASE + TOP_U_PCIE_0_OFFSET);
        CHECK(root->env.base == PhalBridge::CHIP_RCF_BASE + TOP_U_PCIE_0_OFFSET);
        return PHAL_STATUS_OK;
    }) == PHAL_STATUS_OK);
    phal_block_ctx_exit(root, TOP_U_PCIE_0_OFFSET);
    CHECK(root->env.base == PhalBridge::CHIP_RCF_BASE);
}

class ProbeTarget : public TestTarget {
public:
    ProbeTarget(const DeviceContext& ctx, bool* saw_context)
        : TestTarget("probe", "probe", ctx)
    {
        _add_test("probe", {}, [saw_context](TestInfo& ti) {
            *saw_context = ti.phal_ctx != nullptr &&
                           ti.phal_ctx->env.base == PhalBridge::CHIP_RCF_BASE;
            return *saw_context ? PHAL_STATUS_OK : PHAL_STATUS_ERROR;
        });
    }
};

void framework_injects_context()
{
    HalContext hal;
    auto device = make_device(hal);
    bool saw_context = false;
    ProbeTarget target(device, &saw_context);
    CHECK(target.run_testcase("probe_probe", {}, nullptr, &hal) == PHAL_STATUS_OK);
    CHECK(saw_context);
}

void allocations_are_independent_of_phal_root()
{
    HalContext hal;
    auto device = make_device(hal, "allocator");
    auto testcase_scope = hal.begin_testcase();
    CHECK(testcase_scope);

    auto first = hal.device_mem_alloc(DMEM, 64);
    auto second = hal.device_mem_alloc(DMEM, 64);
    CHECK(first.valid() && second.valid());
    CHECK(first.offset() == 0 && second.offset() == 512);
    CHECK(first.region_base() == 0x10000000 && first.address() == 0x10000000);
    CHECK(second.address() == 0x10000200);

    auto pmu_sram = hal.device_mem_alloc(PMU_SRAM, 4);
    CHECK(pmu_sram.valid());
    CHECK(pmu_sram.region_base() == 0x3b000800);
    CHECK(pmu_sram.address() == 0x3b000800);

    CHECK(!hal.device_mem_alloc(DMEM, 3).valid());
    CHECK(!hal.device_mem_alloc(AIC_SMEM, 4).valid());
    first.release();
    auto reused = hal.device_mem_alloc(DMEM, 64);
    CHECK(reused.valid() && reused.offset() == 0);

    auto fixed = hal.device_mem_alloc(DMEM, 4, 0x1000);
    CHECK(fixed.valid() && fixed.offset() == 0x1000);
    CHECK(!hal.device_mem_alloc(DMEM, 4, 0x1000).valid());
    auto explicit_offset = hal.device_mem_alloc(DMEM, 4, 68);
    CHECK(explicit_offset.valid() && explicit_offset.offset() == 68);
    auto third = hal.device_mem_alloc(DMEM, 80);
    CHECK(third.valid() && third.offset() == 1024 && third.size() == 80);
    auto descriptor = hal.device_mem_alloc(HQC_SRAM_0, 80);
    auto payload = hal.device_mem_alloc(HQC_SRAM_0, 128);
    CHECK(descriptor.valid() && payload.valid() && payload.offset() == 512);
    auto near_end = hal.device_mem_alloc(HQC_SRAM_1, 0x80000 - 4);
    CHECK(near_end.valid());
    CHECK(!hal.device_mem_alloc(HQC_SRAM_1, 4).valid());
    CHECK(hal.device_mem_alloc(HQC_SRAM_1, 4, 0x80000 - 4).valid());
}

void device_memory_api_owns_aperture_context()
{
    HalContext hal;
    auto device = make_device(hal, "device-memory");
    {
        auto testcase_scope = hal.begin_testcase();
        CHECK(testcase_scope);
        auto buffer = hal.device_mem_alloc(DMEM, 4, 0x1000);
        CHECK(buffer.valid());

        constexpr uint32_t expected = 0x12345678;
        uint32_t actual = 0;
        CHECK(mem_write(buffer, 0, expected) == PHAL_STATUS_OK);
        CHECK(mem_read(buffer, 0, &actual) == PHAL_STATUS_OK);
        CHECK(actual == expected);
        CHECK(hal.phal_context() != nullptr);
        CHECK(hal.phal_context()->env.base == PhalBridge::CHIP_RCF_BASE);
    }
    CHECK(hal.phal_context() == nullptr);
    auto next_scope = hal.begin_testcase();
    CHECK(next_scope);
    CHECK(hal.phal_context()->env.base == PhalBridge::CHIP_RCF_BASE);
}

void dmem_supports_native_64_bit_access()
{
    HalContext hal;
    auto device = make_device(hal, "dmem64");
    auto testcase_scope = hal.begin_testcase();
    CHECK(testcase_scope);

    auto prefix = hal.device_mem_alloc(DMEM, 4);
    auto buffer = hal.device_mem_alloc(DMEM, 16, std::nullopt, 8);
    CHECK(prefix.valid() && buffer.valid());
    CHECK(buffer.offset() == 8);

    constexpr uint64_t expected = UINT64_C(0x1122334455667788);
    uint64_t actual = 0;
    CHECK(mem_write64(buffer, 0, expected) == PHAL_STATUS_OK);
    CHECK(mem_read64(buffer, 0, &actual) == PHAL_STATUS_OK);
    CHECK(actual == expected);
    CHECK(mem_read64(buffer, 4, &actual) == PHAL_STATUS_INVALID);
    CHECK(mem_write64(buffer, 4, expected) == PHAL_STATUS_INVALID);

    auto non_dmem = hal.device_mem_alloc(PMU_SRAM, 8, std::nullopt, 8);
    CHECK(non_dmem.valid());
    CHECK(mem_read64(non_dmem, 0, &actual) == PHAL_STATUS_INVALID);
    CHECK(mem_write64(non_dmem, 0, expected) == PHAL_STATUS_INVALID);
}

void csr_headers_are_available()
{
    CHECK(TOP_U_CHIP_OFFSET == 0x20000000u);
    CHECK(TOP_U_PCIE_0_OFFSET != TOP_U_CHIP_OFFSET);
}

void testcase_parameter_lists_are_loaded_in_order()
{
    const auto catalog = load_testcase_policies("isml_diag/policies/testcases");
    const auto& runs = catalog.at("pcie").at("dma_data_transfer").runs;
    CHECK(runs.size() == 8);
    CHECK(runs[0].at("direction") == "h2d");
    CHECK(runs[0].at("size_bytes") == "4096");
    CHECK(runs[0].find("timeout_ms") == runs[0].end());
    CHECK(runs[1].at("direction") == "d2h");
    CHECK(runs[1].at("size_bytes") == "0x100000");
    CHECK(runs[1].at("timeout_ms") == "30000");
    CHECK(runs[2].at("direction") == "d2s2d");
    CHECK(runs[2].at("size_bytes") == "128");
    CHECK(runs[2].find("timeout_ms") == runs[2].end());
    CHECK(runs[3].at("direction") == "d2i2d");
    CHECK(runs[3].at("size_bytes") == "256");
    CHECK(runs[3].find("timeout_ms") == runs[3].end());
    CHECK(runs[4].at("direction") == "h2i");
    CHECK(runs[4].at("size_bytes") == "256");
    CHECK(runs[5].at("direction") == "i2h");
    CHECK(runs[5].at("size_bytes") == "256");
    CHECK(runs[6].at("direction") == "h2s");
    CHECK(runs[6].at("size_bytes") == "256");
    CHECK(runs[7].at("direction") == "s2h");
    CHECK(runs[7].at("size_bytes") == "256");
}

void atlas_resource_info_is_decoded()
{
    CHECK(ATLAS_BAR0_RCF_BASE == 0x20000000ull);
    CHECK(ATLAS_PMU_INTERNAL_BASE == 0x18000000ull);
    CHECK(ATLAS_PMU_BASE == 0x38000000ull);
    CHECK(ATLAS_PMU_UNLOCK_BAR0_OFFSET == 0x38000000ull);
    CHECK(ATLAS_PMU_UNLOCK_KEY == 0x5a5aa5a5u);
    CHECK(ATLAS_PMU_REG_WINDOW == 0x800ull);
    CHECK(ATLAS_FW_STATUS_BAR0_OFFSET == 0x3a10285cull);
    CHECK(ATLAS_RESOURCE_INFO_BAR0_OFFSET == 0x3a10589cull);

    const auto isi_aic = decode_atlas_presilicon_resource(0x00031042u);
    CHECK(isi_aic.valid);
    CHECK(isi_aic.profile_matched);
    CHECK(isi_aic.platform == AtlasPlatform::Fpga);
    CHECK(isi_aic.isi_mask == 0x33u);
    CHECK(isi_aic.isi_indices() == std::vector<uint32_t>({0, 1, 4, 5}));
    CHECK(!isi_aic.has_isi(3));
    CHECK(isi_aic.has_isi(4));
    CHECK(isi_aic.aic.profile == AtlasAicProfile::FpgaCut3);
    CHECK(isi_aic.aic.tpe_count == 1);
    CHECK(isi_aic.aic.lpe_count == 2);
    CHECK(isi_aic.profile_name == "isi-aic-cut3");
    CHECK(isi_aic.fake_dmem_size_bytes ==
          8ull * 1024ull * 1024ull * 1024ull);
    CHECK(isi_aic.jtag_mux == AtlasJtagMux::Pmu);

    const auto fpga = decode_atlas_presilicon_resource(0x00000002u);
    CHECK(fpga.valid);
    CHECK(fpga.profile_matched);
    CHECK(fpga.profile_name == "base");
    CHECK(fpga.dmc_mask == 0x249u);
    CHECK(fpga.fake_dmem_size_bytes ==
          8ull * 1024ull * 1024ull * 1024ull);
    CHECK(fpga.has_dmc(0, 0));
    CHECK(fpga.has_dmc(3, 0));
    CHECK(!fpga.has_dmc(0, 1));
    CHECK(!fpga.has_cxl(0, 0));
    CHECK(fpga.get_dmc_type(0, 0) == AtlasDmcType::FakeDmem);

    const auto emu_base = decode_atlas_presilicon_resource(0x00000001u);
    CHECK(emu_base.valid);
    CHECK(emu_base.profile_name == "base1");
    CHECK(emu_base.dmc_mask == 0xfffu);
    CHECK(emu_base.fake_dmem_size_bytes ==
          32ull * 1024ull * 1024ull * 1024ull);
    CHECK(emu_base.cxl_mask == 0x000u);
    CHECK(emu_base.has_dmc(0, 1));
    CHECK(emu_base.has_dmc(3, 2));

    const auto cxl1 = decode_atlas_presilicon_resource(0x00000111u);
    CHECK(cxl1.valid);
    CHECK(cxl1.profile_name == "cxl1");
    CHECK(cxl1.dmc_mask == 0xfffu);
    CHECK(cxl1.cxl_mask == 0x001u);
    CHECK(cxl1.has_dmc(0, 1));
    CHECK(!cxl1.has_cxl(0, 1));

    const auto cxl2 = decode_atlas_presilicon_resource(0x00000211u);
    CHECK(cxl2.valid);
    CHECK(cxl2.dmc_mask == 0xfffu);
    CHECK(cxl2.cxl_mask == 0x007u);
    CHECK(cxl2.has_cxl(0, 1));
    CHECK(!cxl2.has_cxl(1, 0));
    CHECK(cxl2.get_dmc_type(0, 0) == AtlasDmcType::Cxl30);
    CHECK(cxl2.get_dmc_type(0, 1) == AtlasDmcType::Cxl30);
    CHECK(cxl2.get_dmc_type(0, 2) == AtlasDmcType::Cxl20);

    const auto cxl3 = decode_atlas_presilicon_resource(0x00000311u);
    CHECK(cxl3.valid);
    CHECK(cxl3.dmc_mask == 0xfffu);
    CHECK(cxl3.cxl_mask == 0x249u);
    CHECK(cxl3.has_dmc(0, 1));
    CHECK(!cxl3.has_cxl(0, 1));
    CHECK(cxl3.get_dmc_type(0, 0) == AtlasDmcType::Cxl30);
    CHECK(cxl3.get_dmc_type(1, 0) == AtlasDmcType::Cxl30);
    CHECK(cxl3.get_dmc_type(2, 0) == AtlasDmcType::Cxl20);
    CHECK(cxl3.get_dmc_type(3, 0) == AtlasDmcType::Cxl20);

    const auto cxl4 = decode_atlas_presilicon_resource(0x00020151u);
    CHECK(cxl4.valid);
    CHECK(cxl4.profile_name == "cxl4");
    CHECK(cxl4.dmc_mask == 0xfffu);
    CHECK(cxl4.cxl_mask == 0x001u);
    CHECK(cxl4.aic.profile == AtlasAicProfile::Cut2);

    const auto emu_64g = decode_atlas_presilicon_resource(0x00400001u);
    CHECK(emu_64g.valid);
    CHECK(emu_64g.platform == AtlasPlatform::Emu);
    CHECK(emu_64g.profile_name == "base2");
    CHECK(emu_64g.fake_dmem_size_bytes ==
          64ull * 1024ull * 1024ull * 1024ull);

    for (const auto& profile : ATLAS_EMU_RESOURCE_PROFILES) {
        const auto info = decode_atlas_presilicon_resource(profile.raw);
        CHECK(info.valid);
        CHECK(info.profile_matched);
        CHECK(info.profile_name == profile.name);
        CHECK(info.dmc_mask == profile.dmc_mask);
    }
    for (const auto& profile : ATLAS_FPGA_RESOURCE_PROFILES) {
        const auto info = decode_atlas_presilicon_resource(profile.raw);
        CHECK(info.valid);
        CHECK(info.profile_matched);
        CHECK(info.profile_name == profile.name);
        CHECK(info.dmc_mask == profile.dmc_mask);
    }

    CHECK(!decode_atlas_presilicon_resource(0u).valid);
    CHECK(!decode_atlas_presilicon_resource(0xffffffffu).valid);

    const auto fallback_cxl =
        decode_atlas_presilicon_resource(0x00100211u);
    CHECK(fallback_cxl.valid);
    CHECK(!fallback_cxl.profile_matched);
    CHECK(fallback_cxl.profile_name == "unmatched");
    CHECK(fallback_cxl.has_cxl_subsystem);
    CHECK(fallback_cxl.cxl_mask == 0x007u);
    CHECK(fallback_cxl.has_cxl(0, 2));
    CHECK(fallback_cxl.dmc_mask == ATLAS_ALL_DMC_MASK);

    const auto fallback_isi =
        decode_atlas_presilicon_resource(0x00001021u);
    CHECK(fallback_isi.valid);
    CHECK(!fallback_isi.profile_matched);
    CHECK(fallback_isi.isi_mask == 0x33u);
    CHECK(fallback_isi.isi_indices() ==
          std::vector<uint32_t>({0, 1, 4, 5}));

    const auto fallback_aic =
        decode_atlas_presilicon_resource(0x00010032u);
    CHECK(fallback_aic.valid);
    CHECK(!fallback_aic.profile_matched);
    CHECK(fallback_aic.aic.profile == AtlasAicProfile::EmuCut1);
    CHECK(fallback_aic.has_aic_blocks(ATLAS_AIC_TPE0));
    CHECK(fallback_aic.dmc_mask == ATLAS_DMC0_MASK);

    CHECK(!decode_atlas_presilicon_resource(0x00000411u).valid);
    CHECK(!decode_atlas_presilicon_resource(0x00002021u).valid);
    CHECK(!decode_atlas_presilicon_resource(0x00060032u).valid);
    CHECK(!decode_atlas_presilicon_resource(0x00400002u).valid);
}

void bar_write32_round_trips()
{
    HalContext hal;
    make_device(hal, "bar-write");

    uint32_t value = 0;
    CHECK(hal.write_bar32(0, 4, 0x1234abcdu));
    CHECK(hal.read_bar32(0, 4, value));
    CHECK(value == 0x1234abcdu);
}

void discovery_uses_resource_inventory()
{
    DeviceManager manager("isml_diag/policies");
    const auto tree = manager.discover();

    const auto find = [&tree](const std::string& name)
        -> const DeviceDiscoveryInfo* {
        for (const auto& device : tree.devices) {
            if (device.name == name) return &device;
        }
        return nullptr;
    };

    const auto* root = find("TPU0");
    CHECK(root != nullptr);
    CHECK(root->properties.at("profile") == "isi-aic-cut3");
    CHECK(root->properties.at("profile_matched") == "true");
    CHECK(root->properties.at("fake_dmem_size_gib") == "8");
    CHECK(root->properties.at("isi_mask") == "0x33");
    CHECK(root->properties.at("dmc_mask") == "0x249");
    CHECK(root->properties.at("cxl_mask") == "0x000");
    CHECK(find("ISI_0_0") != nullptr);
    CHECK(find("ISI_0_1") != nullptr);
    CHECK(find("ISI_0_3") == nullptr);
    CHECK(find("ISI_0_4") != nullptr);
    CHECK(find("ISI_0_5") != nullptr);

    const auto* dmc = find("DMC_0_0_0");
    CHECK(dmc != nullptr);
    CHECK(dmc->parent == "DDP_0_0");
    CHECK(dmc->properties.at("type") == "fake-dmem");
    CHECK(dmc->properties.at("size_gib") == "8");
    CHECK(find("DMC_0_0_1") == nullptr);
    CHECK(find("DMC_0_0_2") == nullptr);
    CHECK(find("DMC_0_1_0") != nullptr);
    CHECK(find("DMC_0_2_0") != nullptr);
    CHECK(find("DMC_0_3_0") != nullptr);

    const auto* aic = find("AIC_0");
    CHECK(aic != nullptr);
    CHECK(aic->properties.at("profile") == "fpga-cut3");
}

void logging_uses_flow_and_regio_levels()
{
    Logger logger;
    logger.set_level(LogLevel::Flow);
    auto output = capture_stdout([&] {
        logger.info("owner");
        logger.flow("phase");
        logger.regio("register");
    });
    CHECK(output.find("[INFO] owner") != std::string::npos);
    CHECK(output.find("[FLOW] phase") != std::string::npos);
    CHECK(output.find("register") == std::string::npos);

    logger.set_level(LogLevel::RegIo);
    output = capture_stdout([&] {
        logger.flow("phase");
        logger.regio("register");
    });
    CHECK(output.find("[FLOW] phase") != std::string::npos);
    CHECK(output.find("[REGIO] register") != std::string::npos);

    logger.set_level(LogLevel::Debug);
    output = capture_stdout([&] { logger.debug("legacy-debug"); });
    CHECK(output.find("[FLOW] legacy-debug") != std::string::npos);
    logger.set_level(LogLevel::Trace);
    output = capture_stdout([&] { logger.trace("legacy-trace"); });
    CHECK(output.find("[REGIO] legacy-trace") != std::string::npos);
}

void phal_regio_suppression_is_scoped()
{
    Logger logger;
    logger.set_level(LogLevel::RegIo);
    HalContext hal;
    auto device = make_device(hal, "phal-regio", &logger);
    auto testcase_scope = hal.begin_testcase();
    CHECK(testcase_scope);
    auto* phal = hal.phal_context();
    CHECK(phal != nullptr);
    phal->env.base = 0;

    uint32_t value = 0;
    auto output = capture_stdout([&] {
        CHECK(phal_read(phal, 0, &value) == PHAL_STATUS_OK);
    });
    CHECK(output.find("[REGIO] phal_read") != std::string::npos);

    output = capture_stdout([&] {
        ScopedPhalRegIoSuppression suppress(phal);
        CHECK(phal_read(phal, 0, &value) == PHAL_STATUS_OK);
    });
    CHECK(output.empty());

    phal->env.base = PhalBridge::CHIP_RCF_BASE;
    output = capture_stdout([&] {
        ScopedPhalRegIoSuppression suppress(phal);
        CHECK(phal_read(phal, 0, &value) != PHAL_STATUS_OK);
    });
    CHECK(output.find("[ERROR] phal_read") != std::string::npos);
}

}

int main()
{
    try {
        phal_is_chip_scoped();
        framework_injects_context();
        allocations_are_independent_of_phal_root();
        device_memory_api_owns_aperture_context();
        dmem_supports_native_64_bit_access();
        csr_headers_are_available();
        testcase_parameter_lists_are_loaded_in_order();
        atlas_resource_info_is_decoded();
        bar_write32_round_trips();
        discovery_uses_resource_inventory();
        logging_uses_flow_and_regio_levels();
        phal_regio_suppression_is_scoped();
        std::cout << "IO contracts passed (software only; no hardware validation)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
