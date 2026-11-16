#include "diag/implementer/HqcDma.h"

#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP_NDM_REG/DDP_NDM_REG.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/HQC/PCIE_HQC_REG/PCIE_HQC_REG.h>
}

extern bool test_host_allocations_enabled;
void release_test_host_storage();

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace {
constexpr uintptr_t HQC_SYNC_CSR = 0x36040000;
constexpr uintptr_t HQC_CSR = 0x36050000;
constexpr uintptr_t CHIP_RCF_BASE = 0x20000000;
constexpr std::array<uintptr_t, 4> DDP_TOP_OFFSETS{{
    TOP_U_DDP_0_OFFSET, TOP_U_DDP_1_OFFSET,
    TOP_U_DDP_2_OFFSET, TOP_U_DDP_3_OFFSET,
}};
constexpr std::array<DirectRcfRegion, 2> TEST_RCF_REGIONS{{
    {MemoryRegion::HQC_SRAM_1, "HQC_SRAM_1", 0, 0x1000, 0x80000, true},
    {MemoryRegion::AIC_SMEM, "AIC_SMEM", 0, 0x30000, 0x1000, true},
}};

struct Model {
    static Model* active;
    std::map<std::pair<uintptr_t, uintptr_t>, uint32_t> registers;
    std::array<uint32_t, 256> done{};
    std::vector<uint32_t> fetch_addresses;
    std::vector<uint32_t> selected_cores;
    std::vector<uintptr_t> ddp_access_bases;

    Model() { active = this; }

    static phal_status_t read(env_t* env, uintptr_t offset, uint32_t* value)
    {
        auto& model = *active;
        if (offset == DDP_NDM_REG_NDM_CTRL_OFFSET) {
            for (const auto ddp_offset : DDP_TOP_OFFSETS) {
                const auto expected = CHIP_RCF_BASE + ddp_offset +
                                      DDP_U_DDP_NDM_REG_0_OFFSET;
                if (env->base == expected) {
                    model.ddp_access_bases.push_back(env->base);
                    break;
                }
                if (env->base == expected + TOP_U_PCIE_0_OFFSET) {
                    model.ddp_access_bases.push_back(env->base);
                    return PHAL_STATUS_INVALID;
                }
            }
        }
        if (env->base == HQC_CSR &&
            (offset == PCIE_HQC_REG_CORE0_DESC_CTRL_IDLE_OFFSET ||
             offset == PCIE_HQC_REG_CORE1_DESC_CTRL_IDLE_OFFSET)) {
            *value = 1;
            return PHAL_STATUS_OK;
        }
        if (env->base == HQC_SYNC_CSR && offset >= SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(0) &&
            offset <= SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(255)) {
            const auto id = (offset - SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(0)) / 4;
            *value = model.done[id];
            return PHAL_STATUS_OK;
        }
        *value = model.registers[{env->base, offset}];
        return PHAL_STATUS_OK;
    }

    static phal_status_t write(env_t* env, uintptr_t offset, uint32_t value)
    {
        auto& model = *active;
        model.registers[{env->base, offset}] = value;
        if (env->base == HQC_SYNC_CSR && offset >= SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(0) &&
            offset <= SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(255)) {
            const auto id = (offset - SOC_HQC_SYNCFLAG_CSR_DONE_OFFSET(0)) / 4;
            model.done[id] = value;
            return PHAL_STATUS_OK;
        }
        uint32_t core = 2;
        if (env->base == HQC_CSR &&
            offset == PCIE_HQC_REG_CORE0_DESC_CTRL_TRIG_OFFSET && value == 1)
            core = 0;
        if (env->base == HQC_CSR &&
            offset == PCIE_HQC_REG_CORE1_DESC_CTRL_TRIG_OFFSET && value == 1)
            core = 1;
        if (core < 2) {
            const auto address_offset = core == 0
                ? PCIE_HQC_REG_CORE0_DESC_CTRL_FETCH_ADDR_LOW_OFFSET
                : PCIE_HQC_REG_CORE1_DESC_CTRL_FETCH_ADDR_LOW_OFFSET;
            const auto address = model.registers[{env->base, address_offset}];
            CHECK(address >= 0x30080000u);
            const auto slot = (address - 0x30080000u) /
                              HqcDmaEngine::DESCRIPTOR_SLOT_STRIDE;
            CHECK(slot < model.done.size());
            model.fetch_addresses.push_back(address);
            model.selected_cores.push_back(core);
            model.done[slot] = 1;
        }
        return PHAL_STATUS_OK;
    }
};
Model* Model::active = nullptr;

std::vector<uintptr_t> expected_ddp_accesses()
{
    std::vector<uintptr_t> result;
    for (const auto ddp_offset : DDP_TOP_OFFSETS) {
        const auto base = CHIP_RCF_BASE + ddp_offset +
                          DDP_U_DDP_NDM_REG_0_OFFSET;
        // Q phase and station ID are separate read-modify-write operations.
        result.push_back(base);
        result.push_back(base);
    }
    return result;
}

DeviceContext bind(HalContext& hal)
{
    DeviceContext device;
    device.bdf = "dma-protocol";
    device.tpu_type = TPUType::Atlas;
    device.memory_regions = {
        {TEST_RCF_REGIONS.data(), TEST_RCF_REGIONS.size()}, {}};
    auto mapped = hal.mmap_bar_space(device);
    mapped.memory_regions = device.memory_regions;
    hal.bind_device(mapped);
    return mapped;
}

void install(HalContext& hal)
{
    static hw_ops_t ops{};
    ops.read = Model::read;
    ops.write = Model::write;
    hal.phal_context()->env.ops = &ops;
    CHECK(hal.with_pcie_context([&](phal_ctx_t* ctx) {
        ctx->env.ops = &ops;
        return PHAL_STATUS_OK;
    }) == PHAL_STATUS_OK);
}

void alignment_contracts()
{
    HalContext hal;
    bind(hal);
    auto scope = hal.begin_testcase();
    CHECK(scope);
    Model model;
    install(hal);
    auto host = hal.host_mem_alloc(1024);
    auto smem = hal.device_mem_alloc(AIC_SMEM, 32, 0);
    auto hqc = hal.device_mem_alloc(HQC_SRAM_1, 512, 0x40000);
    CHECK(host.valid() && smem.valid() && hqc.valid());
    HqcDmaEngine engine(hal);

    DmaDescriptor dmem_bad(engine);
    CHECK(dmem_bad.set_src(host) == PHAL_STATUS_OK);
    CHECK(dmem_bad.set_dst(DmaMemory::Dmem, 1, 0, 4) == PHAL_STATUS_OK);
    CHECK(dmem_bad.set_size(32) == PHAL_STATUS_OK);
    CHECK(dmem_bad.finalize() == PHAL_STATUS_INVALID);

    DmaDescriptor dmem_ok(engine);
    CHECK(dmem_ok.set_src(host) == PHAL_STATUS_OK);
    CHECK(dmem_ok.set_dst(DmaMemory::Dmem, 1, 0, 32) == PHAL_STATUS_OK);
    CHECK(dmem_ok.set_size(32) == PHAL_STATUS_OK);
    CHECK(dmem_ok.finalize() == PHAL_STATUS_OK);

    DmaDescriptor vmem_bad(engine);
    CHECK(vmem_bad.set_src(host) == PHAL_STATUS_OK);
    CHECK(vmem_bad.set_dst(DmaMemory::Vmem, 2, 0, 4) == PHAL_STATUS_OK);
    CHECK(vmem_bad.set_size(32) == PHAL_STATUS_OK);
    CHECK(vmem_bad.finalize() == PHAL_STATUS_INVALID);

    DmaDescriptor smem_ok(engine);
    CHECK(smem_ok.set_src(host) == PHAL_STATUS_OK);
    CHECK(smem_ok.set_dst(DmaMemory::Smem, 2, 1, 4) == PHAL_STATUS_OK);
    CHECK(smem_ok.set_size(32) == PHAL_STATUS_OK);
    CHECK(smem_ok.finalize() == PHAL_STATUS_OK);

    DmaDescriptor imem_bad_address(engine);
    CHECK(imem_bad_address.set_src(host) == PHAL_STATUS_OK);
    CHECK(imem_bad_address.set_dst(DmaMemory::Imem, 2, 2, 32) == PHAL_STATUS_OK);
    CHECK(imem_bad_address.set_size(256) == PHAL_STATUS_OK);
    CHECK(imem_bad_address.finalize() == PHAL_STATUS_INVALID);

    DmaDescriptor imem_bad_size(engine);
    CHECK(imem_bad_size.set_src(host) == PHAL_STATUS_OK);
    CHECK(imem_bad_size.set_dst(DmaMemory::Imem, 2, 2, 256) == PHAL_STATUS_OK);
    CHECK(imem_bad_size.set_size(32) == PHAL_STATUS_OK);
    CHECK(imem_bad_size.finalize() == PHAL_STATUS_INVALID);

    DmaDescriptor imem_ok(engine);
    CHECK(imem_ok.set_src(host) == PHAL_STATUS_OK);
    CHECK(imem_ok.set_dst(DmaMemory::Imem, 2, 2, 256) == PHAL_STATUS_OK);
    CHECK(imem_ok.set_size(256) == PHAL_STATUS_OK);
    CHECK(imem_ok.finalize() == PHAL_STATUS_OK);

    DmaDescriptor range_bad(engine);
    CHECK(range_bad.set_src(host) == PHAL_STATUS_OK);
    CHECK(range_bad.set_dst(smem, 4) == PHAL_STATUS_OK);
    CHECK(range_bad.set_size(32) == PHAL_STATUS_OK);
    CHECK(range_bad.finalize() == PHAL_STATUS_INVALID);

    DmaDescriptor unsupported(engine);
    CHECK(unsupported.set_dst(hqc) == PHAL_STATUS_INVALID);

    DmaDescriptor bad_core(engine);
    CHECK(bad_core.set_dst(DmaMemory::Smem, 4, 0, 0) == PHAL_STATUS_INVALID);
}

void async_slot_protocol()
{
    HalContext hal;
    bind(hal);
    auto scope = hal.begin_testcase();
    CHECK(scope);
    Model model;
    install(hal);
    auto host = hal.host_mem_alloc(512);
    CHECK(host.valid());
    HqcDmaEngine engine(hal);

    std::vector<std::unique_ptr<DmaDescriptor>> jobs;
    std::vector<DmaDescriptor*> batch;
    for (uint32_t i = 0; i < 4; ++i) {
        auto job = std::make_unique<DmaDescriptor>(engine, DmaProfile::H2D);
        CHECK(job->set_src(host, i * 32) == PHAL_STATUS_OK);
        CHECK(job->set_dst(DmaMemory::Dmem, 1, 0, i * 32) == PHAL_STATUS_OK);
        CHECK(job->set_size(32) == PHAL_STATUS_OK);
        CHECK(job->fill_descriptor() == PHAL_STATUS_OK);
        CHECK(job->state() == DmaJobState::Prepared);
        CHECK(job->slot_id() == i + 1 && job->sync_flag() == i + 1);
        CHECK(job->execution_thread() == i);
        CHECK(((job->words()[0] >> 25) & 1u) == 1u);
        CHECK(((job->words()[0] >> 27) & 3u) == i);
        CHECK(((job->words()[2] >> 16) & 0x3fffu) == i + 1);
        batch.push_back(job.get());
        jobs.push_back(std::move(job));
    }
    CHECK(engine.available_slots() == 251);
    CHECK(model.ddp_access_bases == expected_ddp_accesses());
    CHECK(jobs.front()->set_size(64) == PHAL_STATUS_INVALID);
    CHECK(!hal.device_mem_alloc(HQC_SRAM_1, 4, 0).valid());
    CHECK(engine.submit(batch) == PHAL_STATUS_OK);
    CHECK(model.fetch_addresses == std::vector<uint32_t>({
        0x30080200u, 0x30080400u, 0x30080600u, 0x30080800u}));
    CHECK(model.selected_cores == std::vector<uint32_t>({0, 1, 0, 1}));
    CHECK(model.done[1] == 1 && model.done[2] == 1 &&
          model.done[3] == 1 && model.done[4] == 1);
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        const auto completion = (*it)->poll_completion();
        if (completion != PHAL_STATUS_OK) {
            throw std::runtime_error(
                "poll_completion status=" +
                std::to_string(static_cast<int>(completion)) +
                " sync_flag=" + std::to_string((*it)->sync_flag()));
        }
        CHECK((*it)->state() == DmaJobState::Completed);
    }
    CHECK(engine.available_slots() == HqcDmaEngine::DESCRIPTOR_USABLE_SLOT_COUNT);
}

