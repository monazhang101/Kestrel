#include "diag/modules/PCIeModule.h"
#include "diag/core/Common.h"

#include <cstring>
#include <vector>

namespace {
bool parse_queue(const std::string& name, CpQueueId& queue)
{
    if (name == "comp0" || name == "computer0") queue = CpQueueId::Comp0;
    else if (name == "comp1" || name == "computer1") queue = CpQueueId::Comp1;
    else if (name == "comp2" || name == "computer2") queue = CpQueueId::Comp2;
    else if (name == "comp3" || name == "computer3") queue = CpQueueId::Comp3;
    else if (name == "mem0") queue = CpQueueId::Mem0;
    else if (name == "mem1") queue = CpQueueId::Mem1;
    else if (name == "mem2") queue = CpQueueId::Mem2;
    else if (name == "mem3") queue = CpQueueId::Mem3;
    else return false;
    return true;
}
}

TestStatus PCIeModule::cp_queue_data_transfer(TestInfo& ti)
{
    using common::args::get_string;
    using common::args::get_u64;
    if (!impl_ || !ti.hal) return PHAL_STATUS_UNIMPLEMENTED;
    const auto direction = get_string(ti.args, "direction");
    const auto bytes = get_u64(ti.args, "size_bytes");
    const auto bank = get_u64(ti.args, "sram_bank");
    const auto timeout = get_u64(ti.args, "timeout_ms");
    const auto pattern = get_string(ti.args, "pattern");
    const auto queue_name = get_string(ti.args, "queue");
    CpTransferRequest request;
    if (!parse_queue(queue_name, request.queue)) {
        ti.logger->error("CP queue must be mem0..mem3 or comp0..comp3");
        return PHAL_STATUS_INVALID;
    }
    bool host = false, to_sram = false;
    if (direction == "h2sram") {
        request.type = CpTransferType::H2Sram; host = true; to_sram = true;
    } else if (direction == "sram2h") {
        request.type = CpTransferType::Sram2H; host = true;
    } else if (direction == "d2sram") {
        request.type = CpTransferType::D2Sram; to_sram = true;
    } else if (direction == "sram2d") {
        request.type = CpTransferType::Sram2D;
    } else {
        ti.logger->error("CP direction must be h2sram, sram2h, d2sram or sram2d");
        return PHAL_STATUS_INVALID;
    }
    constexpr MemoryRegion BANKS[] = {HQC_SRAM_0, HQC_SRAM_1, HQC_SRAM_2, HQC_SRAM_3};
    if (bank >= 4 || bytes == 0 || bytes % 128 || timeout == 0) {
        ti.logger->error("CP requires sram_bank=0..3, nonzero 128-byte length and nonzero timeout_ms");
        return PHAL_STATUS_INVALID;
    }
    const auto* region = find_rcf_region(ti.hal->memory_regions().rcf, BANKS[bank]);
    if (!region || !region->supported) return PHAL_STATUS_UNIMPLEMENTED;
    if (bytes > region->size) return PHAL_STATUS_INVALID;
    const auto expected = common::pattern::generate(static_cast<size_t>(bytes), pattern);
    if (expected.empty()) return PHAL_STATUS_INVALID;
    auto poison = expected;
    for (auto& byte : poison) byte ^= 0xff;
    std::vector<uint8_t> actual(expected.size());

    auto sram = ti.hal->device_mem_alloc(BANKS[bank], bytes);
    if (!sram.valid()) return PHAL_STATUS_ERROR;
    auto dmem = host ? MemBuffer{} : ti.hal->device_mem_alloc(DMEM, bytes);
    auto host_buffer = host ? ti.hal->host_mem_alloc(bytes) : DmaBuffer{};
    if ((host && !host_buffer.valid()) || (!host && !dmem.valid())) {
        ti.logger->error("CP buffer allocation failed");
        return PHAL_STATUS_ERROR;
    }
    // Read back device setup writes to flush posted writes and independently
    // verify the source/poison setup. CP completion is tested by copy(), payload
    // correctness is tested here using BAR reads or the host CPU mapping.
    const auto prepare = [&](MemBuffer& buffer, const std::vector<uint8_t>& data) {
        auto status = mem_write(buffer, 0, data);
        if (status != PHAL_STATUS_OK) return status;
        status = mem_read(buffer, 0, &actual);
        if (status != PHAL_STATUS_OK) return status;
        return actual == data ? PHAL_STATUS_OK : PHAL_STATUS_ERROR;
    };
    auto status = prepare(sram, to_sram ? poison : expected);
    if (status != PHAL_STATUS_OK) return status;
    const auto& other_data = to_sram ? expected : poison;
    if (host) {
        std::memcpy(host_buffer.cpu_base(), other_data.data(), other_data.size());
    } else {
        status = prepare(dmem, other_data);
        if (status != PHAL_STATUS_OK) return status;
    }
    request.host_buffer = host ? &host_buffer : nullptr;
    request.dmem_buffer = host ? nullptr : &dmem;
    request.sram_buffer = &sram;
    request.size_bytes = bytes;
    request.timeout_ms = timeout;
    status = impl_->cp_copy(ti, request);
    if (status != PHAL_STATUS_OK) return status;

    if (to_sram) status = mem_read(sram, 0, &actual);
    else if (host) std::memcpy(actual.data(), host_buffer.cpu_base(), actual.size());
    else status = mem_read(dmem, 0, &actual);
    if (status != PHAL_STATUS_OK) return status;
    if (actual != expected) {
        ti.logger->error("CP data mismatch: direction=" + direction);
        return PHAL_STATUS_ERROR;
    }
    ti.logger->info("CP data verified: direction=" + direction +
                    " queue=" + queue_name +
                    " sram_bank=" + std::to_string(bank) +
                    " size_bytes=" + std::to_string(bytes));
    return PHAL_STATUS_OK;
}
