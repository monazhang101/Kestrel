#include "diag/modules/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/implementer/HqcDma.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using common::args::get_string;
using common::args::get_u64;

TestStatus PCIeModule::dma_concurrent_transfer(TestInfo& ti)
{
    constexpr uint64_t DMA_ALIGNMENT = 32;
    const auto job_count = get_u64(ti.args, "job_count");
    const auto size_bytes = get_u64(ti.args, "size_bytes");
    const auto timeout_ms = get_u64(ti.args, "timeout_ms");
    const auto pattern = get_string(ti.args, "pattern");

    if (job_count == 0 || size_bytes == 0 ||
        size_bytes % DMA_ALIGNMENT != 0 ||
        timeout_ms == 0 ||
        size_bytes > std::numeric_limits<uint64_t>::max() / job_count) {
        ti.logger->error(
            "concurrent DMA requires nonzero job_count, 32-byte size alignment, and nonzero timeout");
        return PHAL_STATUS_INVALID;
    }
    const uint64_t total_bytes = job_count * size_bytes;
    if (total_bytes > HOST_DMA_MAX_SIZE_BYTES ||
        total_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        ti.logger->error("concurrent DMA total payload exceeds the host allocation limit");
        return PHAL_STATUS_INVALID;
    }

    auto host = ti.hal->host_mem_alloc(total_bytes);
    auto device = ti.hal->device_mem_alloc(DMEM, total_bytes);
    if (!host.valid() || !device.valid()) {
        ti.logger->error("concurrent DMA payload allocation failed");
        return PHAL_STATUS_ERROR;
    }
    const auto retain_payloads = [&] {
        host.keep_allocated();
        device.keep_allocated();
    };

    auto expected = common::pattern::generate(
        static_cast<size_t>(total_bytes), pattern);
    if (expected.empty()) return PHAL_STATUS_INVALID;
    std::memcpy(host.cpu_base(), expected.data(), expected.size());

    std::vector<uint8_t> poison(expected.size(), 0xa5);
    std::vector<uint8_t> actual(expected.size());
    auto status = mem_write(device, 0, poison);
    if (status == PHAL_STATUS_OK) status = mem_read(device, 0, &actual);
    if (status != PHAL_STATUS_OK || actual != poison) {
        ti.logger->error("concurrent DMA destination preparation failed");
        return status == PHAL_STATUS_OK ? PHAL_STATUS_ERROR : status;
    }

    HqcDmaEngine engine(*ti.hal);
    const size_t window_size = static_cast<size_t>(std::min<uint64_t>(
        job_count, HqcDmaEngine::DESCRIPTOR_SLOT_COUNT));
    std::vector<std::unique_ptr<DmaDescriptor>> jobs;
    std::vector<DmaDescriptor*> batch;
    std::vector<uint64_t> active_job(window_size);
    jobs.reserve(window_size);
    batch.reserve(window_size);
    const auto fill_job = [&](DmaDescriptor& job, uint64_t logical_id) {
        const uint64_t offset = logical_id * size_bytes;
        auto fill_status = job.set_src(host, offset);
        if (fill_status == PHAL_STATUS_OK)
            fill_status = job.set_dst(device, offset);
        if (fill_status == PHAL_STATUS_OK)
            fill_status = job.set_size(size_bytes);
        if (fill_status == PHAL_STATUS_OK)
            fill_status = job.fill_descriptor();
        return fill_status;
    };
    for (size_t i = 0; i < window_size; ++i) {
        auto job = std::make_unique<DmaDescriptor>(engine, DmaProfile::H2D);
        status = fill_job(*job, i);
        if (status != PHAL_STATUS_OK) return status;
        active_job[i] = i;
        batch.push_back(job.get());
        jobs.push_back(std::move(job));
    }

    ti.logger->info("DMA concurrent window prepared: jobs=" +
                    std::to_string(job_count) +
                    " in_flight=" + std::to_string(window_size) +
                    " bytes_per_job=" + std::to_string(size_bytes));
    status = engine.submit(batch);
    if (status != PHAL_STATUS_OK) {
        retain_payloads();
        return status;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::vector<uint32_t> completion_order;
    constexpr size_t COMPLETION_SAMPLE_LIMIT = 64;
    completion_order.reserve(std::min(window_size, COMPLETION_SAMPLE_LIMIT));
    uint64_t next_job = window_size;
    uint64_t remaining = job_count;
    uint64_t last_completed = 0;
    uint64_t out_of_order = 0;
    bool have_completion = false;
    while (remaining != 0 && std::chrono::steady_clock::now() < deadline) {
        bool progress = false;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (active_job[i] == std::numeric_limits<uint64_t>::max()) continue;
            status = jobs[i]->poll_completion();
            if (status == PHAL_STATUS_INVALID) continue;
            if (status != PHAL_STATUS_OK) {
                retain_payloads();
                return status;
            }
            const uint64_t completed_job = active_job[i];
            --remaining;
            progress = true;
            if (have_completion && completed_job < last_completed)
                ++out_of_order;
            last_completed = completed_job;
            have_completion = true;
            if (completion_order.size() < COMPLETION_SAMPLE_LIMIT)
                completion_order.push_back(static_cast<uint32_t>(completed_job));

            if (next_job < job_count) {
                status = fill_job(*jobs[i], next_job);
                if (status == PHAL_STATUS_OK) status = jobs[i]->submit();
                if (status != PHAL_STATUS_OK) {
                    retain_payloads();
                    return status;
                }
                active_job[i] = next_job++;
            } else {
                active_job[i] = std::numeric_limits<uint64_t>::max();
            }
        }
        if (!progress && remaining != 0)
            std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    if (remaining != 0) {
        ti.logger->error("DMA concurrent batch timed out: remaining=" +
                         std::to_string(remaining));
        retain_payloads();
        return PHAL_STATUS_TIMEOUT;
    }

    status = mem_read(device, 0, &actual);
    if (status != PHAL_STATUS_OK) return status;
    if (!common::pattern::compare(expected.data(), actual.data(), expected.size())) {
        ti.logger->error("DMA concurrent batch data mismatch");
        return PHAL_STATUS_ERROR;
    }

    std::string order;
    for (size_t i = 0; i < completion_order.size(); ++i) {
        if (i != 0) order += ',';
        order += std::to_string(completion_order[i]);
    }
    ti.logger->info("DMA concurrent batch verified: jobs=" +
                    std::to_string(job_count) +
                    " completion_sample=" + order +
                    " out_of_order_transitions=" +
                    std::to_string(out_of_order));
    return PHAL_STATUS_OK;
}
