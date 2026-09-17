#include "diag/modules/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/core/DevMem.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// Host transfers and device-only round trips share the single-command DMA API.
// The testcase owns pattern preparation and comparison; the product impl owns
// HQC command construction and submission.
TestStatus PCIeModule::dma_data_transfer(TestInfo& ti)
{
    // FW length uses 32-byte units; device offsets use 4-byte words.
    constexpr uint64_t DMA_ALIGNMENT = 32;
    constexpr uint64_t HOST_OFFSET = 0;

    // Temporary Atlas bring-up scratch mapping; confirm these values against
    // the versioned FW/platform memory map before treating them as product ABI:
    //   BAR4 aperture 0, identity 0 maps 256 MiB of DMEM starting at
    //   device address 0x10000000 into BAR offset 0. device_offset below is an
    //   offset relative to this DMEM base, not an absolute device address.
    constexpr auto DMEM_WINDOW = [] {
        common::devmem::Window window;
        window.bar_index = 4;
        window.aperture_index = 0;
        window.identity = 0;
        window.target_addr = 0x10000000;
        window.size = 0x10000000;
        window.bar_offset = 0;
        return window;
    }();

    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PCIe implementation is not bound");
    }

    // TestTarget has already supplied registered defaults and validated each
    // argument's format and product-policy range. Cross-argument constraints
    // and temporary I/S scratch bounds are checked below.
    const auto direction = common::args::get_string(ti.args, "direction");
    if (direction == "d2v2d") {
        // VMEM is unavailable on the current platform. Keep an explicit
        // placeholder, without preparing memory or submitting DMA commands.
        return make_unimplemented_status(ti, "D2V2D is reserved; VMEM DMA is not enabled");
    }
    const bool host_case = direction == "h2d" || direction == "d2h";
    const bool imem_case = direction == "d2i2d";
    const bool smem_case = direction == "d2s2d";
    const auto size_bytes = common::args::get_u64(ti.args, "size_bytes");
    const auto pattern = common::args::get_string(ti.args, "pattern");
    const auto timeout_ms = common::args::get_u64(ti.args, "timeout_ms");
    const auto device_offset = common::args::get_u64(ti.args, "device_offset");
    const auto return_offset = common::args::get_u64(ti.args, "return_offset");
    const auto intermediate_offset = common::args::get_u64(ti.args, "intermediate_offset");

    if (ti.hal == nullptr) {
        if (ti.logger != nullptr) {
            ti.logger->error("HAL context is not available");
        }
        return TestStatus::ERROR;
    }
    if ((!host_case && !imem_case && !smem_case) ||
        size_bytes == 0 || (host_case && size_bytes > HOST_DMA_MAX_SIZE_BYTES) ||
        (size_bytes % DMA_ALIGNMENT) != 0 ||
        (device_offset % 4) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "unsupported direction or invalid range: size must be a nonzero multiple of 32 (host <= 128MiB); device offset must be a multiple of 4");
        }
        return TestStatus::INVALID;
    }
    if (!common::mmio::is_valid_range(DMEM_WINDOW.size,
                                      device_offset,
                                      static_cast<size_t>(size_bytes))) {
        if (ti.logger != nullptr) {
            ti.logger->error("DMA range is outside the diagnostic DMEM window");
        }
        return TestStatus::INVALID;
    }

    if (!host_case) {
        // Conservative bring-up coverage from the supplied FW tests, not the
        // physical capacity or a decoded CSR range. TODO: replace with confirmed
        // platform scratch ranges when the range-register semantics are known.
        const uint64_t scratch_limit = imem_case ? 256 : 128;
        if (intermediate_offset % 4 != 0 || return_offset % 4 != 0 ||
            intermediate_offset > scratch_limit ||
            size_bytes > scratch_limit - intermediate_offset ||
            !common::mmio::is_valid_range(DMEM_WINDOW.size, return_offset,
                                          static_cast<size_t>(size_bytes))) {
            if (ti.logger != nullptr) {
                ti.logger->error("invalid round-trip range: IMEM coverage [0,256), SMEM [0,128); use --size-bytes 128; offsets must be 4-byte aligned");
            }
            return TestStatus::INVALID;
        }
        // Separate A/B prevents unchanged source data from passing comparison.
        if (device_offset < return_offset + size_bytes &&
            return_offset < device_offset + size_bytes) {
            if (ti.logger != nullptr) {
                ti.logger->error("DMEM source and return ranges overlap");
            }
            return TestStatus::INVALID;
        }
    }

    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    if (expected.empty()) {
        if (ti.logger != nullptr) {
            ti.logger->error("unsupported DMA pattern=" + pattern);
        }
        return TestStatus::INVALID;
    }

    // Poison the destination even for the all-zero pattern, so a transfer
    // that leaves it untouched cannot pass merely because memory was zeroed.
    std::vector<uint8_t> poison(expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        poison[i] = static_cast<uint8_t>(~expected[i]);
    }

    if (!host_case) {
        std::vector<uint8_t> actual(expected.size());
        std::string error;
        if (ti.logger != nullptr) {
            std::ostringstream message;
            message << "DMA round-trip request"
                    << "\n       direction           = " << direction
                    << "\n       size_bytes          = " << size_bytes
                    << "\n       pattern             = " << pattern
                    << "\n       dmem_source_offset  = 0x" << std::hex << device_offset
                    << "\n       intermediate_offset = 0x" << intermediate_offset
                    << "\n       dmem_return_offset  = 0x" << return_offset;
            ti.logger->info(message.str());
        }
        // Read back each setup write before DMA: verify preparation and issue
        // a same-device MMIO read after the posted BAR writes.
        const auto prepare = [&](uint64_t offset, const std::vector<uint8_t>& data) {
            if (!common::devmem::write(ti, ctx_, DMEM_WINDOW, offset,
                                       data.data(), data.size(), &error) ||
                !common::devmem::read(ti, ctx_, DMEM_WINDOW, offset,
                                      actual.data(), actual.size(), &error)) {
                return false;
            }
            if (!common::pattern::compare(data.data(), actual.data(), data.size())) {
                error = "DMEM preparation readback mismatch";
                return false;
            }
            return true;
        };
        if (!prepare(device_offset, expected) || !prepare(return_offset, poison)) {
            if (ti.logger != nullptr) {
                ti.logger->error(direction + " preparation failed: " + error);
            }
            return TestStatus::ERROR;
        }
        DmaTransferRequest request{};
        request.type = imem_case ? DmaTransferType::D2I : DmaTransferType::D2S;
        request.src_offset = device_offset;
        request.dst_offset = intermediate_offset;
        request.size_bytes = size_bytes;
        // Existing timeout applies to each queue push/pop, not to the total
        // wall-clock time of both commands in this round trip.
        request.timeout_ms = timeout_ms;
        auto status = impl_->dma_copy(ti, request);
        if (status != TestStatus::OK) {
            return status;
        }
        request.type = imem_case ? DmaTransferType::I2D : DmaTransferType::S2D;
        request.src_offset = intermediate_offset;
        request.dst_offset = return_offset;
        status = impl_->dma_copy(ti, request);
        if (status != TestStatus::OK) {
            return status;
        }
        if (!common::devmem::read(ti, ctx_, DMEM_WINDOW, return_offset,
                                  actual.data(), actual.size(), &error)) {
            if (ti.logger != nullptr) {
                ti.logger->error(direction + " readback failed: " + error);
            }
            return TestStatus::ERROR;
        }
        if (!common::pattern::compare(expected.data(), actual.data(), actual.size())) {
            // Final mismatch alone cannot identify which direction failed.
            if (ti.logger != nullptr) {
                ti.logger->error(direction + " DMA data mismatch");
            }
            return TestStatus::ERROR;
        }
        if (ti.logger != nullptr) {
            ti.logger->info("DMA data verification\n       direction      = " + direction +
                            "\n       size_bytes     = " + std::to_string(size_bytes) +
                            "\n       compare_status = match");
        }
        return TestStatus::OK;
    }

    auto host_buffer = ti.hal->alloc_host_dma_buffer(ctx_, size_bytes);
    if (!host_buffer.valid()) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "host DMA allocation failed; check that the matching /dev/isml_diag device is available");
        }
        return TestStatus::ERROR;
    }
    auto* host_base = static_cast<uint8_t*>(host_buffer.cpu_base());

    DmaTransferRequest request{};
    request.type = direction == "h2d" ? DmaTransferType::H2D : DmaTransferType::D2H;
    request.host_buffer = &host_buffer;
    request.src_offset = direction == "h2d" ? HOST_OFFSET : device_offset;
    request.dst_offset = direction == "h2d" ? device_offset : HOST_OFFSET;
    request.size_bytes = size_bytes;
    request.timeout_ms = timeout_ms;

    if (ti.logger != nullptr) {
        std::ostringstream message;
        message << "PCIe DMA request"
                << "\n       direction        = " << direction
                << "\n       size_bytes       = " << size_bytes
                << "\n       pattern          = " << pattern
                << "\n       host_dma_addr    = 0x" << std::hex
                << host_buffer.device_addr()
                << "\n       address_kind     = " << host_buffer.addr_kind()
                << "\n       dmem_window_base = 0x" << DMEM_WINDOW.target_addr
                << "\n       dmem_offset      = 0x" << device_offset
                << "\n       dmem_target_addr = 0x"
                << (DMEM_WINDOW.target_addr + device_offset)
                << "\n       timeout_ms       = " << std::dec << timeout_ms;
        ti.logger->info(message.str());
    }

    std::string access_error;
    if (direction == "h2d") {
        std::memcpy(host_base + HOST_OFFSET, expected.data(), expected.size());
        if (!common::devmem::write(ti, ctx_, DMEM_WINDOW, device_offset,
                                   poison.data(), poison.size(), &access_error)) {
            if (ti.logger != nullptr) {
                ti.logger->error("H2D destination preparation failed: " + access_error);
            }
            return TestStatus::ERROR;
        }
        // Read back destination preparation before submitting the command.
        std::vector<uint8_t> prepared(poison.size());
        if (!common::devmem::read(ti, ctx_, DMEM_WINDOW, device_offset,
                                  prepared.data(), prepared.size(), &access_error) ||
            !common::pattern::compare(poison.data(), prepared.data(), prepared.size())) {
            if (ti.logger != nullptr) {
                ti.logger->error("H2D destination readback failed: " + access_error);
            }
            return TestStatus::ERROR;
        }
        const auto dma_status = impl_->dma_copy(ti, request);
        if (dma_status != TestStatus::OK) {
            return dma_status;
        }

        std::vector<uint8_t> actual(static_cast<size_t>(size_bytes));
        if (!common::devmem::read(ti, ctx_, DMEM_WINDOW, device_offset,
                                  actual.data(), actual.size(), &access_error)) {
            if (ti.logger != nullptr) {
                ti.logger->error("H2D readback failed: " + access_error);
            }
            return TestStatus::ERROR;
        }
        const bool match = common::pattern::compare(expected.data(),
                                                     actual.data(),
                                                     actual.size());
        if (!match) {
            if (ti.logger != nullptr) {
                ti.logger->error("H2D DMA data mismatch");
            }
            return TestStatus::ERROR;
        }
    } else {
        if (!common::devmem::write(ti, ctx_, DMEM_WINDOW, device_offset,
                                   expected.data(), expected.size(), &access_error)) {
            if (ti.logger != nullptr) {
                ti.logger->error("D2H source write failed: " + access_error);
            }
            return TestStatus::ERROR;
        }
        std::memcpy(host_base + HOST_OFFSET, poison.data(), poison.size());
        std::vector<uint8_t> prepared(expected.size());
        if (!common::devmem::read(ti, ctx_, DMEM_WINDOW, device_offset,
                                  prepared.data(), prepared.size(), &access_error) ||
            !common::pattern::compare(expected.data(), prepared.data(), prepared.size())) {
            if (ti.logger != nullptr) {
                ti.logger->error("D2H source readback failed: " + access_error);
            }
            return TestStatus::ERROR;
        }
        const auto dma_status = impl_->dma_copy(ti, request);
        if (dma_status != TestStatus::OK) {
            return dma_status;
        }

        const bool match = common::pattern::compare(
            expected.data(),
            host_base + HOST_OFFSET,
            static_cast<size_t>(size_bytes));
        if (!match) {
            if (ti.logger != nullptr) {
                ti.logger->error("D2H DMA data mismatch");
            }
            return TestStatus::ERROR;
        }
    }

    if (ti.logger != nullptr) {
        ti.logger->info(
            "DMA data verification\n"
            "       direction      = " + direction +
            "\n       size_bytes     = " + std::to_string(size_bytes) +
            "\n       compare_status = match");
    }
    return TestStatus::OK;
}
