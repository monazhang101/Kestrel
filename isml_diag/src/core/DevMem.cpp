/* Common access to DMEM through an aperture and RCF memory through BAR0. */
#include "diag/core/DevMem.h"

#include "diag/core/Common.h"
#include "diag/core/Format.h"
#include "diag/core/HalContext.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

using common::format::hex;
using common::format::hex_bytes;

extern "C" {
#include <phal/components/pcie/pcie.h>
}

namespace {

constexpr size_t MMIO_PROGRESS_MIN_BYTES = 1u << 20;
constexpr size_t MMIO_CHUNK_BYTES = 64u << 10;
constexpr auto MMIO_PROGRESS_INTERVAL = std::chrono::seconds(10);

bool mmio_transfer(const DeviceContext& ctx, uint32_t bar_index,
                   uint64_t bar_offset, void* output, const void* input,
                   size_t len, Logger* logger)
{
    const bool writing = input != nullptr;
    const bool show_progress = logger != nullptr && len >= MMIO_PROGRESS_MIN_BYTES;
    const auto start = std::chrono::steady_clock::now();
    auto next_progress = start + MMIO_PROGRESS_INTERVAL;

    if (show_progress) {
        logger->info(std::string("MMIO ") + (writing ? "write" : "read") +
                     " started: size_bytes=" + std::to_string(len));
    }

    size_t completed = 0;
    while (completed < len) {
        const auto chunk = std::min(MMIO_CHUNK_BYTES, len - completed);
        const bool ok = writing
            ? common::bar::write(ctx, bar_index, bar_offset + completed,
                                 static_cast<const uint8_t*>(input) + completed, chunk)
            : common::bar::read(ctx, bar_index, bar_offset + completed,
                                static_cast<uint8_t*>(output) + completed, chunk);
        if (!ok) return false;
        completed += chunk;

        const auto now = std::chrono::steady_clock::now();
        if (show_progress && completed < len && now >= next_progress) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            std::ostringstream message;
            message << "MMIO " << (writing ? "write" : "read")
                    << " progress: " << std::fixed << std::setprecision(1)
                    << (100.0 * static_cast<double>(completed) /
                        static_cast<double>(len))
                    << "% (" << completed << '/' << len
                    << " bytes), elapsed_s=" << elapsed;
            logger->info(message.str());
            next_progress = now + MMIO_PROGRESS_INTERVAL;
        }
    }

    if (show_progress) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        logger->info(std::string("MMIO ") + (writing ? "write" : "read") +
                     " progress: 100.0% (" + std::to_string(len) + '/' +
                     std::to_string(len) + " bytes), elapsed_ms=" +
                     std::to_string(elapsed));
    }
    return true;
}

struct Window {
    uint32_t bar_index = 0;
    uint8_t aperture_index = 0;
    uint8_t identity = 0;
    uint64_t target_addr = 0;
    uint64_t size = 0;
    uint64_t bar_offset = 0;
};

