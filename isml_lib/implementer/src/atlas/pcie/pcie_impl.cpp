#include "atlas/pcie/hqc_admin_queue.h"
#include "generic/generic_impl.h"

#include <chrono>
#include <limits>
#include <sstream>
#include <utility>

namespace atlas_impl {
namespace {

// Atlas HQC DMA ABI alignment for both addresses and transfer length.
constexpr uint64_t DMA_ALIGNMENT = 32;

class AtlasPCIeImpl : public generic_impl::GenericPCIeImpl {
public:
    explicit AtlasPCIeImpl(ModuleImplContext ctx)
        : generic_impl::GenericPCIeImpl(std::move(ctx))
    {
    }

    LinkStatus link_status_get() override
    {
        LinkStatus status;
        status.ok = true;
        status.current_speed = "gen5";
        status.current_width = "x8";
        status.metrics = {{"impl", "atlas"}};
        return status;
    }

    DmaTransferResult dma_copy_h2d(TestInfo& ti,
                                   const DmaTransferRequest& req) override
    {
        return dma_copy(ti, req, true);
    }

    DmaTransferResult dma_copy_d2h(TestInfo& ti,
                                   const DmaTransferRequest& req) override
    {
        return dma_copy(ti, req, false);
    }

private:
    DmaTransferResult dma_copy(TestInfo& ti,
                               const DmaTransferRequest& req,
                               bool host_to_device)
    {
        if (req.host_buffer == nullptr || !req.host_buffer->valid()) {
            return {false, 0, 0, "host_buffer_invalid", "host DMA buffer is invalid", {}};
        }
        if (req.host_buffer->addr_kind() != "dma_addr") {
            return {false, 0, 0, "dma_backend_invalid",
                    "HQC DMA requires a buffer allocated by isml_kmod", {}};
        }
        if (req.size_bytes == 0 || req.size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
            req.size_bytes > std::numeric_limits<uint32_t>::max() ||
            req.host_offset > req.host_buffer->size() ||
            req.size_bytes > req.host_buffer->size() - req.host_offset) {
            return {false, 0, 0, "invalid_range", "DMA request range is invalid", {}};
        }
        if (req.host_buffer->device_addr() >
            std::numeric_limits<uint64_t>::max() - req.host_offset) {
            return {false, 0, 0, "invalid_range", "host DMA address overflow", {}};
        }
        if (req.device_offset >
            std::numeric_limits<uint64_t>::max() - req.size_bytes) {
            return {false, 0, 0, "invalid_range", "device DMA range overflow", {}};
        }

        const uint64_t host_addr = req.host_buffer->device_addr() + req.host_offset;
        if ((host_addr % DMA_ALIGNMENT) != 0 ||
            (req.device_offset % DMA_ALIGNMENT) != 0 ||
            (req.size_bytes % DMA_ALIGNMENT) != 0) {
            return {false, 0, 0, "invalid_alignment",
                    "HQC DMA addresses and size must be 32-byte aligned", {}};
        }

        HqcAdminCommand command = {};
        command.header.cmd_type = HQC_ADMIN_CMD_TEST;
        command.header.cmd_id = 0; // TODO: allocate and correlate cmd_id for concurrent requests.
        command.payload.test.test_type = host_to_device
                                             ? HQC_TEST_DMA_HOST_TO_DMEM
                                             : HQC_TEST_DMA_DMEM_TO_HOST;
        auto& dma = command.payload.test.submit.dma;
        dma.src_offset = host_to_device ? host_addr : req.device_offset;
        dma.dst_offset = host_to_device ? req.device_offset : host_addr;
        dma.size = static_cast<uint32_t>(req.size_bytes);

        const char* direction = host_to_device ? "h2d" : "d2h";
        if (ti.logger != nullptr) {
            std::ostringstream message;
            message << "submit HQC DMA " << direction
                    << " size=" << req.size_bytes
                    << " host_addr=0x" << std::hex << host_addr
                    << " dmem_offset=0x" << req.device_offset;
            ti.logger->info(message.str());
        }

        const auto start = std::chrono::steady_clock::now();
        HqcAdminQueue queue(ctx_.device_ctx, ti.logger);
        int status = queue.push(command, req.timeout_ms);
        if (status != HQC_STATUS_OK) {
            return {false, 0, 0, "sq_push_failed",
                    "HQC admin SQ push failed, status=" + std::to_string(status), {}};
        }

        HqcAdminCommand completion = {};
        status = queue.pop(completion, req.timeout_ms);
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
        if (status != HQC_STATUS_OK) {
            return {false, 0, static_cast<uint64_t>(duration), "cq_pop_failed",
                    "HQC admin CQ pop failed, status=" + std::to_string(status), {}};
        }
        if (completion.header.cmd_status != HQC_STATUS_OK) {
            if (ti.logger != nullptr) {
                ti.logger->error("HQC DMA completion returned cmd_status=" +
                                 std::to_string(completion.header.cmd_status));
            }
            return {false, 0, static_cast<uint64_t>(duration), "fw_error",
                    "HQC DMA command failed, cmd_status=" +
                        std::to_string(completion.header.cmd_status), {}};
        }

        if (ti.logger != nullptr) {
            ti.logger->info(std::string("HQC DMA ") + direction + " completed");
        }
        DmaTransferResult result;
        result.ok = true;
        result.bytes = req.size_bytes;
        result.duration_us = static_cast<uint64_t>(duration);
        result.completion_status = "completed";
        result.metrics = {
            {"dma_direction", direction},
            {"dma_addr_kind", req.host_buffer->addr_kind()}
        };
        return result;
    }
};

}

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasPCIeImpl>(ctx);
}

}
