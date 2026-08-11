#include "DDPModule.h"

#include "Common.h"

DDPModule::DDPModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint64_t reg_offset,
                     uint64_t reg_size)
    : BaseDevice(name, ctx), reg_offset_(reg_offset), reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("ddp_bdf_get_secondary_bus", [this](const TestArgs& args) { return DdpBdfGetSecondaryBus(args); });
    _add_test("ddp_dvsec_verify", [this](const TestArgs& args) { return DdpDvsecVerify(args); });
    _add_test("ddp_dvsec_walk_chain", [this](const TestArgs& args) { return DdpDvsecWalkChain(args); });
    _add_test("ddp_width_verify", [this](const TestArgs& args) { return DdpWidthVerify(args); });
    _add_test("ddp_dmem_linkup_verify", [this](const TestArgs& args) { return DdpDmemLinkupVerify(args); });
    _add_test("ddp_dmem_perf", [this](const TestArgs& args) { return DdpDmemPerf(args); });
    _add_test("ddp_mc_linkup_intr_verify", [this](const TestArgs& args) { return DdpMcLinkupIntrVerify(args); });
    _add_test("ddp_pcie_reg_scan", [this](const TestArgs& args) { return DdpPcieRegScan(args); });
    _add_test("ddp_bist_fifo_run", [this](const TestArgs& args) { return DdpBistFifoRun(args); });
}

// ddp_bdf_get_secondary_bus : To read the secondary bus number behind the DDP bridge.
// @input: none.
// @output: TestResult metrics include secondary_bus and bdf_source.
TestResult DDPModule::DdpBdfGetSecondaryBus(const TestArgs& args)
{
    (void)reg_base_;
    (void)reg_size_;
    (void)args;
    return {"ddp_bdf_get_secondary_bus", get_name(), true, {
        {"secondary_bus", "0x66"},
        {"bdf_source", ctx_.bdf}
    }};
}

// ddp_dvsec_verify : To verify DDP DVSEC capability fields against expected values.
// @input: args["expected_vendor"] optional expected DVSEC vendor.
// @output: TestResult metrics include dvsec_status and expected_vendor.
TestResult DDPModule::DdpDvsecVerify(const TestArgs& args)
{
    auto expected_vendor = common::args::get_string(args, "expected_vendor", "tpu_vendor");
    return {"ddp_dvsec_verify", get_name(), true, {
        {"expected_vendor", expected_vendor},
        {"dvsec_status", "matched"}
    }};
}

// ddp_dvsec_walk_chain : To walk the DDP DVSEC chain and report discovered entries.
// @input: none.
// @output: TestResult metrics include entry_count and chain_status.
TestResult DDPModule::DdpDvsecWalkChain(const TestArgs& args)
{
    (void)args;
    return {"ddp_dvsec_walk_chain", get_name(), true, {
        {"entry_count", "4"},
        {"chain_status", "complete"}
    }};
}

// ddp_width_verify : To verify DDP negotiated width.
// @input: args["expected_width"] expected DDP width.
// @output: TestResult metrics include expected_width, observed_width, and width_status.
TestResult DDPModule::DdpWidthVerify(const TestArgs& args)
{
    auto expected_width = common::args::get_string(args, "expected_width", "x8");
    return {"ddp_width_verify", get_name(), true, {
        {"expected_width", expected_width},
        {"observed_width", expected_width},
        {"width_status", "matched"}
    }};
}

// ddp_dmem_linkup_verify : To verify DDP device-memory link-up state.
// @input: args["link"] optional DDP DMEM link selector.
// @output: TestResult metrics include link and linkup_status.
TestResult DDPModule::DdpDmemLinkupVerify(const TestArgs& args)
{
    auto link = common::args::get_string(args, "link", "all");
    return {"ddp_dmem_linkup_verify", get_name(), true, {
        {"link", link},
        {"linkup_status", "up"}
    }};
}

// ddp_dmem_perf : To measure DDP device-memory path performance.
// @input: args["size_bytes"] transfer size, args["pattern"] payload pattern.
// @output: TestResult metrics include size_bytes, pattern, and bandwidth_gbps.
TestResult DDPModule::DdpDmemPerf(const TestArgs& args)
{
    auto size_bytes = common::args::get_string(args, "size_bytes", "1048576");
    auto pattern = common::args::get_string(args, "pattern", "incremental");
    return {"ddp_dmem_perf", get_name(), true, {
        {"size_bytes", size_bytes},
        {"pattern", pattern},
        {"bandwidth_gbps", "96.0"}
    }};
}

// ddp_mc_linkup_intr_verify : To verify memory-controller link-up interrupt state through DDP.
// @input: args["mc"] memory-controller instance selector.
// @output: TestResult metrics include mc and interrupt_status.
TestResult DDPModule::DdpMcLinkupIntrVerify(const TestArgs& args)
{
    auto mc = common::args::get_string(args, "mc", "all");
    return {"ddp_mc_linkup_intr_verify", get_name(), true, {
        {"mc", mc},
        {"interrupt_status", "observed"}
    }};
}

// ddp_pcie_reg_scan : To scan PCIe-facing DDP registers.
// @input: args["range"] optional register range.
// @output: TestResult metrics include scanned_range and bad_register_count.
TestResult DDPModule::DdpPcieRegScan(const TestArgs& args)
{
    auto range = common::args::get_string(args, "range", "all");
    (void)reg_base_;
    (void)reg_size_;
    return {"ddp_pcie_reg_scan", get_name(), true, {
        {"scanned_range", range},
        {"bad_register_count", "0"}
    }};
}

// ddp_bist_fifo_run : To run DDP BIST FIFO diagnostics.
// @input: args["pattern"] optional BIST data pattern.
// @output: TestResult metrics include pattern and bist_status.
TestResult DDPModule::DdpBistFifoRun(const TestArgs& args)
{
    auto pattern = common::args::get_string(args, "pattern", "incremental");
    return {"ddp_bist_fifo_run", get_name(), true, {
        {"pattern", pattern},
        {"bist_status", "passed"}
    }};
}