bool fail(std::string* error, const std::string& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool prepare_window(phal_ctx_t* phal, Logger* logger,
                    const DeviceContext& ctx,
                    const Window& window,
                    uint64_t offset,
                    size_t len,
                    std::string* error)
{
    if (phal == nullptr) return fail(error, "PCIe PHAL context is not initialized");
    if (!common::mmio::is_valid_range(window.size, offset, len)) {
        return fail(error, "device-memory access is outside the aperture window");
    }
    if (window.bar_offset > std::numeric_limits<uint64_t>::max() - offset) {
        return fail(error, "device-memory BAR offset overflow");
    }
    if (window.size == 0 || window.target_addr >
        std::numeric_limits<uint64_t>::max() - (window.size - 1)) {
        return fail(error, "device-memory target range overflow");
    }
    const auto absolute_bar_offset = window.bar_offset + offset;
    const auto* data_bar = common::bar::find(ctx, window.bar_index);
    if (data_bar == nullptr || !data_bar->mapped ||
        !common::mmio::is_valid_range(data_bar->mapped_size,
                                      absolute_bar_offset,
                                      len)) {
        return fail(error, "device-memory access is outside the mapped data BAR");
    }

    phal_pcie_aperture_t requested = {};
    requested.identity = window.identity;
    requested.target_addr = window.target_addr;
    requested.size = window.size;

    auto status = phal_pcie_aperture_set(phal,
                                         window.bar_index,
                                         window.aperture_index,
                                         &requested);
    if (status != PHAL_STATUS_OK) {
        return fail(error,
                    "phal_pcie_aperture_set failed: status=" +
                        std::to_string(static_cast<int>(status)));
    }

    phal_pcie_aperture_t actual = {};
    status = phal_pcie_aperture_get(phal,
                                    window.bar_index,
                                    window.aperture_index,
                                    &actual);
    if (status != PHAL_STATUS_OK) {
        return fail(error,
                    "phal_pcie_aperture_get failed: status=" +
                        std::to_string(static_cast<int>(status)));
    }
    if (actual.identity != requested.identity ||
        actual.target_addr != requested.target_addr ||
        actual.size != requested.size) {
        return fail(error, "PCIe aperture readback does not match the requested window");
    }

    if (logger != nullptr) {
        std::ostringstream stream;
        stream << "Device-memory aperture configured"
               << "\n       bar_index        = " << window.bar_index
               << "\n       aperture_index   = "
               << static_cast<uint32_t>(window.aperture_index)
               << "\n       target_addr      = " << hex(window.target_addr)
               << "\n       window_size      = " << hex(window.size)
               << "\n       data_bar_offset  = " << hex(window.bar_offset);
        logger->debug(stream.str());
    }
    return true;
}


bool transfer(phal_ctx_t* phal, Logger* logger, const DeviceContext& ctx,
              const Window& window, uint64_t offset, void* output,
              const void* input, size_t len, std::string* error)
{
    if (error != nullptr) error->clear();
    if (input == nullptr && output == nullptr) return fail(error, "device-memory buffer is null");
    const bool writing = input != nullptr;
    const auto* bar = common::bar::find(ctx, window.bar_index);
    if (writing && (bar == nullptr || !bar->mapped || !bar->writable))
        return fail(error, "device-memory data BAR is not writable");
    if (!prepare_window(phal, logger, ctx, window, offset, len, error)) return false;
    const bool ok = mmio_transfer(ctx, window.bar_index,
                                  window.bar_offset + offset,
                                  output, input, len, logger);
    if (!ok) return fail(error, "device-memory BAR access failed");
    if (logger != nullptr) {
        logger->debug(std::string(writing ? "D-MEM write" : "D-MEM read") +
            " target_addr=" + hex(window.target_addr + offset) +
            " size_bytes=" + std::to_string(len) + " data_preview=" +
            hex_bytes(writing ? input : output, len));
    }
    return true;
}

}

