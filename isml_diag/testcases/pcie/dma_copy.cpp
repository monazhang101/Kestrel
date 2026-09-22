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
// HQC command construction and submission.
TestStatus PCIeModule::dma_data_transfer(TestInfo& ti)
{
    // FW length uses 32-byte units; device offsets use 4-byte words.
    constexpr uint64_t DMA_ALIGNMENT = 32;
    constexpr uint64_t HOST_OFFSET = 0;

    auto& ctx = ctx_;
    auto& logger = *ti.logger;

    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PCIe implementation is not bound");
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
    const bool host_case = direction == "h2d" || direction == "d2h";
    const bool imem_case = direction == "d2i2d";
    const bool smem_case = direction == "d2s2d";
    const auto size_bytes = get_u64(ti.args, "size_bytes");
    const auto pattern = get_string(ti.args, "pattern");
    const auto timeout_ms = get_u64(ti.args, "timeout_ms");
    const auto device_offset = get_u64(ti.args, "device_offset");
    const auto return_offset = get_u64(ti.args, "return_offset");
    const auto intermediate_offset = get_u64(ti.args, "intermediate_offset");

    if ((!host_case && !imem_case && !smem_case) ||
        timeout_ms == 0 || size_bytes == 0 || (host_case && size_bytes > HOST_DMA_MAX_SIZE_BYTES) ||
        (size_bytes % DMA_ALIGNMENT) != 0 ||
        (device_offset % 4) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "unsupported direction or invalid range: size must be a nonzero multiple of 32 (host <= 128MiB); device offset must be a multiple of 4; timeout must be nonzero");
        }
        return PHAL_STATUS_INVALID;
    }
    if (!common::mmio::is_valid_range(ctx.dmem_size,
                                      device_offset,
                                      static_cast<size_t>(size_bytes))) {
        if (ti.logger != nullptr) {
            ti.logger->error("DMA range is outside the diagnostic DMEM window");
        }
        return PHAL_STATUS_INVALID;
    }

    if (!host_case) {
        // Conservative bring-up coverage from the supplied FW tests, not the
        // physical capacity or a decoded CSR range. TODO: replace with confirmed
        // platform scratch ranges when the range-register semantics are known.
        const uint64_t scratch_limit = imem_case ? 256 : 128;
        if (intermediate_offset % 4 != 0 || return_offset % 4 != 0 ||
            intermediate_offset > scratch_limit ||
            size_bytes > scratch_limit - intermediate_offset ||
            !common::mmio::is_valid_range(ctx.dmem_size, return_offset,
                                          static_cast<size_t>(size_bytes))) {
            if (ti.logger != nullptr) {
                ti.logger->error("invalid round-trip range: IMEM coverage [0,256), SMEM [0,128); use --size-bytes 128; offsets must be 4-byte aligned");
            }
            return PHAL_STATUS_INVALID;
        }
        // Separate A/B prevents unchanged source data from passing comparison.
        if (device_offset < return_offset + size_bytes &&
            return_offset < device_offset + size_bytes) {
            if (ti.logger != nullptr) {
                ti.logger->error("DMEM source and return ranges overlap");
            }
            return PHAL_STATUS_INVALID;
        }
    }

    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    if (expected.empty()) {
        logger.error("unsupported DMA pattern=" + pattern);
        return PHAL_STATUS_INVALID;
    }
    std::vector<uint8_t> actual(expected.size()), poison(expected.size());
    // A DMA that leaves its destination untouched must fail even for zero patterns.
    for (size_t i = 0; i < expected.size(); ++i) poison[i] = static_cast<uint8_t>(~expected[i]);

    logger.info("DMA request: direction=" + direction + " size_bytes=" + std::to_string(size_bytes) +
                " pattern=" + pattern + " device_offset=" + hex(device_offset) +
                " return_offset=" + hex(return_offset) +
                " intermediate_offset=" + hex(intermediate_offset) +
                " dmem_base=" + hex(ctx.dmem_base) +
                " timeout_ms=" + std::to_string(timeout_ms));

    // Read back setup writes before HQC submission: verify data and follow posted
    // BAR writes with a same-device MMIO read. Each API transfers the whole buffer.
    const auto prepare = [&](uint64_t offset, const std::vector<uint8_t>& data) {
        auto status = dmem_write(ctx, offset, data);
        if (status != PHAL_STATUS_OK) return status;
        status = dmem_read(ctx, offset, &actual);
        if (status != PHAL_STATUS_OK) return status;
        if (actual != data) {
            logger.error("DMEM preparation mismatch: offset=" + hex(offset));
            return PHAL_STATUS_ERROR;
        }
        return PHAL_STATUS_OK;
    };

    DmaTransferRequest request{};
    request.size_bytes = size_bytes;
    // Timeout applies to each HQC queue push/pop, not the whole round trip.
    request.timeout_ms = timeout_ms;
    auto status = PHAL_STATUS_OK;
    if (!host_case) {
        status = prepare(device_offset, expected);
        if (status != PHAL_STATUS_OK) return status;
        status = prepare(return_offset, poison);
        if (status != PHAL_STATUS_OK) return status;

        request.type = imem_case ? DmaTransferType::D2I : DmaTransferType::D2S;
        request.src_offset = device_offset;
        request.dst_offset = intermediate_offset;
        status = impl_->dma_copy(ti, request);
        if (status != PHAL_STATUS_OK) return status;

        request.type = imem_case ? DmaTransferType::I2D : DmaTransferType::S2D;
        request.src_offset = intermediate_offset;
        request.dst_offset = return_offset;
        status = impl_->dma_copy(ti, request);
        if (status != PHAL_STATUS_OK) return status;

        status = dmem_read(ctx, return_offset, &actual);
        if (status != PHAL_STATUS_OK) return status;
        if (actual != expected) {
            logger.error(direction + " DMA data mismatch");
            return PHAL_STATUS_ERROR;
        }
    } else {
        auto host_buffer = ti.hal->alloc_host_dma_buffer(ctx, size_bytes);
        if (!host_buffer.valid()) {
            logger.error("host DMA allocation failed; check the matching /dev/isml_diag device");
            return PHAL_STATUS_ERROR;
        }
        const bool h2d = direction == "h2d";
        auto* host_data = static_cast<uint8_t*>(host_buffer.cpu_base()) + HOST_OFFSET;
        const auto& host_pattern = h2d ? expected : poison;
        std::memcpy(host_data, host_pattern.data(), host_pattern.size());
        status = prepare(device_offset, h2d ? poison : expected);
        if (status != PHAL_STATUS_OK) return status;

        request.type = h2d ? DmaTransferType::H2D : DmaTransferType::D2H;
        request.host_buffer = &host_buffer;
        request.src_offset = h2d ? HOST_OFFSET : device_offset;
        request.dst_offset = h2d ? device_offset : HOST_OFFSET;
        logger.info("host_dma_addr=" + hex(host_buffer.device_addr()) +
                    " address_kind=" + host_buffer.addr_kind());
        status = impl_->dma_copy(ti, request);
        if (status != PHAL_STATUS_OK) return status;

        if (h2d) {
            status = dmem_read(ctx, device_offset, &actual);
            if (status != PHAL_STATUS_OK) return status;
        }
        if (!common::pattern::compare(expected.data(), h2d ? actual.data() : host_data, expected.size())) {
            logger.error(direction + " DMA data mismatch");
            return PHAL_STATUS_ERROR;
        }
    }
    logger.info("DMA data verified: direction=" + direction + " size_bytes=" + std::to_string(size_bytes));
    return PHAL_STATUS_OK;
}
