#include "diag/modules/PCIeModule.h"

#include "diag/core/Common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using common::args::get_string;
using common::args::get_u64;
using common::format::hex;

// Host transfers and device-only round trips share the single-command DMA API.
// The testcase owns pattern preparation and comparison; the product impl owns
// HQC command construction and submission. IMEM/SMEM verification uses DMEM as
// a CPU-visible staging or readback endpoint.
TestStatus PCIeModule::dma_data_transfer(TestInfo& ti)
{
    // FW length uses 32-byte units; device offsets use 4-byte words.
    constexpr uint64_t DMA_ALIGNMENT = 32;
    constexpr uint64_t HOST_OFFSET = 0;

    auto& logger = *ti.logger;
    const auto* dmem = find_df_region(ti.hal->memory_regions().df, DMEM);

    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PCIe implementation is not bound");
    }
    if (dmem == nullptr || !dmem->supported) {
        return make_unimplemented_status(ti, "DMEM region is not supported");
    }

    // TestTarget has already supplied registered defaults and validated each
    // argument's format and product-policy range. Cross-argument constraints
    // and temporary I/S scratch bounds are checked below.
    const auto direction = get_string(ti.args, "direction");
    if (direction == "d2v2d") {
        // VMEM is unavailable on the current platform. Keep an explicit
        // placeholder, without preparing memory or submitting DMA commands.
        return make_unimplemented_status(ti, "D2V2D is reserved; VMEM DMA is not enabled");
    }
    const bool h2d = direction == "h2d";
    const bool d2h = direction == "d2h";
    const bool h2i = direction == "h2i";
    const bool i2h = direction == "i2h";
    const bool h2s = direction == "h2s";
    const bool s2h = direction == "s2h";
    const bool host_to_internal = h2i || h2s;
    const bool internal_to_host = i2h || s2h;
    const bool host_case = h2d || d2h || host_to_internal || internal_to_host;
    const bool round_trip = direction == "d2i2d" || direction == "d2s2d";
    const bool imem_case = direction == "d2i2d" || h2i || i2h;
    const bool smem_case = direction == "d2s2d" || h2s || s2h;
    const bool internal_case = imem_case || smem_case;
    const auto size_bytes = get_u64(ti.args, "size_bytes");
    const auto pattern = get_string(ti.args, "pattern");
    const auto timeout_ms = get_u64(ti.args, "timeout_ms");
    const auto intermediate_offset = get_u64(ti.args, "intermediate_offset");

    if ((!host_case && !round_trip) ||
        timeout_ms == 0 || size_bytes == 0 || (host_case && size_bytes > HOST_DMA_MAX_SIZE_BYTES) ||
        (size_bytes % DMA_ALIGNMENT) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "unsupported direction or invalid range: size must be a nonzero multiple of 32 (host <= 128MiB); timeout must be nonzero");
        }
        return PHAL_STATUS_INVALID;
    }
    if (internal_case) {
        // This is a temporary bring-up test window, not a capacity declaration.
        // TODO: Align the usable address ranges and sizes for each internal
        // memory with FW/HW owners, including SRAM space reserved by FW.
        constexpr uint64_t scratch_limit = 256;
        if (intermediate_offset % 4 != 0 ||
            intermediate_offset > scratch_limit ||
            size_bytes > scratch_limit - intermediate_offset) {
            if (ti.logger != nullptr) {
                ti.logger->error(
                    "invalid internal-memory range: temporary IMEM/SMEM test window [0,256); offsets must be 4-byte aligned");
            }
            return PHAL_STATUS_INVALID;
        }
    }

    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    if (expected.empty()) {
        logger.error("unsupported DMA pattern=" + pattern);
        return PHAL_STATUS_INVALID;
    }
    auto device_buffer = ti.hal->device_mem_alloc(DMEM, size_bytes);
    auto return_buffer = round_trip ? ti.hal->device_mem_alloc(DMEM, size_bytes) : MemBuffer{};
    if (!device_buffer.valid() || (round_trip && !return_buffer.valid()))
        return PHAL_STATUS_ERROR;
    const auto device_offset = device_buffer.offset();
    const auto return_offset = return_buffer.offset();

    std::vector<uint8_t> actual(expected.size());

    logger.info("DMA request: direction=" + direction + " size_bytes=" + std::to_string(size_bytes) +
                " pattern=" + pattern + " device_offset=" + hex(device_offset) +
                " return_offset=" + hex(return_offset) +
                " intermediate_offset=" + hex(intermediate_offset) +
                " dmem_base=" + hex(dmem->base) +
                " timeout_ms=" + std::to_string(timeout_ms));

    // Read back setup writes before HQC submission: verify data and follow posted
    // BAR writes with a same-device MMIO read. Each API transfers the whole buffer.
    const auto prepare = [&](MemBuffer& buffer, const std::vector<uint8_t>& data) {
        auto status = mem_write( buffer, 0, data);
        if (status != PHAL_STATUS_OK) return status;
        status = mem_read( buffer, 0, &actual);
        if (status != PHAL_STATUS_OK) return status;
        if (actual != data) {
            logger.error("DMEM preparation mismatch: offset=" + hex(buffer.offset()));
            return PHAL_STATUS_ERROR;
        }
        return PHAL_STATUS_OK;
    };

    DmaTransferRequest request{};
    request.size_bytes = size_bytes;
    // Timeout applies to each HQC queue push/pop, not the whole round trip.
    request.timeout_ms = timeout_ms;
    const auto submit = [&](DmaTransferType type, DmaBuffer* host_buffer,
                            uint64_t src_offset, uint64_t dst_offset) {
        request.type = type;
        request.host_buffer = host_buffer;
        request.src_offset = src_offset;
        request.dst_offset = dst_offset;
        return impl_->dma_copy(ti, request);
    };
    const auto keep_device_buffers = [&] {
        device_buffer.keep_allocated();
        return_buffer.keep_allocated();
    };
    auto status = PHAL_STATUS_OK;
    if (round_trip) {
        status = prepare(device_buffer, expected);
        if (status != PHAL_STATUS_OK) return status;

        status = submit(imem_case ? DmaTransferType::D2I : DmaTransferType::D2S,
                        nullptr, device_offset, intermediate_offset);
        if (status != PHAL_STATUS_OK) {
            keep_device_buffers();
            return status;
        }

        status = submit(imem_case ? DmaTransferType::I2D : DmaTransferType::S2D,
                        nullptr, intermediate_offset, return_offset);
        if (status != PHAL_STATUS_OK) {
            keep_device_buffers();
            return status;
        }

        status = mem_read( return_buffer, 0, &actual);
        if (status != PHAL_STATUS_OK) return status;
        if (actual != expected) {
            logger.error(direction + " DMA data mismatch");
            return PHAL_STATUS_ERROR;
        }
    } else {
        auto host_buffer = ti.hal->host_mem_alloc(size_bytes);
        if (!host_buffer.valid()) {
            logger.error("host DMA allocation failed; see the preceding HAL error");
            return PHAL_STATUS_ERROR;
        }
        auto* host_data = static_cast<uint8_t*>(host_buffer.cpu_base()) + HOST_OFFSET;
        logger.info("host_dma_addr=" + hex(host_buffer.device_addr()) +
                    " address_kind=" + host_buffer.addr_kind());

        if (h2d || host_to_internal) {
            std::memcpy(host_data, expected.data(), expected.size());
        } else if (d2h || internal_to_host) {
            status = prepare(device_buffer, expected);
            if (status != PHAL_STATUS_OK) return status;
        }

        if (h2d) {
            status = submit(DmaTransferType::H2D, &host_buffer,
                            HOST_OFFSET, device_offset);
        } else if (d2h) {
            status = submit(DmaTransferType::D2H, &host_buffer,
                            device_offset, HOST_OFFSET);
        } else if (host_to_internal) {
            status = submit(imem_case ? DmaTransferType::H2I : DmaTransferType::H2S,
                            &host_buffer, HOST_OFFSET, intermediate_offset);
            if (status == PHAL_STATUS_OK) {
                status = submit(imem_case ? DmaTransferType::I2D : DmaTransferType::S2D,
                                nullptr, intermediate_offset, device_offset);
            }
        } else {
            status = submit(imem_case ? DmaTransferType::D2I : DmaTransferType::D2S,
                            nullptr, device_offset, intermediate_offset);
            if (status == PHAL_STATUS_OK) {
                status = submit(imem_case ? DmaTransferType::I2H : DmaTransferType::S2H,
                                &host_buffer, intermediate_offset, HOST_OFFSET);
            }
        }
        if (status != PHAL_STATUS_OK) {
            keep_device_buffers();
            return status;
        }

        if (h2d || host_to_internal) {
            status = mem_read(device_buffer, 0, &actual);
            if (status != PHAL_STATUS_OK) return status;
        }
        const auto* observed = (h2d || host_to_internal) ? actual.data() : host_data;
        if (!common::pattern::compare(expected.data(), observed, expected.size())) {
            logger.error(direction + " DMA data mismatch");
            return PHAL_STATUS_ERROR;
        }
    }
    logger.info("DMA data verified: direction=" + direction + " size_bytes=" + std::to_string(size_bytes));
    return PHAL_STATUS_OK;
}
