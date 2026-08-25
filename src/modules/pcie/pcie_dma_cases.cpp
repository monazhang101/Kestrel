#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// pcie_dma_data_transfer : To run PCIe DMA data movement and compare the transferred pattern.
// @input: "direction": h2d/d2h, "size_bytes", "pattern"
// @output: TestResult metrics include direction, size_bytes, pattern, data_compare.
TestResult PCIeModule::pcie_dma_data_transfer(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }

    constexpr uint64_t device_offset = 0x2000;
    constexpr uint64_t host_offset = 0;

    auto direction = common::args::get_string(ti.args, "direction");
    auto size_bytes = common::args::get_u64(ti.args, "size_bytes");
    auto pattern = common::args::get_string(ti.args, "pattern");
    auto timeout_ms = common::args::get_u64(ti.args, "timeout_ms", 1000);

    auto make_result = [&](bool passed,
                           const std::string& compare_status,
                           const DmaTransferResult& dma_transfer_res = DmaTransferResult{},
                           const std::string& error_description = "") {
        TestMetrics metrics = {
            {"direction", direction},
            {"size_bytes", std::to_string(size_bytes)},
            {"pattern", pattern},
            {"device_offset", std::to_string(device_offset)},
            {"host_offset", std::to_string(host_offset)},
            {"compare_status", compare_status},
            {"dma_completion_status", dma_transfer_res.completion_status},
            {"dma_bytes", std::to_string(dma_transfer_res.bytes)},
            {"dma_duration_us", std::to_string(dma_transfer_res.duration_us)}
        };
        for (const auto& item : dma_transfer_res.metrics) {
            metrics[item.first] = item.second;
        }
        return TestResult{"pcie_dma_data_transfer", get_name(), passed, metrics, error_description};
    };

    if (ti.hal == nullptr) {
        return make_result(false, "hal_session_missing", {}, "HAL session is not available");
    }

    auto expected = common::pattern::generate(static_cast<size_t>(size_bytes), pattern);
    if (expected.empty()) {
        return make_result(false, "pattern_generate_failed", {}, "pattern generation failed");
    }

    auto host_buffer = ti.hal->alloc_host_buffer(ctx_, size_bytes);
    if (!host_buffer.valid()) {
        return make_result(false, "dma_alloc_failed", {}, "host DMA buffer allocation failed");
    }

    auto* host_base = static_cast<uint8_t*>(host_buffer.cpu_base());
    if (host_base == nullptr) {
        return make_result(false, "host_buffer_missing", {}, "host DMA buffer CPU mapping is not available");
    }
    if (host_offset > host_buffer.size() || size_bytes > host_buffer.size() - host_offset) {
        return make_result(false, "host_buffer_range_invalid", {}, "host DMA buffer range is invalid");
    }

    DmaTransferRequest req;
    req.host_buffer = &host_buffer;
    req.host_offset = host_offset;
    req.device_offset = device_offset;
    req.size_bytes = size_bytes;
    req.timeout_ms = timeout_ms;

    DmaTransferResult dma_transfer_res;
    if (direction == "h2d") {
        if (!common::pattern::write(host_base + host_offset, size_bytes, pattern)) {
            return make_result(false, "pattern_write_failed", {}, "failed to write pattern to host DMA buffer");
        }
        dma_transfer_res = impl_->dma_copy_h2d(req);
        if (!dma_transfer_res.ok) {
            return make_result(false, "dma_failed", dma_transfer_res, dma_transfer_res.error);
        }

        std::vector<uint8_t> actual(static_cast<size_t>(size_bytes), 0);
        if (!common::devmem::read(ctx_.mapped_bar_base,
                                  ctx_.bar_size,
                                  device_offset,
                                  actual.data(),
                                  actual.size())) {
            return make_result(false, "device_readback_failed", dma_transfer_res, "failed to read device window");
        }

        auto compare_ok = common::pattern::compare(expected.data(), actual.data(), actual.size());
        return make_result(compare_ok,
                           compare_ok ? "match" : "mismatch",
                           dma_transfer_res,
                           compare_ok ? "" : "H2D DMA data compare failed");
    }

    if (direction == "d2h") {
        if (!common::devmem::write(ctx_.mapped_bar_base,
                                   ctx_.bar_size,
                                   device_offset,
                                   expected.data(),
                                   expected.size())) {
            return make_result(false, "device_pattern_write_failed", {}, "failed to write device source pattern");
        }
        std::memset(host_base + host_offset, 0, static_cast<size_t>(size_bytes));

        dma_transfer_res = impl_->dma_copy_d2h(req);
        if (!dma_transfer_res.ok) {
            return make_result(false, "dma_failed", dma_transfer_res, dma_transfer_res.error);
        }

        auto compare_ok = common::pattern::compare(expected.data(),
                                                   host_base + host_offset,
                                                   static_cast<size_t>(size_bytes));
        return make_result(compare_ok,
                           compare_ok ? "match" : "mismatch",
                           dma_transfer_res,
                           compare_ok ? "" : "D2H DMA data compare failed");
    }

    return make_result(false, "invalid_direction", {}, "direction must be h2d or d2h");
}
