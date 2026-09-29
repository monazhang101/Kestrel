#include "diag/core/Common.h"
#include "diag/core/HalContext.h"
#include "diag/core/PhalBridge.h"
#include "diag/core/TestTarget.h"
#include "diag/implementer/Implementer.h"

#include <atlas_csr_sw/src/h/TOP.h>

#include <iostream>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace {

DeviceContext make_device(HalContext& hal, const char* bdf = "test")
{
    DeviceContext ctx;
    ctx.bdf = bdf;
    ctx.tpu_type = TPUType::Atlas;
    ctx.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    auto mapped = hal.mmap_bar_space(std::move(ctx));
    hal.bind_device(mapped);
    return mapped;
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
    CHECK(hal.with_aperture_context([&](phal_ctx_t* aperture) {
        CHECK(aperture != root);
        CHECK(aperture->env.base == PhalBridge::CHIP_RCF_BASE + TOP_U_PCIE_0_OFFSET);
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
    CHECK(first.offset() == 0 && second.offset() == 64);
    CHECK(!hal.device_mem_alloc(DMEM, 3).valid());
    CHECK(!hal.device_mem_alloc(AIC_SMEM, 4).valid());
    first.release();
    auto reused = hal.device_mem_alloc(DMEM, 64);
    CHECK(reused.valid() && reused.offset() == 0);

    auto fixed = hal.device_mem_alloc(DMEM, 4, 0x1000);
    CHECK(fixed.valid() && fixed.offset() == 0x1000);
    CHECK(!hal.device_mem_alloc(DMEM, 4, 0x1000).valid());
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

void csr_headers_are_available()
{
    CHECK(TOP_U_CHIP_OFFSET == 0x20000000u);
    CHECK(TOP_U_PCIE_0_OFFSET != TOP_U_CHIP_OFFSET);
}

}

int main()
{
    try {
        phal_is_chip_scoped();
        framework_injects_context();
        allocations_are_independent_of_phal_root();
        device_memory_api_owns_aperture_context();
        csr_headers_are_available();
        std::cout << "IO contracts passed (software only; no hardware validation)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
