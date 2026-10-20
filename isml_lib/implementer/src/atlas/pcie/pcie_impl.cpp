#include "diag/implementer/HqcDma.h"
#include "generic/generic_impl.h"

#include <limits>
#include <string>

namespace atlas_impl {
namespace {

TestStatus fail(TestInfo& ti, TestStatus status, const std::string& message)
{
    if (ti.logger != nullptr) ti.logger->error(message);
    return status;
}

DmaProfile profile_for(DmaTransferType type)
{
    switch (type) {
    case DmaTransferType::H2D: return DmaProfile::H2D;
    case DmaTransferType::D2H: return DmaProfile::D2H;
    case DmaTransferType::D2I: return DmaProfile::D2I;
    case DmaTransferType::I2D: return DmaProfile::I2D;
    case DmaTransferType::D2S: return DmaProfile::D2S;
    case DmaTransferType::S2D: return DmaProfile::S2D;
    case DmaTransferType::D2V: return DmaProfile::D2V;
    case DmaTransferType::V2D: return DmaProfile::V2D;
    }
    return DmaProfile::Default;
}

void set_device_endpoint(DmaDescriptor& desc, bool source,
                         DmaTransferType type, uint64_t offset)
{
    DmaMemory memory = DmaMemory::Dmem;
    uint32_t core = 1;
    uint32_t mem = 0;
    switch (type) {
    case DmaTransferType::D2I:
        if (!source) { memory = DmaMemory::Imem; core = 2; mem = 2; }
        break;
    case DmaTransferType::I2D:
        if (source) { memory = DmaMemory::Imem; core = 2; mem = 2; }
        break;
    case DmaTransferType::D2S:
        if (!source) { memory = DmaMemory::Smem; core = 2; mem = 1; }
        break;
    case DmaTransferType::S2D:
        if (source) { memory = DmaMemory::Smem; core = 2; mem = 1; }
        break;
    case DmaTransferType::D2V:
        if (!source) { memory = DmaMemory::Vmem; core = 2; mem = 0; }
        break;
    case DmaTransferType::V2D:
        if (source) { memory = DmaMemory::Vmem; core = 2; mem = 0; }
        break;
    default: break;
    }
    if (source) desc.set_src(memory, core, mem, offset);
    else desc.set_dst(memory, core, mem, offset);
}

class AtlasPCIeImpl final : public generic_impl::GenericPCIeImpl {
public:
    TestStatus dma_copy(TestInfo& ti, const DmaTransferRequest& req) override
    {
        if (ti.hal == nullptr)
            return fail(ti, PHAL_STATUS_ERROR, "DMA requires a bound HAL context");
        if (req.size_bytes == 0 || req.size_bytes % 32 != 0 ||
            req.size_bytes > std::numeric_limits<uint32_t>::max() || req.timeout_ms == 0) {
            return fail(ti, PHAL_STATUS_INVALID,
                        "DMA size must be a nonzero multiple of 32 and timeout must be nonzero");
        }

        const bool h2d = req.type == DmaTransferType::H2D;
        const bool d2h = req.type == DmaTransferType::D2H;
        if ((h2d || d2h) && (req.host_buffer == nullptr || !req.host_buffer->valid()))
            return fail(ti, PHAL_STATUS_INVALID, "host DMA requires a valid host DMA buffer");

        HqcDmaEngine engine(*ti.hal);
        DmaDescriptor desc(engine, profile_for(req.type));
        TestStatus status = PHAL_STATUS_OK;
        if (h2d) {
            status = desc.set_src(*req.host_buffer, req.src_offset);
            if (status == PHAL_STATUS_OK)
                set_device_endpoint(desc, false, req.type, req.dst_offset);
        } else if (d2h) {
            set_device_endpoint(desc, true, req.type, req.src_offset);
            status = desc.set_dst(*req.host_buffer, req.dst_offset);
        } else {
            set_device_endpoint(desc, true, req.type, req.src_offset);
            set_device_endpoint(desc, false, req.type, req.dst_offset);
        }
        if (status != PHAL_STATUS_OK) return status;
        status = desc.set_size(req.size_bytes);
        if (status != PHAL_STATUS_OK) return status;

        if (ti.logger != nullptr) {
            ti.logger->flow("DMA descriptor prepared: size_bytes=" +
                            std::to_string(req.size_bytes) +
                            " sync_flag=" + std::to_string(desc.sync_flag()));
        }
        return desc.execute(req.timeout_ms);
    }
};

}

std::unique_ptr<PCIeImpl> make_pcie_impl()
{
    return std::make_unique<AtlasPCIeImpl>();
}

}
