#include "diag/modules/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/implementer/HqcDma.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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

    auto& engine = ti.hal->dma_engine();
    const size_t pool_size = static_cast<size_t>(std::min<uint64_t>(
        job_count, HqcDmaEngine::DESCRIPTOR_USABLE_SLOT_COUNT));
    std::vector<std::unique_ptr<DmaDescriptor>> jobs;
    std::vector<uint64_t> logical_jobs(pool_size);
    jobs.reserve(pool_size);
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
    for (size_t i = 0; i < pool_size; ++i)
        jobs.push_back(std::make_unique<DmaDescriptor>(engine, DmaProfile::H2D));

    ti.logger->info("DMA concurrent scheduler started: jobs=" +
                    std::to_string(job_count) +
                    " descriptor_slots=" + std::to_string(pool_size) +
                    " bytes_per_job=" + std::to_string(size_bytes));
    auto last_progress = std::chrono::steady_clock::now();
    std::vector<uint32_t> completion_order;
    constexpr size_t COMPLETION_SAMPLE_LIMIT = 64;
    completion_order.reserve(std::min(pool_size, COMPLETION_SAMPLE_LIMIT));
    uint64_t next_job = 0;
    uint64_t completed = 0;
    uint64_t last_completed = 0;
    uint64_t out_of_order = 0;
    bool have_completion = false;
    while (completed != job_count) {
        bool progress = false;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i]->state() != DmaJobState::Submitted) continue;
            status = jobs[i]->poll_completion();
            if (status == PHAL_STATUS_INVALID) continue;
            if (status != PHAL_STATUS_OK) {
                retain_payloads();
                return status;
            }
            const uint64_t completed_job = logical_jobs[i];
            ++completed;
            progress = true;
            if (have_completion && completed_job < last_completed)
                ++out_of_order;
            last_completed = completed_job;
            have_completion = true;
            if (completion_order.size() < COMPLETION_SAMPLE_LIMIT)
                completion_order.push_back(static_cast<uint32_t>(completed_job));

        }

        bool controllers_busy = false;
        for (auto& job : jobs) {
            if (job->state() != DmaJobState::Prepared) continue;
            bool triggered = false;
            status = job->try_submit(triggered);
            if (status != PHAL_STATUS_OK) {
                retain_payloads();
                return status;
            }
            if (!triggered) {
                controllers_busy = true;
                break;
            }
            progress = true;
        }

        for (size_t i = 0;
             !controllers_busy && i < jobs.size() && next_job < job_count;
             ++i) {
            if (jobs[i]->state() != DmaJobState::Draft &&
                jobs[i]->state() != DmaJobState::Completed) continue;
            status = fill_job(*jobs[i], next_job);
            if (status != PHAL_STATUS_OK) {
                retain_payloads();
                return status;
            }
            logical_jobs[i] = next_job++;

            bool triggered = false;
            status = jobs[i]->try_submit(triggered);
            if (status != PHAL_STATUS_OK) {
                retain_payloads();
                return status;
            }
            if (!triggered) break;
            progress = true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (progress) {
            last_progress = now;
        } else if (now - last_progress >= std::chrono::milliseconds(timeout_ms)) {
            ti.logger->error("DMA concurrent scheduler made no progress: completed=" +
                             std::to_string(completed) +
                             " jobs=" + std::to_string(job_count));
            retain_payloads();
            return PHAL_STATUS_TIMEOUT;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
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
    ti.logger->info("DMA concurrent scheduler verified: jobs=" +
                    std::to_string(job_count) +
                    " completion_sample=" + order +
                    " out_of_order_transitions=" +
                    std::to_string(out_of_order));
    return PHAL_STATUS_OK;
}

TestStatus PCIeModule::dma_threaded_stress(TestInfo& ti)
{
    constexpr size_t H2D_WORKER = 0;
    constexpr size_t D2H_WORKER = 1;
    constexpr size_t MMIO_WORKER = 2;
    constexpr size_t WORKER_COUNT = 3;
    constexpr uint64_t DMA_ALIGNMENT = 32;

    if (impl_ == nullptr || ti.hal == nullptr)
        return make_unimplemented_status(ti, "PCIe implementation is not bound");

    const auto iterations = get_u64(ti.args, "iterations");
    const auto size_bytes = get_u64(ti.args, "size_bytes");
    const auto timeout_ms = get_u64(ti.args, "timeout_ms");
    ti.logger->info(
        "DMA threaded stress started: iterations=" + std::to_string(iterations) +
        " size_bytes=" + std::to_string(size_bytes) +
        " timeout_ms=" + std::to_string(timeout_ms) +
        " workers=h2d,d2h,mmio");
    if (iterations == 0 || size_bytes == 0 || timeout_ms == 0 ||
        size_bytes % DMA_ALIGNMENT != 0 ||
        size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
        size_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        ti.logger->error(
            "threaded stress requires nonzero iterations/timeout, 32-byte size alignment, and size_bytes <= 128MiB");
        return PHAL_STATUS_INVALID;
    }

    const auto h2d_expected = common::pattern::generate(
        static_cast<size_t>(size_bytes), "incremental");
    const auto d2h_expected = common::pattern::generate(
        static_cast<size_t>(size_bytes), "random");
    const auto mmio_pattern = common::pattern::generate(
        static_cast<size_t>(size_bytes), "incremental");
    if (h2d_expected.empty() || d2h_expected.empty() || mmio_pattern.empty())
        return PHAL_STATUS_INVALID;

    auto h2d_host = ti.hal->host_mem_alloc(size_bytes);
    auto d2h_host = ti.hal->host_mem_alloc(size_bytes);
    auto h2d_device = ti.hal->device_mem_alloc(DMEM, size_bytes);
    auto d2h_device = ti.hal->device_mem_alloc(DMEM, size_bytes);
    auto mmio_device = ti.hal->device_mem_alloc(DMEM, size_bytes);
    if (!h2d_host.valid() || !d2h_host.valid() || !h2d_device.valid() ||
        !d2h_device.valid() || !mmio_device.valid()) {
        ti.logger->error("threaded stress buffer allocation failed");
        return PHAL_STATUS_ERROR;
    }

    std::memcpy(h2d_host.cpu_base(), h2d_expected.data(), h2d_expected.size());
    std::memset(d2h_host.cpu_base(), 0xa5, static_cast<size_t>(size_bytes));
    std::vector<uint8_t> poison(static_cast<size_t>(size_bytes), 0xa5);
    std::vector<uint8_t> setup_readback(static_cast<size_t>(size_bytes));
    auto status = mem_write(h2d_device, 0, poison);
    if (status == PHAL_STATUS_OK)
        status = mem_write(d2h_device, 0, d2h_expected);
    if (status == PHAL_STATUS_OK)
        status = mem_read(d2h_device, 0, &setup_readback);
    if (status != PHAL_STATUS_OK || setup_readback != d2h_expected) {
        ti.logger->error("threaded stress DMEM preparation failed");
        return status == PHAL_STATUS_OK ? PHAL_STATUS_ERROR : status;
    }

    // Create the testcase-scoped engine before the workers start. PCIeImpl's
    // one-shot DMA path obtains this same instance from HalContext.
    (void)ti.hal->dma_engine();
    ti.logger->info("DMA threaded stress buffers prepared; launching workers");

    std::mutex start_mutex;
    std::condition_variable start_cv;
    size_t ready_workers = 0;
    bool start = false;
    std::atomic<bool> stop{false};
    std::array<TestStatus, WORKER_COUNT> results{
        PHAL_STATUS_OK, PHAL_STATUS_OK, PHAL_STATUS_OK};

    const auto wait_for_start = [&] {
        std::unique_lock<std::mutex> lock(start_mutex);
        ++ready_workers;
        start_cv.notify_all();
        start_cv.wait(lock, [&] { return start; });
    };
    const auto run_worker = [&](size_t index,
                                const std::function<TestStatus()>& operation) {
        return std::thread([&, index, operation] {
            wait_for_start();
            try {
                results[index] = operation();
            } catch (const std::exception& error) {
                ti.logger->error("threaded stress worker exception: " +
                                 std::string(error.what()));
                results[index] = PHAL_STATUS_ERROR;
            } catch (...) {
                ti.logger->error("threaded stress worker raised an unknown exception");
                results[index] = PHAL_STATUS_ERROR;
            }
            if (results[index] != PHAL_STATUS_OK) stop = true;
        });
    };

    const auto h2d_operation = [&] {
        DmaTransferRequest request{};
        request.type = DmaTransferType::H2D;
        request.host_buffer = &h2d_host;
        request.dst_offset = h2d_device.offset();
        request.size_bytes = size_bytes;
        request.timeout_ms = timeout_ms;
        for (uint64_t i = 0; i < iterations && !stop; ++i) {
            const auto result = impl_->dma_copy(ti, request);
            if (result != PHAL_STATUS_OK) return result;
        }
        ti.logger->info("DMA threaded stress H2D worker completed");
        return PHAL_STATUS_OK;
    };

    const auto d2h_operation = [&] {
        DmaTransferRequest request{};
        request.type = DmaTransferType::D2H;
        request.host_buffer = &d2h_host;
        request.src_offset = d2h_device.offset();
        request.size_bytes = size_bytes;
        request.timeout_ms = timeout_ms;
        for (uint64_t i = 0; i < iterations && !stop; ++i) {
            const auto result = impl_->dma_copy(ti, request);
            if (result != PHAL_STATUS_OK) return result;
        }
        ti.logger->info("DMA threaded stress D2H worker completed");
        return PHAL_STATUS_OK;
    };

    const auto mmio_operation = [&] {
        auto expected = mmio_pattern;
        std::vector<uint8_t> actual(expected.size());
        for (uint64_t i = 0; i < iterations && !stop; ++i) {
            expected[0] = static_cast<uint8_t>(i);
            auto result = mem_write(mmio_device, 0, expected);
            if (result == PHAL_STATUS_OK)
                result = mem_read(mmio_device, 0, &actual);
            if (result != PHAL_STATUS_OK) return result;
            if (actual != expected) return PHAL_STATUS_ERROR;
        }
        ti.logger->info("DMA threaded stress MMIO worker completed");
        return PHAL_STATUS_OK;
    };

    std::thread h2d_thread;
    std::thread d2h_thread;
    std::thread mmio_thread;
    try {
        h2d_thread = run_worker(H2D_WORKER, h2d_operation);
        d2h_thread = run_worker(D2H_WORKER, d2h_operation);
        mmio_thread = run_worker(MMIO_WORKER, mmio_operation);
    } catch (const std::exception& error) {
        stop = true;
        {
            std::lock_guard<std::mutex> lock(start_mutex);
            start = true;
        }
        start_cv.notify_all();
        if (h2d_thread.joinable()) h2d_thread.join();
        if (d2h_thread.joinable()) d2h_thread.join();
        if (mmio_thread.joinable()) mmio_thread.join();
        ti.logger->error("threaded stress could not start workers: " +
                         std::string(error.what()));
        return PHAL_STATUS_ERROR;
    }

    {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_cv.wait(lock, [&] { return ready_workers == WORKER_COUNT; });
        ti.logger->info("DMA threaded stress workers synchronized; starting concurrent workload");
        start = true;
    }
    start_cv.notify_all();

    h2d_thread.join();
    d2h_thread.join();
    mmio_thread.join();

    status = PHAL_STATUS_OK;
    for (const auto result : results) status |= result;
    if (results[H2D_WORKER] != PHAL_STATUS_OK ||
        results[D2H_WORKER] != PHAL_STATUS_OK) {
        h2d_host.keep_allocated();
        d2h_host.keep_allocated();
        h2d_device.keep_allocated();
        d2h_device.keep_allocated();
    }
    if (status != PHAL_STATUS_OK) {
        ti.logger->error(
            "threaded stress failed: h2d=" + test_status_name(results[H2D_WORKER]) +
            " d2h=" + test_status_name(results[D2H_WORKER]) +
            " mmio=" + test_status_name(results[MMIO_WORKER]));
        return status;
    }

    std::vector<uint8_t> h2d_actual(static_cast<size_t>(size_bytes));
    status = mem_read(h2d_device, 0, &h2d_actual);
    if (status != PHAL_STATUS_OK || h2d_actual != h2d_expected) {
        ti.logger->error("threaded stress H2D verification failed");
        return status == PHAL_STATUS_OK ? PHAL_STATUS_ERROR : status;
    }
    if (!common::pattern::compare(
            d2h_expected.data(), d2h_host.cpu_base(), d2h_expected.size())) {
        ti.logger->error("threaded stress D2H verification failed");
        return PHAL_STATUS_ERROR;
    }

    ti.logger->info("DMA threaded stress verified: iterations=" +
                    std::to_string(iterations) +
                    " size_bytes=" + std::to_string(size_bytes));
    return PHAL_STATUS_OK;
}
