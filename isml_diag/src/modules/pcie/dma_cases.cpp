#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/core/DevMem.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// End-to-end feasibility case for the host DMA allocation + HQC command path.
// The testcase owns pattern preparation and comparison; the product impl owns
// HQC command construction and submission.
TestStatus PCIeModule::pcie_dma_data_transfer(TestInfo& ti)
{
    // HQC DMA command ABI requires source, destination, and transfer length to
    // be 32-byte aligned. The host allocation limit is owned by HalContext.
    constexpr uint64_t DMA_ALIGNMENT = 32;
    constexpr uint64_t HOST_OFFSET = 0;
    constexpr uint64_t DEFAULT_TIMEOUT_MS = 1000;
    constexpr uint64_t DEFAULT_DEVICE_OFFSET = 0x10200;

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

    const auto direction = common::args::get_string(ti.args, "direction");
    const auto size_bytes = common::args::get_u64(ti.args, "size_bytes");
    const auto pattern = common::args::get_string(ti.args, "pattern");
    // One second is the bring-up default for SQ availability plus FW command
    // completion; callers can override it through policy/test arguments.
    const auto timeout_ms = common::args::get_u64(ti.args, "timeout_ms", DEFAULT_TIMEOUT_MS);
    // Bring-up default chosen to avoid the low DMEM scratch prefix; replace it
    // with a platform-policy value once FW publishes the owned test region.
    const auto device_offset = common::args::get_u64(
        ti.args, "device_offset", DEFAULT_DEVICE_OFFSET);

    if (ti.hal == nullptr) {
        if (ti.logger != nullptr) {
            ti.logger->error("HAL context is not available");
        }
        return TestStatus::ERROR;
    }
    if ((direction != "h2d" && direction != "d2h") ||
        size_bytes == 0 || size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
        (size_bytes % DMA_ALIGNMENT) != 0 ||
        (device_offset % DMA_ALIGNMENT) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "direction must be h2d/d2h and DMA range must be 32-byte aligned and <= 128MiB");
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

    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    if (expected.empty()) {
        if (ti.logger != nullptr) {
            ti.logger->error("unsupported DMA pattern=" + pattern);
        }
        return TestStatus::INVALID;
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

    DmaTransferRequest request;
    request.host_buffer = &host_buffer;
    request.host_offset = HOST_OFFSET;
    request.device_offset = device_offset;
    request.size_bytes = size_bytes;
    request.timeout_ms = timeout_ms;

    if (ti.logger != nullptr) {
        ti.logger->info("PCIe DMA begin direction=" + direction +
                        " size=" + std::to_string(size_bytes) +
                        " pattern=" + pattern +
                        " dmem_base=" + std::to_string(DMEM_WINDOW.target_addr) +
                        " dmem_offset=" + std::to_string(device_offset));
    }

    std::string access_error;
    if (direction == "h2d") {
        std::memcpy(host_base + HOST_OFFSET, expected.data(), expected.size());
        const auto dma_status = impl_->dma_copy_h2d(ti, request);
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
        if (ti.logger != nullptr) {
            ti.logger->info("H2D DMA data compare passed");
        }
        return TestStatus::OK;
    }

    if (!common::devmem::write(ti, ctx_, DMEM_WINDOW, device_offset,
                               expected.data(), expected.size(), &access_error)) {
        if (ti.logger != nullptr) {
            ti.logger->error("D2H source write failed: " + access_error);
        }
        return TestStatus::ERROR;
    }
    std::memset(host_base + HOST_OFFSET, 0, static_cast<size_t>(size_bytes));
    const auto dma_status = impl_->dma_copy_d2h(ti, request);
    if (dma_status != TestStatus::OK) {
        return dma_status;
    }

    const bool match = common::pattern::compare(expected.data(),
                                                 host_base + HOST_OFFSET,
                                                 static_cast<size_t>(size_bytes));
    if (!match) {
        if (ti.logger != nullptr) {
            ti.logger->error("D2H DMA data mismatch");
        }
        return TestStatus::ERROR;
    }
    if (ti.logger != nullptr) {
        ti.logger->info("D2H DMA data compare passed");
    }
    return TestStatus::OK;
}
