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
// Relative to PCIe TOP, not BAR0. Atlas TOP is at BAR0 + 0x36000000,
// so the host-visible HQC SRAM window starts at BAR0 + 0x36d00000.
constexpr uint64_t ATL_HQC_SRAM_PCIE_OFFSET = 0xd00000;

TestStatus fail(TestInfo& ti, TestStatus status, const std::string& message)
{
    if (ti.logger != nullptr) {
        ti.logger->error(message);
    }
    return status;
}

TestStatus hqc_status(int status)
{
    return status == HQC_STATUS_TIMEOUT ? TestStatus::TIMEOUT
                                        : TestStatus::ERROR;
}

class AtlasPCIeImpl : public generic_impl::GenericPCIeImpl {
public:
    explicit AtlasPCIeImpl(ModuleImplContext ctx)
        : generic_impl::GenericPCIeImpl(std::move(ctx))
    {
    }

    TestStatus dma_copy_h2d(TestInfo& ti,
                            const DmaTransferRequest& req) override
    {
        return dma_copy(ti, req, true);
    }

    TestStatus dma_copy_d2h(TestInfo& ti,
                            const DmaTransferRequest& req) override
    {
        return dma_copy(ti, req, false);
    }

private:
    TestStatus dma_copy(TestInfo& ti,
                        const DmaTransferRequest& req,
                        bool host_to_device)
    {
        if (req.host_buffer == nullptr || !req.host_buffer->valid()) {
            return fail(ti, TestStatus::INVALID, "host DMA buffer is invalid");
        }
        if (req.host_buffer->addr_kind() != "dma_addr") {
            return fail(ti, TestStatus::INVALID,
                        "HQC DMA requires a buffer allocated by isml_kmod");
        }
        if (req.size_bytes == 0 || req.size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
            req.size_bytes > std::numeric_limits<uint32_t>::max() ||
            req.host_offset > req.host_buffer->size() ||
            req.size_bytes > req.host_buffer->size() - req.host_offset) {
            return fail(ti, TestStatus::INVALID, "DMA request range is invalid");
        }
        if (req.host_buffer->device_addr() >
            std::numeric_limits<uint64_t>::max() - req.host_offset) {
            return fail(ti, TestStatus::INVALID, "host DMA address overflow");
        }
        if (req.device_offset >
            std::numeric_limits<uint64_t>::max() - req.size_bytes) {
            return fail(ti, TestStatus::INVALID, "device DMA range overflow");
        }

        const uint64_t host_addr = req.host_buffer->device_addr() + req.host_offset;
        if ((host_addr % DMA_ALIGNMENT) != 0 ||
            (req.device_offset % DMA_ALIGNMENT) != 0 ||
            (req.size_bytes % DMA_ALIGNMENT) != 0) {
            return fail(ti, TestStatus::INVALID,
                        "HQC DMA addresses and size must be 32-byte aligned");
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
            message << "HQC DMA command"
                    << "\n       direction              = " << direction
                    << "\n       descriptor_source      = 0x" << std::hex
                    << dma.src_offset
                    << "\n       descriptor_destination = 0x"
                    << dma.dst_offset
                    << "\n       size_bytes             = " << std::dec
                    << req.size_bytes;
            ti.logger->debug(message.str());
        }

        const auto start = std::chrono::steady_clock::now();
        HqcAdminQueue queue(ctx_.device_ctx,
                            ctx_.reg_base_offset + ATL_HQC_SRAM_PCIE_OFFSET,
                            ti.logger);
        int status = queue.push(command, req.timeout_ms);
        if (status != HQC_STATUS_OK) {
            return fail(ti, hqc_status(status),
                        "HQC admin SQ push failed, status=" + std::to_string(status));
        }

        HqcAdminCommand completion = {};
        status = queue.pop(completion, req.timeout_ms);
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
        if (status != HQC_STATUS_OK) {
            return fail(ti, hqc_status(status),
                        "HQC admin CQ pop failed, status=" + std::to_string(status) +
                            " duration_us=" + std::to_string(duration));
        }
        if (completion.header.cmd_status != HQC_STATUS_OK) {
            if (ti.logger != nullptr) {
                ti.logger->error("HQC DMA completion returned cmd_status=" +
                                 std::to_string(completion.header.cmd_status));
            }
            return TestStatus::ERROR;
        }

        if (ti.logger != nullptr) {
            std::ostringstream message;
            message << "HQC DMA completed"
                    << "\n       direction      = " << direction
                    << "\n       size_bytes     = " << req.size_bytes
                    << "\n       duration_us    = " << duration
                    << "\n       address_kind   = "
                    << req.host_buffer->addr_kind();
            ti.logger->info(message.str());
        }
        return TestStatus::OK;
    }
};

}

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasPCIeImpl>(ctx);
}

}
