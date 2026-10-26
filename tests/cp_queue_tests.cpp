#include "diag/implementer/HqcCpQueue.h"
#include "diag/implementer/Implementer.h"

#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

extern bool test_host_allocations_enabled;
void release_test_host_storage();

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace {
// Scripted PHAL transport, deliberately independent of production header
// offsets. It checks the host protocol, not RTL or real DMA data movement.
constexpr uintptr_t CSR = 0x36050000;
struct Model {
    static Model* active;
    uint32_t slot;
    std::array<uint32_t, 8> remaps{};
    uint32_t q128 = 1;
    std::array<uint32_t, 12> registers{};
    std::array<uint32_t, 12> submitted{};
    std::vector<std::pair<uintptr_t, uint32_t>> writes;
    bool stall = false;
    bool fail_doorbell = false;
    bool fail_completion_read = false;
    bool stall_cleanup = false;
    bool ignore_src_write = false;
    bool config_verified_before_kick = false;
    unsigned verification_reads = 0;
    unsigned kicks = 0;
    explicit Model(CpQueueId queue = CpQueueId::Mem3)
        : slot(static_cast<uint32_t>(queue))
    {
        // Exercise runtime placement rather than the default remap.
        remap() = (memory() ? 0x08008000 : 0x02008000) + index() * 0x1000;
        registers[3] = 16;
        active = this;
    }
    bool memory() const { return slot >= 4; }
    uint32_t index() const { return slot & 3u; }
    uint32_t& remap() { return remaps[slot]; }
    uint32_t remap() const { return remaps[slot]; }
    uintptr_t canonical() const {
        return CSR + (memory() ? 0xd8 : 0x19c) + index() * 0x30;
    }
    uintptr_t alias() const { return 0x10000000 + remap(); }
    static int remap_slot(uintptr_t address) {
        if (address >= CSR && address < CSR + 0x10 && (address - CSR) % 4 == 0)
            return 4 + static_cast<int>((address - CSR) / 4);
        if (address >= CSR + 0x10 && address < CSR + 0x20 &&
            (address - CSR) % 4 == 0)
            return static_cast<int>((address - CSR - 0x10) / 4);
        return -1;
    }
    int index(uintptr_t address) const {
        if (address >= canonical() && address < canonical() + 0x30)
            return static_cast<int>((address - canonical()) / 4);
        if (remap() && address >= alias() && address < alias() + 0x30)
            return static_cast<int>((address - alias()) / 4);
        return -1;
    }
    static phal_status_t read(env_t* env, uintptr_t offset, uint32_t* value) {
        auto& m = *active;
        const auto addr = env->base + offset;
        const auto remap_index = remap_slot(addr);
        if (remap_index >= 0) {
            *value = m.remaps[static_cast<size_t>(remap_index)];
            return PHAL_STATUS_OK;
        }
        if (addr == CSR + 0x2f0) { *value = m.q128; return PHAL_STATUS_OK; }
        const auto i = m.index(addr);
        if (i < 0) return PHAL_STATUS_ERROR;
        if (i == 10 && m.kicks && m.fail_completion_read) return PHAL_STATUS_ERROR;
        *value = m.registers[i];
        if (addr >= m.alias() && addr < m.alias() + 0x30 && (i == 0 || (i >= 4 && i <= 9)))
            ++m.verification_reads;
        return PHAL_STATUS_OK;
    }
    static phal_status_t write(env_t* env, uintptr_t offset, uint32_t value) {
        auto& m = *active;
        const auto addr = env->base + offset;
        m.writes.emplace_back(addr, value);
        const auto remap_index = remap_slot(addr);
        if (remap_index >= 0) {
            m.remaps[static_cast<size_t>(remap_index)] = value;
            return PHAL_STATUS_OK;
        }
        // Fail writes to the canonical (read-only) CSR image.
        if (addr < m.alias() || addr >= m.alias() + 0x30) return PHAL_STATUS_ERROR;
        const auto i = m.index(addr);
        if (i == 1) {
            CHECK(value == 1);
            ++m.kicks;
            m.config_verified_before_kick = m.verification_reads >= 7;
            m.verification_reads = 0;
            m.submitted = m.registers;
            if (m.fail_doorbell) return PHAL_STATUS_ERROR;
            if (!m.stall) m.registers[10] += m.registers[6];
            if (m.stall || m.stall_cleanup) m.registers[3] = 15;
            return PHAL_STATUS_OK;
        }
        if (i == 5 && m.ignore_src_write) return PHAL_STATUS_OK;
        m.registers[i] = value;
        return PHAL_STATUS_OK;
    }
};
Model* Model::active = nullptr;

DeviceContext bind(HalContext& hal)
{
    DeviceContext device;
    device.bdf = "cp-protocol";
    device.tpu_type = TPUType::Atlas;
    device.memory_regions = Implementer(TPUType::Atlas).memory_regions();
    hal.bind_device(device);
    return device;
}
void install(HalContext& hal)
{
    static hw_ops_t ops{};
    ops.read = Model::read;
    ops.write = Model::write;
    CHECK(hal.with_pcie_context([&](phal_ctx_t* ctx) {
        ctx->env.ops = &ops;
        return PHAL_STATUS_OK;
    }) == PHAL_STATUS_OK);
}

void directions_and_banks()
{
    constexpr MemoryRegion banks[] = {HQC_SRAM_0, HQC_SRAM_1, HQC_SRAM_2, HQC_SRAM_3};
    for (unsigned bank = 0; bank < 4; ++bank) {
        HalContext hal;
        bind(hal);
        auto scope = hal.begin_testcase();
        CHECK(scope);
        Model model;
        model.registers[10] = model.registers[11] = 128; // Drained, nonzero baseline.
        install(hal);
        auto sram = hal.device_mem_alloc(banks[bank], 512, 1024);
        auto dmem = hal.device_mem_alloc(DMEM, 512, 0x4000);
        auto host = hal.host_mem_alloc(512);
        CHECK(sram.valid() && dmem.valid() && host.valid());
        HqcCpQueue queue(hal);
        for (unsigned direction = 0; direction < 4; ++direction) {
            TestStatus status;
            if (direction == 0) status = queue.copy(host, sram, 128, 1000, 128, 256);
            else if (direction == 1) status = queue.copy(sram, host, 128, 1000, 256, 128);
            else if (direction == 2) status = queue.copy(dmem, sram, 128, 1000, 128, 256);
            else status = queue.copy(sram, dmem, 128, 1000, 256, 128);
            CHECK(status == PHAL_STATUS_OK);
            CHECK(model.submitted[0] == direction);
            CHECK(model.config_verified_before_kick);
            const auto source = uint64_t(model.submitted[4]) | (uint64_t(model.submitted[5]) << 32);
            const auto dest = uint64_t(model.submitted[7]) | (uint64_t(model.submitted[8]) << 32);
            const uint64_t local = 0x30000000 + bank * 0x80000 + 1024 + 256;
            const uint64_t other = direction < 2 ? 0x120000080ull : 0x10004080;
            CHECK(source == (direction % 2 == 0 ? other : local));
            CHECK(dest == (direction % 2 == 0 ? local : other));
            CHECK(model.registers[10] == 0 && model.registers[11] == 0);
            CHECK(sram.valid() && dmem.valid() && host.valid());
        }
        CHECK(model.kicks == 4);
        CHECK(hal.phal_context()->env.base == PhalBridge::CHIP_RCF_BASE);
    }
}

void validation_before_io()
{
    HalContext hal, other_hal;
    bind(hal); bind(other_hal);
    auto scope = hal.begin_testcase();
    Model model;
    install(hal);
    auto sram = hal.device_mem_alloc(HQC_SRAM_0, 512);
    auto dmem = hal.device_mem_alloc(DMEM, 512);
    auto foreign = other_hal.device_mem_alloc(DMEM, 512);
    auto bad_alignment = hal.device_mem_alloc(HQC_SRAM_1, 512, 4);
    HqcCpQueue queue(hal);
    CHECK(queue.copy(dmem, sram, 0) == PHAL_STATUS_INVALID);
    CHECK(queue.copy(dmem, sram, 32) == PHAL_STATUS_INVALID);
    CHECK(queue.copy(dmem, sram, 128, 0) == PHAL_STATUS_INVALID);
    CHECK(queue.copy(dmem, sram, 128, 1000, UINT64_MAX, 0) == PHAL_STATUS_INVALID);
    CHECK(queue.copy(dmem, sram, 128, 1000, 512, 0) == PHAL_STATUS_INVALID);
    CHECK(queue.copy(dmem, bad_alignment, 128) == PHAL_STATUS_INVALID);
    CHECK(queue.copy(dmem, dmem, 128) == PHAL_STATUS_INVALID);
    CHECK(queue.copy(foreign, sram, 128) == PHAL_STATUS_INVALID);
    CHECK(model.writes.empty());
}

void queue_paths()
{
    constexpr CpQueueId queues[] = {
        CpQueueId::Comp0, CpQueueId::Comp1, CpQueueId::Comp2, CpQueueId::Comp3,
        CpQueueId::Mem0, CpQueueId::Mem1, CpQueueId::Mem2, CpQueueId::Mem3
    };
    for (const auto selected : queues) {
        HalContext hal;
        bind(hal);
        auto scope = hal.begin_testcase();
        Model model(selected);
        install(hal);
        auto host = hal.host_mem_alloc(512);
        auto sram = hal.device_mem_alloc(HQC_SRAM_0, 512);
        CHECK(host.valid() && sram.valid());
        HqcCpQueue queue(hal);
        CHECK(queue.copy(host, sram, 128, selected) == PHAL_STATUS_OK);
        CHECK(model.kicks == 1);
        CHECK(model.submitted[0] == 0);
    }
}

void failure_paths()
{
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        HalContext hal;
        auto device = bind(hal);
        auto scope = hal.begin_testcase();
        Model model;
        install(hal);
        auto sram = hal.device_mem_alloc(HQC_SRAM_0, 512);
        auto host = hal.host_mem_alloc(512);
        auto dmem = hal.device_mem_alloc(DMEM, 512);
        HqcCpQueue queue(hal);
        if (scenario == 0) model.stall = true;
        if (scenario == 1) model.fail_doorbell = true;
        if (scenario == 2) model.fail_completion_read = true;
        if (scenario == 3) model.ignore_src_write = true;
        if (scenario == 4) model.remap() = 0x1000; // Mailbox, must reject.
        if (scenario == 5) model.registers[3] = 15; // Busy, must not reconfigure.
        if (scenario == 6) model.stall_cleanup = true;
        if (scenario == 7) model.remap() = 0; // Initialize only an idle queue.
        if (scenario == 8) model.remap() = 0x08007001; // Bad remap alignment.
        if (scenario == 9) { model.remap() = 0; model.remaps[4] = 0x08007000; }
        if (scenario == 10) model.q128 = 0; // DMEM translation not enabled.
        const auto result = scenario == 10 ? queue.copy(dmem, sram, 128, 20)
                                          : queue.copy(host, sram, 128, 20);
        CHECK(hal.phal_context()->env.base == PhalBridge::CHIP_RCF_BASE);
        if (scenario == 7) {
            CHECK(result == PHAL_STATUS_OK && model.remap() == 0x08007000);
            CHECK(model.writes.front().first == CSR + 0xc);
            continue;
        }
        CHECK(result != PHAL_STATUS_OK);
        if (scenario <= 2) {
            CHECK(!host.valid() && !sram.valid());
            CHECK(device.memory->cp_queue_faulted[7]);
            CHECK(!hal.device_mem_alloc(HQC_SRAM_0, 512, 0).valid());
        } else {
            CHECK(host.valid() && sram.valid());
        }
        if (scenario == 3 || scenario == 6) CHECK(device.memory->cp_queue_faulted[7]);
        if (scenario >= 3 && scenario != 6) CHECK(model.kicks == 0);
        if (scenario == 4 || scenario == 5 || scenario >= 8) CHECK(model.writes.empty());
        if (device.memory->cp_queue_faulted[7]) {
            auto another = hal.device_mem_alloc(HQC_SRAM_2, 512);
            auto other_host = hal.host_mem_alloc(512);
            const auto writes = model.writes.size();
            HqcCpQueue reopened(hal);
            CHECK(reopened.copy(other_host, another, 128) == PHAL_STATUS_ERROR);
            CHECK(writes == model.writes.size());
        }
    }
}
}

int main()
{
    test_host_allocations_enabled = true;
    try {
        directions_and_banks();
        queue_paths();
        validation_before_io();
        failure_paths();
        release_test_host_storage();
        std::cout << "CP register protocol passed (software transport only; no hardware validation)\n";
        return 0;
    } catch (const std::exception& error) {
        release_test_host_storage();
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