void synchronous_execute_protocol()
{
    HalContext hal;
    bind(hal);
    auto scope = hal.begin_testcase();
    CHECK(scope);
    Model model;
    install(hal);
    auto host = hal.host_mem_alloc(32);
    CHECK(host.valid());
    HqcDmaEngine engine(hal);

    DmaDescriptor job(engine, DmaProfile::H2D);
    CHECK(job.set_src(host) == PHAL_STATUS_OK);
    CHECK(job.set_dst(DmaMemory::Dmem, 1, 0, 0) == PHAL_STATUS_OK);
    CHECK(job.set_size(32) == PHAL_STATUS_OK);
    CHECK(job.execute(10) == PHAL_STATUS_OK);
    CHECK(job.state() == DmaJobState::Completed);
    CHECK(engine.available_slots() == HqcDmaEngine::DESCRIPTOR_USABLE_SLOT_COUNT);
    CHECK(model.fetch_addresses == std::vector<uint32_t>({0x30080200u}));
    CHECK(model.selected_cores == std::vector<uint32_t>({0}));
    CHECK(model.ddp_access_bases == expected_ddp_accesses());
}

void slot_pressure_contract()
{
    HalContext hal;
    bind(hal);
    auto scope = hal.begin_testcase();
    CHECK(scope);
    Model model;
    install(hal);
    auto host = hal.host_mem_alloc(32);
    CHECK(host.valid());
    HqcDmaEngine engine(hal);

    std::vector<std::unique_ptr<DmaDescriptor>> jobs;
    jobs.reserve(HqcDmaEngine::DESCRIPTOR_USABLE_SLOT_COUNT);
    for (uint32_t i = 0;
         i < HqcDmaEngine::DESCRIPTOR_USABLE_SLOT_COUNT; ++i) {
        auto job = std::make_unique<DmaDescriptor>(engine, DmaProfile::H2S);
        CHECK(job->set_src(host) == PHAL_STATUS_OK);
        CHECK(job->set_dst(DmaMemory::Smem, 2, 1, 0) == PHAL_STATUS_OK);
        CHECK(job->set_size(32) == PHAL_STATUS_OK);
        const auto fill = job->fill_descriptor();
        if (fill != PHAL_STATUS_OK) {
            throw std::runtime_error(
                "fill_descriptor status=" +
                std::to_string(static_cast<int>(fill)) +
                " slot=" + std::to_string(i));
        }
        CHECK(job->slot_id() == i + 1 && job->sync_flag() == i + 1);
        jobs.push_back(std::move(job));
    }
    CHECK(engine.available_slots() == 0);

    DmaDescriptor overflow(engine, DmaProfile::H2S);
    CHECK(overflow.set_src(host) == PHAL_STATUS_OK);
    CHECK(overflow.set_dst(DmaMemory::Smem, 2, 1, 0) == PHAL_STATUS_OK);
    CHECK(overflow.set_size(32) == PHAL_STATUS_OK);
    CHECK(overflow.fill_descriptor() == PHAL_STATUS_ERROR);

    CHECK(jobs.front()->submit() == PHAL_STATUS_OK);
    CHECK(jobs.front()->poll_completion() == PHAL_STATUS_OK);
    CHECK(engine.available_slots() == 1);
    CHECK(overflow.fill_descriptor() == PHAL_STATUS_OK);
    CHECK(overflow.slot_id() == 1 && overflow.sync_flag() == 1);
}
}

int main()
{
    test_host_allocations_enabled = true;
    try {
        alignment_contracts();
        synchronous_execute_protocol();
        async_slot_protocol();
        slot_pressure_contract();
        release_test_host_storage();
        std::cout << "DMA async contract passed (software transport only; no hardware validation)\n";
        return 0;
    } catch (const std::exception& error) {
        release_test_host_storage();
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
