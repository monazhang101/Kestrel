#include "atlas/pcie/hqc_admin_queue.h"

#include "diag/core/Common.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace atlas_impl {
namespace {

constexpr uint32_t HQC_BAR = 0;
constexpr uint32_t QUEUE_SIZE = 0x400;
constexpr uint32_t ENTRY_SIZE = sizeof(HqcAdminCommand);
// Short bring-up polling interval; IRQ/event-driven completion is future work.
constexpr uint64_t POLL_INTERVAL_MS = 1;

}

HqcAdminQueue::HqcAdminQueue(const DeviceContext& device,
                             uint64_t hqc_sram_bar_offset,
                             Logger* logger)
    : device_(device),
      hqc_sram_bar_offset_(hqc_sram_bar_offset),
      logger_(logger)
{
}

int HqcAdminQueue::read32(uint64_t offset, uint32_t& value) const
{
    return common::bar::read32(device_, HQC_BAR, hqc_sram_bar_offset_ + offset, value)
               ? HQC_STATUS_OK
               : HQC_STATUS_IO_ERROR;
}

int HqcAdminQueue::write32(uint64_t offset, uint32_t value) const
{
    return common::bar::write32(device_, HQC_BAR, hqc_sram_bar_offset_ + offset, value)
               ? HQC_STATUS_OK
               : HQC_STATUS_IO_ERROR;
}

int HqcAdminQueue::full(bool& is_full) const
{
    uint32_t wptr = 0;
    uint32_t rptr = 0;
    if (read32(sq_.wptr, wptr) != HQC_STATUS_OK ||
        read32(sq_.rptr, rptr) != HQC_STATUS_OK) {
        return HQC_STATUS_IO_ERROR;
    }
    is_full = static_cast<uint32_t>(wptr - rptr) >= QUEUE_SIZE;
    return HQC_STATUS_OK;
}

int HqcAdminQueue::empty(bool& is_empty) const
{
    uint32_t wptr = 0;
    uint32_t rptr = 0;
    if (read32(cq_.wptr, wptr) != HQC_STATUS_OK ||
        read32(cq_.rptr, rptr) != HQC_STATUS_OK) {
        return HQC_STATUS_IO_ERROR;
    }
    is_empty = wptr == rptr;
    return HQC_STATUS_OK;
}

int HqcAdminQueue::push(const HqcAdminCommand& command, uint64_t timeout_ms) const
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    bool is_full = false;
    while (true) {
        const int status = full(is_full);
        if (status != HQC_STATUS_OK) {
            if (logger_ != nullptr) {
                logger_->error("HQC SQ pointer read failed");
            }
            return status;
        }
        if (!is_full) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (logger_ != nullptr) {
                logger_->error("HQC SQ push timed out: queue remains full");
            }
            return HQC_STATUS_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }

    uint32_t wptr = 0;
    if (read32(sq_.wptr, wptr) != HQC_STATUS_OK) {
        return HQC_STATUS_IO_ERROR;
    }

    std::array<uint32_t, ENTRY_SIZE / sizeof(uint32_t)> words = {};
    std::memcpy(words.data(), &command, sizeof(command));
    const uint64_t entry = sq_.buffer + (wptr & (QUEUE_SIZE - 1));
    for (uint32_t i = 0; i < words.size(); ++i) {
        if (write32(entry + i * sizeof(uint32_t), words[i]) != HQC_STATUS_OK) {
            if (logger_ != nullptr) {
                logger_->error("HQC SQ command write failed");
            }
            return HQC_STATUS_IO_ERROR;
        }
    }

    std::atomic_thread_fence(std::memory_order_release);
    if (write32(sq_.wptr, wptr + ENTRY_SIZE) != HQC_STATUS_OK) {
        return HQC_STATUS_IO_ERROR;
    }
    return HQC_STATUS_OK;
}

int HqcAdminQueue::pop(HqcAdminCommand& command, uint64_t timeout_ms) const
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    bool is_empty = true;
    while (true) {
        const int status = empty(is_empty);
        if (status != HQC_STATUS_OK) {
            if (logger_ != nullptr) {
                logger_->error("HQC CQ pointer read failed");
            }
            return status;
        }
        if (!is_empty) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (logger_ != nullptr) {
                logger_->error("HQC CQ pop timed out: queue remains empty");
            }
            return HQC_STATUS_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }

    uint32_t rptr = 0;
    if (read32(cq_.rptr, rptr) != HQC_STATUS_OK) {
        return HQC_STATUS_IO_ERROR;
    }

    std::array<uint32_t, ENTRY_SIZE / sizeof(uint32_t)> words = {};
    const uint64_t entry = cq_.buffer + (rptr & (QUEUE_SIZE - 1));
    for (uint32_t i = 0; i < words.size(); ++i) {
        if (read32(entry + i * sizeof(uint32_t), words[i]) != HQC_STATUS_OK) {
            if (logger_ != nullptr) {
                logger_->error("HQC CQ command read failed");
            }
            return HQC_STATUS_IO_ERROR;
        }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    std::memcpy(&command, words.data(), sizeof(command));

    if (write32(cq_.rptr, rptr + ENTRY_SIZE) != HQC_STATUS_OK) {
        return HQC_STATUS_IO_ERROR;
    }
    return HQC_STATUS_OK;
}

}