namespace common {
namespace {
TestStatus dmem_transfer(HalContext& hal, DeviceContext& ctx, uint64_t addr, void* output,
                         const void* input, size_t len, size_t alignment)
{
    const auto* region = find_df_region(ctx.memory_regions.df, MemoryRegion::DMEM);
    if (region == nullptr || !region->supported) {
        if (ctx.logger) ctx.logger->error("DMEM region is not supported");
        return PHAL_STATUS_UNIMPLEMENTED;
    }
    if ((output == nullptr && input == nullptr) || addr % alignment != 0 ||
        region->base % alignment != 0 ||
        !mmio::is_valid_range(region->size, addr, len) ||
        region->aperture_size == 0 ||
        region->base > std::numeric_limits<uint64_t>::max() - addr) {
        if (ctx.logger) ctx.logger->error("invalid D-MEM address or buffer length");
        return PHAL_STATUS_INVALID;
    }
    const auto window_base = addr - (addr % region->aperture_size);
    const auto window_offset = addr - window_base;
    if (window_base > std::numeric_limits<uint64_t>::max() - region->base ||
        !mmio::is_valid_range(region->aperture_size, window_offset, len)) {
        if (ctx.logger) ctx.logger->error("invalid D-MEM aperture range");
        return PHAL_STATUS_INVALID;
    }
    const Window window{
        region->bar_index,
        region->aperture_index,
        region->identity,
        region->base + window_base,
        region->aperture_size,
        0};
    std::string error;
    const auto status = hal.with_aperture_context(
        [&](phal_ctx_t* phal) {
            return transfer(phal, ctx.logger, ctx, window, window_offset,
                            output, input, len, &error)
                       ? PHAL_STATUS_OK
                       : PHAL_STATUS_ERROR;
        },
        &error);
    if (status != PHAL_STATUS_OK) {
        if (ctx.logger) ctx.logger->error("D-MEM access failed: " + error);
        return status;
    }
    if (ctx.logger) ctx.logger->debug(std::string(input ? "mem_write" : "mem_read") +
        " region=DMEM target=" + ctx.target_name +
        " address=" + hex(region->base + addr) +
        " size_bytes=" + std::to_string(len) +
        " data_preview=" + hex_bytes(input ? input : output, len));
    return PHAL_STATUS_OK;
}

TestStatus direct_rcf_transfer(DeviceContext& ctx, MemoryRegion id,
                               uint64_t addr, void* output,
                               const void* input, size_t len, size_t alignment)
{
    if (output == nullptr && input == nullptr) {
        if (ctx.logger) ctx.logger->error("RCF memory buffer is null");
        return PHAL_STATUS_INVALID;
    }
    const auto* region = find_rcf_region(ctx.memory_regions.rcf, id);
    if (region == nullptr || !region->supported) {
        if (ctx.logger) ctx.logger->error("RCF memory region is not supported");
        return PHAL_STATUS_UNIMPLEMENTED;
    }
    if (addr % alignment != 0 ||
        !mmio::is_valid_range(region->size, addr, len) ||
        region->bar_offset > std::numeric_limits<uint64_t>::max() - addr) {
        if (ctx.logger) ctx.logger->error("invalid RCF memory address or buffer length");
        return PHAL_STATUS_INVALID;
    }
    const auto region_base = region->bar_offset;
    if (region_base % alignment != 0 ||
        region_base > std::numeric_limits<uint64_t>::max() - addr) {
        if (ctx.logger) ctx.logger->error("invalid RCF memory BAR0 base or offset");
        return PHAL_STATUS_INVALID;
    }
    const auto bar0_offset = region_base + addr;
    const auto* bar0 = bar::find(ctx, 0);
    if (bar0 == nullptr || !bar0->mapped ||
        (input != nullptr && !bar0->writable) ||
        !mmio::is_valid_range(bar0->mapped_size, bar0_offset, len)) {
        if (ctx.logger) ctx.logger->error("RCF memory access is outside mapped BAR0 or is not writable");
        return PHAL_STATUS_ERROR;
    }
    const bool ok = mmio_transfer(ctx, 0, bar0_offset,
                                  output, input, len, ctx.logger);
    if (!ok) {
        if (ctx.logger) ctx.logger->error("RCF memory BAR0 access failed");
        return PHAL_STATUS_ERROR;
    }
    if (ctx.logger) {
        ctx.logger->debug(std::string(input ? "mem_write" : "mem_read") +
            " region=" + region->name + " target=" + ctx.target_name +
            " bar0_offset=" + hex(bar0_offset) +
            " size_bytes=" + std::to_string(len) +
            " data_preview=" + hex_bytes(input ? input : output, len));
    }
    return PHAL_STATUS_OK;
}

TestStatus mem_transfer(HalContext& hal, DeviceContext& ctx, MemoryRegion region,
                        uint64_t addr, void* output,
                        const void* input, size_t len, size_t alignment)
{
    if (region == MemoryRegion::DMEM) {
        return dmem_transfer(hal, ctx, addr, output, input, len, alignment);
    }
    if (find_df_region(ctx.memory_regions.df, region) != nullptr) {
        if (ctx.logger) ctx.logger->error("DF memory region is not supported");
        return PHAL_STATUS_UNIMPLEMENTED;
    }
    return direct_rcf_transfer(ctx, region, addr, output, input, len, alignment);
}
}

namespace {
DeviceMemoryState::Key pool_key(MemoryRegion region)
{
    return region;
}
}

}

