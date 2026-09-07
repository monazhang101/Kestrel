#include "generic/generic_impl.h"

#include <utility>

namespace atlas_m_impl {
namespace {

class AtlasMPCIeImpl : public generic_impl::GenericPCIeImpl {
public:
    explicit AtlasMPCIeImpl(ModuleImplContext ctx)
        : generic_impl::GenericPCIeImpl(std::move(ctx))
    {
    }

    LinkStatus link_status_get() override
    {
        LinkStatus status;
        status.ok = true;
        status.current_speed = "gen4";
        status.current_width = "x16";
        status.metrics = {{"impl", "atlas_m"}};
        return status;
    }

    DmaTransferResult dma_copy_h2d(const DmaTransferRequest& req) override
    {
        if (req.host_buffer == nullptr || !req.host_buffer->valid()) {
            return {false, 0, 0, "host_buffer_invalid", "host DMA buffer is invalid"};
        }

        // Real Atlas_M flow:
        //   1. Build an Atlas_M H2D DMA descriptor using req.host_buffer->device_addr()
        //      plus req.host_offset as source, and req.device_offset as destination.
        //   2. Program descriptor/ring state through the Atlas_M DMA engine path.
        //   3. Execute dma_setup and dma_start.
        //   4. Poll or wait for completion, then collect completion status.
        // Pseudocode keeps this as a successful placeholder until the real
        // descriptor format and completion path are wired in.

        DmaTransferResult dma_transfer_res;
        dma_transfer_res.ok = true;
        dma_transfer_res.bytes = req.size_bytes;
        dma_transfer_res.duration_us = 12;
        dma_transfer_res.completion_status = "completed";
        dma_transfer_res.metrics = {
            {"impl", "atlas_m"},
            {"dma_direction", "h2d"},
            {"dma_device_offset", std::to_string(req.device_offset)},
            {"dma_host_offset", std::to_string(req.host_offset)},
            {"dma_addr_kind", req.host_buffer->addr_kind()},
            {"dma_device_addr", std::to_string(req.host_buffer->device_addr())}
        };
        return dma_transfer_res;
    }

    DmaTransferResult dma_copy_d2h(const DmaTransferRequest& req) override
    {
        if (req.host_buffer == nullptr || !req.host_buffer->valid()) {
            return {false, 0, 0, "host_buffer_invalid", "host DMA buffer is invalid"};
        }

        // Real Atlas_M flow:
        //   1. Build an Atlas_M D2H DMA descriptor using req.device_offset as source
        //      and req.host_buffer->device_addr() plus req.host_offset as destination.
        //   2. Program descriptor/ring state through the Atlas_M DMA engine path.
        //   3. Execute dma_setup and dma_start.
        //   4. Poll or wait for completion, then collect completion status.
        // Pseudocode keeps this as a successful placeholder until the real
        // descriptor format and completion path are wired in.

        DmaTransferResult dma_transfer_res;
        dma_transfer_res.ok = true;
        dma_transfer_res.bytes = req.size_bytes;
        dma_transfer_res.duration_us = 12;
        dma_transfer_res.completion_status = "completed";
        dma_transfer_res.metrics = {
            {"impl", "atlas_m"},
            {"dma_direction", "d2h"},
            {"dma_device_offset", std::to_string(req.device_offset)},
            {"dma_host_offset", std::to_string(req.host_offset)},
            {"dma_addr_kind", req.host_buffer->addr_kind()},
            {"dma_device_addr", std::to_string(req.host_buffer->device_addr())}
        };
        return dma_transfer_res;
    }
};

}

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasMPCIeImpl>(ctx);
}

}
