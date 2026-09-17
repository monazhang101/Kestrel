#include "atlas/pcie/hqc_admin_queue.h"
#include "generic/generic_impl.h"

#include <chrono>
#include <limits>
#include <sstream>
#include <utility>

namespace atlas_impl {
namespace {

// FW length is in 32-byte units; host addresses must also be 32-byte aligned.
constexpr uint64_t DMA_ALIGNMENT = 32;
// FW converts device byte offsets to 44-bit word offsets (4 bytes per word).
// This is descriptor representability, not the physical memory capacity.
constexpr uint64_t DEVICE_ADDRESS_LIMIT = uint64_t{1} << 46;
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

    TestStatus dma_copy(TestInfo& ti,
                        const DmaTransferRequest& req) override
    {
        uint32_t test_type = 0;
        const char* direction = nullptr;
        switch (req.type) {
        case DmaTransferType::H2D:
            test_type = HQC_TEST_DMA_HOST_TO_DMEM; direction = "h2d"; break;
        case DmaTransferType::D2H:
            test_type = HQC_TEST_DMA_DMEM_TO_HOST; direction = "d2h"; break;
        case DmaTransferType::D2I:
            test_type = HQC_TEST_DMA_DMEM_TO_IMEM; direction = "d2i"; break;
        case DmaTransferType::I2D:
            test_type = HQC_TEST_DMA_IMEM_TO_DMEM; direction = "i2d"; break;
        case DmaTransferType::D2S:
            test_type = HQC_TEST_DMA_DMEM_TO_SMEM; direction = "d2s"; break;
        case DmaTransferType::S2D:
            test_type = HQC_TEST_DMA_SMEM_TO_DMEM; direction = "s2d"; break;
        case DmaTransferType::D2V:
        case DmaTransferType::V2D:
            // VMEM cannot be tested on the current platform. Keep the paths
            // and ABI constants, but never submit a placeholder to hardware.
            return make_unimplemented_status(ti, "VMEM DMA is not enabled");
        default:
            return fail(ti, TestStatus::INVALID, "unknown DMA transfer type");
        }
        if (req.size_bytes == 0 ||
            req.size_bytes > std::numeric_limits<uint32_t>::max() ||
            (req.size_bytes % DMA_ALIGNMENT) != 0 || req.timeout_ms == 0) {
            return fail(ti, TestStatus::INVALID,
                        "DMA size must fit uint32 and be a nonzero multiple of 32; timeout must be nonzero");
        }

        const bool host_source = req.type == DmaTransferType::H2D;
        const bool host_destination = req.type == DmaTransferType::D2H;
        // Device endpoints are byte offsets. Reject low bits that FW would
        // otherwise silently discard during byte-to-word conversion.
        const auto valid_device_range = [&](uint64_t offset) {
            return offset % 4 == 0 && offset < DEVICE_ADDRESS_LIMIT &&
                   req.size_bytes <= DEVICE_ADDRESS_LIMIT - offset;
        };
        if ((!host_source && !valid_device_range(req.src_offset)) ||
            (!host_destination && !valid_device_range(req.dst_offset))) {
            return fail(ti, TestStatus::INVALID,
                        "device DMA range must be 4-byte aligned and fit the FW word address");
        }

        uint64_t source = req.src_offset;
        uint64_t destination = req.dst_offset;
        if (host_source || host_destination) {
            if (req.host_buffer == nullptr || !req.host_buffer->valid() ||
                req.host_buffer->addr_kind() != "dma_addr") {
                return fail(ti, TestStatus::INVALID,
                            "host DMA requires a valid isml_kmod buffer");
            }
            const auto host_offset = host_source ? req.src_offset : req.dst_offset;
            if (req.size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
                host_offset > req.host_buffer->size() ||
                req.size_bytes > req.host_buffer->size() - host_offset ||
                req.host_buffer->device_addr() >
                    std::numeric_limits<uint64_t>::max() - host_offset) {
                return fail(ti, TestStatus::INVALID, "host DMA range is invalid");
            }
            const auto host_addr = req.host_buffer->device_addr() + host_offset;
            if (host_addr % DMA_ALIGNMENT != 0 ||
                req.size_bytes - 1 > std::numeric_limits<uint64_t>::max() - host_addr) {
                return fail(ti, TestStatus::INVALID,
                            "host DMA address must be 32-byte aligned and not overflow");
            }
            if (host_source) {
                source = host_addr;
            } else {
                destination = host_addr;
            }
        } else if (req.host_buffer != nullptr) {
            return fail(ti, TestStatus::INVALID,
                        "device-only DMA must not carry a host buffer");
        }

        HqcAdminCommand command = {};
        command.header.cmd_type = HQC_ADMIN_CMD_TEST;
        command.header.cmd_id = 0; // TODO: allocate and correlate cmd_id for concurrent requests.
        command.payload.test.test_type = test_type;
        auto& dma = command.payload.test.submit.dma;
        dma.src_offset = source;
        dma.dst_offset = destination;
        dma.size = static_cast<uint32_t>(req.size_bytes);

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
                    << (req.host_buffer != nullptr ? req.host_buffer->addr_kind()
                                                   : "device_byte_offset");
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