MemBuffer HalContext::device_mem_alloc(MemoryRegion region,
                                       uint64_t size,
                                       std::optional<uint64_t> fixed_offset)
{
    uint64_t capacity = 0;
    if (region == DMEM) {
        const auto* entry = find_df_region(active_memory_regions_.df, region);
        if (entry && entry->supported) capacity = entry->size;
    } else {
        const auto* entry = find_rcf_region(active_memory_regions_.rcf, region);
        if (entry && entry->supported) capacity = entry->size;
    }
    if (!device_bound_ || !active_memory_ ||
        !size || size % 4 || size > capacity) {
        if (active_device_.logger) {
            active_device_.logger->error("memory allocation: unsupported region or invalid size");
        }
        return {};
    }

    uint64_t offset = 0;
    if (fixed_offset.has_value()) {
        offset = *fixed_offset;
        if (offset % 4 || offset > capacity || size > capacity - offset) {
            if (active_device_.logger) {
                active_device_.logger->error("memory allocation: fixed offset is outside the region");
            }
            return {};
        }
    }

    MemBuffer result;
    // Prepare the handle before changing the allocation table.
    auto context = std::make_unique<DeviceContext>(active_device_);
    context->memory = active_memory_;
    context->memory_regions = active_memory_regions_;
    std::lock_guard<std::mutex> lock(active_memory_->allocation_mutex);
    auto& occupied = active_memory_->allocations[region];
    if (!fixed_offset.has_value()) {
        for (const auto& entry : occupied) {
            if (size <= entry.first - offset) break;
            offset = entry.first + entry.second;
        }
    }
    if (offset > capacity || size > capacity - offset) {
        if (active_device_.logger) active_device_.logger->error("memory allocation: no free contiguous range");
        return {};
    }
    if (fixed_offset.has_value()) {
        for (const auto& entry : occupied) {
            if (offset < entry.first + entry.second &&
                entry.first < offset + size) {
                if (active_device_.logger) active_device_.logger->error("memory allocation: fixed range overlaps an existing allocation");
                return {};
            }
        }
    }
    occupied.emplace(offset, size);
    result.alloc_ctx_ = this;
    result.ctx_ = std::move(context);
    result.region_ = region;
    result.offset_ = offset;
    result.size_ = size;
    return result;
}

namespace common {

void MemBuffer::release()
{
    if (!ctx_) return;
    // Keep the state alive until after unlocking, even for the last handle.
    auto context = std::move(ctx_);
    std::lock_guard<std::mutex> lock(context->memory->allocation_mutex);
    const auto pool = context->memory->allocations.find(pool_key(region_));
    if (pool != context->memory->allocations.end()) pool->second.erase(offset_);
    alloc_ctx_ = nullptr;
    size_ = 0;
}

MemBuffer::MemBuffer(MemBuffer&& other) noexcept
    : alloc_ctx_(other.alloc_ctx_),
      ctx_(std::move(other.ctx_)),
      region_(other.region_),
      offset_(other.offset_),
      size_(other.size_)
{
    other.alloc_ctx_ = nullptr;
    other.offset_ = 0;
    other.size_ = 0;
}

MemBuffer& MemBuffer::operator=(MemBuffer&& other) noexcept
{
    if (this != &other) {
        release();
        alloc_ctx_ = other.alloc_ctx_;
        ctx_ = std::move(other.ctx_);
        region_ = other.region_;
        offset_ = other.offset_;
        size_ = other.size_;
        other.alloc_ctx_ = nullptr;
        other.offset_ = 0;
        other.size_ = 0;
    }
    return *this;
}

TestStatus MemBuffer::transfer(uint64_t offset,
                               void* output, const void* input, size_t len)
{
    if (!valid() || alloc_ctx_ == nullptr || offset % 4 || len % 4 ||
        !mmio::is_valid_range(size_, offset, len)) return PHAL_STATUS_INVALID;
    return mem_transfer(*alloc_ctx_, *ctx_, region_, offset_ + offset,
                        output, input, len, 4);
}

TestStatus mem_read(MemBuffer& buffer,
                    uint64_t offset, uint32_t* data)
{
    return buffer.transfer(offset, data, nullptr, sizeof(uint32_t));
}

TestStatus mem_write(MemBuffer& buffer,
                     uint64_t offset, uint32_t data)
{
    return buffer.transfer(offset, nullptr, &data, sizeof(data));
}

TestStatus mem_read(MemBuffer& buffer,
                    uint64_t offset, std::vector<uint8_t>* data)
{
    return buffer.transfer(offset, data ? data->data() : nullptr, nullptr,
                           data ? data->size() : 0);
}

TestStatus mem_write(MemBuffer& buffer,
                     uint64_t offset, const std::vector<uint8_t>& data)
{
    return buffer.transfer(offset, nullptr, data.data(), data.size());
}

}
