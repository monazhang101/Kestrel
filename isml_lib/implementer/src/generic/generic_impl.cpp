#include "generic_impl.h"

#include "diag/core/Common.h"
#include "diag/core/Format.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace generic_impl {

// Generic PCIe impl. This is the fallback for BAR and DMA operations when
// a product-specific PCIe implementation does not provide an override.
GenericPCIeImpl::GenericPCIeImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestStatus GenericPCIeImpl::bar_read32(TestInfo& ti)
{
    const auto offset = common::args::get_u64(ti.args, "offset");

    if ((offset % sizeof(uint32_t)) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR read offset must be 4-byte aligned: offset=" +
                             common::format::hex(offset));
        }
        return TestStatus::INVALID;
    }

    uint32_t value = 0;
    const uint32_t bar_index = ctx_.bar_index;
    if (offset > ctx_.reg_size ||
        sizeof(value) > static_cast<size_t>(ctx_.reg_size - offset)) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR read is outside module range\n"
                "       module_offset       = " + common::format::hex(offset) +
                "\n       module_size         = " + common::format::hex(ctx_.reg_size));
        }
        return TestStatus::INVALID;
    }

    if (ctx_.reg_base_offset >
        std::numeric_limits<uint64_t>::max() - offset) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR read module offset overflow");
        }
        return TestStatus::INVALID;
    }
    const uint64_t absolute_offset = ctx_.reg_base_offset + offset;
    if (!common::bar::read32(ctx_.device_ctx,
                             bar_index,
                             absolute_offset,
                             value)) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR read failed\n"
                "       bar_index           = " + std::to_string(bar_index) +
                "\n       module_offset       = " + common::format::hex(offset) +
                "\n       absolute_bar_offset = " + common::format::hex(absolute_offset));
        }
        return TestStatus::ERROR;
    }

    if (ti.logger != nullptr) {
        ti.logger->info(
            "BAR read completed\n"
            "       bar_index           = " + std::to_string(bar_index) +
            "\n       module_offset       = " + common::format::hex(offset) +
            " (" + std::to_string(offset) + ")" +
            "\n       absolute_bar_offset = " + common::format::hex(absolute_offset) +
            " (" + std::to_string(absolute_offset) + ")" +
            "\n       value               = " + common::format::hex(value, 8) +
            " (" + std::to_string(value) + ")");
    }
    return TestStatus::OK;
}

TestStatus GenericPCIeImpl::bar_scan32(TestInfo& ti)
{
    constexpr uint64_t MAX_WORDS = 256;

    const auto offset = common::args::get_u64(ti.args, "offset");
    const auto words = common::args::get_u64(ti.args, "words");

    if ((offset % sizeof(uint32_t)) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR scan offset must be 4-byte aligned: offset=" +
                             common::format::hex(offset));
        }
        return TestStatus::INVALID;
    }
    if (words == 0 || words > MAX_WORDS) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR scan words must be in range 1.." +
                             std::to_string(MAX_WORDS));
        }
        return TestStatus::INVALID;
    }

    std::vector<uint32_t> values(static_cast<size_t>(words), 0);
    const auto bytes = words * sizeof(uint32_t);
    const uint32_t bar_index = ctx_.bar_index;
    if (offset > ctx_.reg_size || bytes > ctx_.reg_size - offset) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR scan is outside module range\n"
                "       module_offset       = " + common::format::hex(offset) +
                "\n       size_bytes          = " + std::to_string(bytes) +
                "\n       module_size         = " + common::format::hex(ctx_.reg_size));
        }
        return TestStatus::INVALID;
    }

    if (ctx_.reg_base_offset >
        std::numeric_limits<uint64_t>::max() - offset) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR scan module offset overflow");
        }
        return TestStatus::INVALID;
    }
    const uint64_t absolute_offset = ctx_.reg_base_offset + offset;
    if (!common::bar::read(ctx_.device_ctx,
                           bar_index,
                           absolute_offset,
                           values.data(),
                           static_cast<size_t>(bytes))) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR scan failed\n"
                "       bar_index           = " + std::to_string(bar_index) +
                "\n       module_offset       = " + common::format::hex(offset) +
                "\n       absolute_bar_offset = " + common::format::hex(absolute_offset) +
                "\n       size_bytes          = " + std::to_string(bytes));
        }
        return TestStatus::ERROR;
    }

    if (ti.logger != nullptr) {
        ti.logger->info(
            "BAR scan completed\n"
            "       bar_index           = " + std::to_string(bar_index) +
            "\n       module_offset       = " + common::format::hex(offset) +
            " (" + std::to_string(offset) + ")" +
            "\n       absolute_bar_offset = " + common::format::hex(absolute_offset) +
            " (" + std::to_string(absolute_offset) + ")" +
            "\n       word_count          = " + std::to_string(words));
        for (size_t i = 0; i < values.size(); ++i) {
            ti.logger->debug(
                "BAR scan word[" + std::to_string(i) + "]" +
                " address=" +
                common::format::hex(absolute_offset + i * sizeof(uint32_t)) +
                " value=" + common::format::hex(values[i], 8));
        }
    }
    return TestStatus::OK;
}

TestStatus GenericPCIeImpl::dma_copy(TestInfo& ti,
                                         const DmaTransferRequest& req)
{
    (void)req;
    return make_unimplemented_status(
        ti, "PCIe DMA copy is not implemented for " + ctx_.target_name);
}

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<GenericPCIeImpl>(ctx);
}

}
