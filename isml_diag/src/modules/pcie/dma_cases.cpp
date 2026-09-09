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
TestResult PCIeModule::pcie_dma_data_transfer(TestInfo& ti)
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
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
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

    DmaTransferResult dma_result;
    auto finish = [&](bool passed,
                      const std::string& compare_status,
                      const std::string& error = "") {
        if (ti.logger != nullptr) {
            if (passed) {
                ti.logger->info("PCIe DMA " + direction + " data compare passed");
            } else {
                ti.logger->error("PCIe DMA " + direction + " failed: " + error);
            }
        }
        TestMetrics metrics = {
            {"direction", direction},
            {"size_bytes", std::to_string(size_bytes)},
            {"pattern", pattern},
            {"device_offset", std::to_string(device_offset)},
            {"compare_status", compare_status},
            {"dma_completion_status", dma_result.completion_status},
            {"dma_duration_us", std::to_string(dma_result.duration_us)}
        };
        for (const auto& item : dma_result.metrics) {
            metrics[item.first] = item.second;
        }
        return TestResult{"pcie_dma_data_transfer", get_name(), passed, metrics, error, ""};
    };

    if (ti.hal == nullptr) {
        return finish(false, "not_started", "HAL context is not available");
    }
    if ((direction != "h2d" && direction != "d2h") ||
        size_bytes == 0 || size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
        (size_bytes % DMA_ALIGNMENT) != 0 ||
        (device_offset % DMA_ALIGNMENT) != 0) {
        return finish(false, "not_started",
                      "direction must be h2d/d2h and DMA range must be 32-byte aligned and <= 128MiB");
    }

    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    if (expected.empty()) {
        return finish(false, "not_started", "pattern generation failed");
    }

    auto host_buffer = ti.hal->alloc_host_dma_buffer(ctx_, size_bytes);
    if (!host_buffer.valid()) {
        return finish(false, "not_started",
                      "host DMA allocation failed; check that the matching /dev/isml_diag device is available");
    }
    auto* host_base = static_cast<uint8_t*>(host_buffer.cpu_base());

    DmaTransferRequest request;
    request.host_buffer = &host_buffer;
    request.host_offset = HOST_OFFSET;
    request.device_offset = device_offset;
    request.size_bytes = size_bytes;
    request.timeout_ms = timeout_ms;

    std::string access_error;
    if (direction == "h2d") {
        std::memcpy(host_base + HOST_OFFSET, expected.data(), expected.size());
        dma_result = impl_->dma_copy_h2d(ti, request);
        if (!dma_result.ok) {
            return finish(false, "dma_failed", dma_result.error);
        }

        std::vector<uint8_t> actual(static_cast<size_t>(size_bytes));
        if (!common::devmem::read(ti, ctx_, DMEM_WINDOW, device_offset,
                                  actual.data(), actual.size(), &access_error)) {
            return finish(false, "readback_failed", access_error);
        }
        const bool match = common::pattern::compare(expected.data(),
                                                     actual.data(),
                                                     actual.size());
        return finish(match, match ? "match" : "mismatch",
                      match ? "" : "H2D DMA data mismatch");
    }

    if (!common::devmem::write(ti, ctx_, DMEM_WINDOW, device_offset,
                               expected.data(), expected.size(), &access_error)) {
        return finish(false, "source_write_failed", access_error);
    }
    std::memset(host_base + HOST_OFFSET, 0, static_cast<size_t>(size_bytes));
    dma_result = impl_->dma_copy_d2h(ti, request);
    if (!dma_result.ok) {
        return finish(false, "dma_failed", dma_result.error);
    }

    const bool match = common::pattern::compare(expected.data(),
                                                 host_base + HOST_OFFSET,
                                                 static_cast<size_t>(size_bytes));
    return finish(match, match ? "match" : "mismatch",
                  match ? "" : "D2H DMA data mismatch");
}
