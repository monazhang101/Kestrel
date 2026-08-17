#include "diag/module/ISIModule.h"

#include "diag/core/Common.h"

ISIModule::ISIModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint32_t link_id,
                     uint64_t reg_offset,
                     uint64_t reg_size)
    : BaseDevice(name, ctx),
      link_id_(link_id),
      reg_offset_(reg_offset),
      reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("isi_linkup", [this](TestInfo& ti) { return IsiLinkup(ti); });
    _add_test("isi_setup", [this](TestInfo& ti) { return IsiSetup(ti); });
    _add_test("isi_isr_tbl_write_enable", [this](TestInfo& ti) { return IsiIsrTblWriteEnable(ti); });
    _add_test("isi_isr_tbl_write_disable", [this](TestInfo& ti) { return IsiIsrTblWriteDisable(ti); });
    _add_test("isi_isr_tbl_set", [this](TestInfo& ti) { return IsiIsrTblSet(ti); });
    _add_test("isi_isr_tbl_direct_set", [this](TestInfo& ti) { return IsiIsrTblDirectSet(ti); });
    _add_test("isi_isr_tbl_get", [this](TestInfo& ti) { return IsiIsrTblGet(ti); });
    _add_test("isi_pcs_loopback", [this](TestInfo& ti) { return IsiPcsLoopback(ti); });
    _add_test("isi_phy_loopback", [this](TestInfo& ti) { return IsiPhyLoopback(ti); });
    _add_test("isi_phy_bist_test", [this](TestInfo& ti) { return IsiPhyBistTest(ti); });
    _add_test("isi_ifc_intr_enable", [this](TestInfo& ti) { return IsiIfcIntrEnable(ti); });
    _add_test("isi_ifc_intr_is_set", [this](TestInfo& ti) { return IsiIfcIntrIsSet(ti); });
    _add_test("isi_ifc_node_id_set", [this](TestInfo& ti) { return IsiIfcNodeIdSet(ti); });
    _add_test("isi_xdc_addr_set", [this](TestInfo& ti) { return IsiXdcAddrSet(ti); });
    _add_test("isi_send_packet", [this](TestInfo& ti) { return IsiSendPacket(ti); });
    _add_test("isi_reg_scan", [this](TestInfo& ti) { return IsiRegScan(ti); });
    _add_test("isi_poll_credit_regs", [this](TestInfo& ti) { return IsiPollCreditRegs(ti); });
    _add_test("isi_dump_debug_regs", [this](TestInfo& ti) { return IsiDumpDebugRegs(ti); });
    _add_test("isi_pcie_pmu_intr_is_set", [this](TestInfo& ti) { return IsiPciePmuIntrIsSet(ti); });
}

// isi_linkup : To check whether this ISI link is physically present and link-up.
// @input: none.
// @output: TestResult metrics include link_id, link_status, lane_ready_bitmap, and error_count.
TestResult ISIModule::IsiLinkup(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;

    // Pseudocode: read present, training done, lane ready, and error counter registers.
    (void)ti.args;

    return {"isi_linkup", get_name(), true, {
        {"link_id", std::to_string(link_id_)},
        {"link_status", "up"},
        {"lane_ready_bitmap", "0xff"},
        {"error_count", "0"}
    }};
}

// isi_setup : To initialize ISI link configuration for this ISI instance.
// @input: args["mode"] optional setup mode.
// @output: TestResult metrics include link_id, mode, and setup_status.
TestResult ISIModule::IsiSetup(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto mode = common::args::get_string(ti.args, "mode", "default");

    // Pseudocode: program link mode, lane config, timeout, and enable training.

    return {"isi_setup", get_name(), true, {
        {"link_id", std::to_string(link_id_)},
        {"mode", mode},
        {"setup_status", "done"}
    }};
}

// isi_isr_tbl_write_enable : To enable writes to the ISI ISR table.
// @input: none.
// @output: TestResult metrics include write_enable_status.
TestResult ISIModule::IsiIsrTblWriteEnable(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;

    // Pseudocode: set ISR table write-enable bit and verify it is enabled.
    (void)ti.args;

    return {"isi_isr_tbl_write_enable", get_name(), true, {{"write_enable_status", "enabled"}}};
}

// isi_isr_tbl_write_disable : To disable writes to the ISI ISR table.
// @input: none.
// @output: TestResult metrics include write_enable_status.
TestResult ISIModule::IsiIsrTblWriteDisable(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;

    // Pseudocode: clear ISR table write-enable bit and verify it is disabled.
    (void)ti.args;

    return {"isi_isr_tbl_write_disable", get_name(), true, {{"write_enable_status", "disabled"}}};
}

// isi_isr_tbl_set : To set one ISI ISR table entry.
// @input: args["entry"], args["value"].
// @output: TestResult metrics include entry, value, and set_status.
TestResult ISIModule::IsiIsrTblSet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto entry = common::args::get_string(ti.args, "entry", "0");
    auto value = common::args::get_string(ti.args, "value", "0x0");

    // Pseudocode: write ISR table entry through indirect table access.

    return {"isi_isr_tbl_set", get_name(), true, {
        {"entry", entry},
        {"value", value},
        {"set_status", "done"}
    }};
}

// isi_isr_tbl_direct_set : To set one ISI ISR table entry through direct access.
// @input: args["entry"], args["value"].
// @output: TestResult metrics include entry, value, and set_status.
TestResult ISIModule::IsiIsrTblDirectSet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto entry = common::args::get_string(ti.args, "entry", "0");
    auto value = common::args::get_string(ti.args, "value", "0x0");

    // Pseudocode: write ISR table entry through direct table register address.

    return {"isi_isr_tbl_direct_set", get_name(), true, {
        {"entry", entry},
        {"value", value},
        {"set_status", "done"}
    }};
}

// isi_isr_tbl_get : To read one ISI ISR table entry.
// @input: args["entry"].
// @output: TestResult metrics include entry and value.
TestResult ISIModule::IsiIsrTblGet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto entry = common::args::get_string(ti.args, "entry", "0");

    // Pseudocode: read ISR table entry and return its raw value.

    return {"isi_isr_tbl_get", get_name(), true, {
        {"entry", entry},
        {"value", "0x00000000"}
    }};
}

// isi_pcs_loopback : To run an ISI PCS loopback sanity check.
// @input: args["pattern"] optional loopback pattern.
// @output: TestResult metrics include pattern and loopback_status.
TestResult ISIModule::IsiPcsLoopback(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto pattern = common::args::get_string(ti.args, "pattern", "prbs31");

    // Pseudocode: enable PCS loopback, send pattern, check PCS counters, disable loopback.

    return {"isi_pcs_loopback", get_name(), true, {
        {"pattern", pattern},
        {"loopback_status", "pass"}
    }};
}

// isi_phy_loopback : To run an ISI PHY loopback sanity check.
// @input: args["pattern"] optional loopback pattern.
// @output: TestResult metrics include pattern and loopback_status.
TestResult ISIModule::IsiPhyLoopback(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto pattern = common::args::get_string(ti.args, "pattern", "prbs31");

    // Pseudocode: enable PHY loopback, send pattern, check PHY counters, disable loopback.

    return {"isi_phy_loopback", get_name(), true, {
        {"pattern", pattern},
        {"loopback_status", "pass"}
    }};
}

// isi_phy_bist_test : To run ISI PHY built-in self-test.
// @input: args["duration_ms"] optional BIST duration.
// @output: TestResult metrics include duration_ms and bist_status.
TestResult ISIModule::IsiPhyBistTest(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto duration_ms = common::args::get_string(ti.args, "duration_ms", "100");

    // Pseudocode: trigger PHY BIST, poll completion, read BIST result code.

    return {"isi_phy_bist_test", get_name(), true, {
        {"duration_ms", duration_ms},
        {"bist_status", "pass"}
    }};
}

// isi_ifc_intr_enable : To enable ISI IFC interrupt reporting.
// @input: args["mask"] optional interrupt mask.
// @output: TestResult metrics include mask and enable_status.
TestResult ISIModule::IsiIfcIntrEnable(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto mask = common::args::get_string(ti.args, "mask", "all");

    // Pseudocode: write IFC interrupt enable mask and verify readback.

    return {"isi_ifc_intr_enable", get_name(), true, {
        {"mask", mask},
        {"enable_status", "enabled"}
    }};
}

// isi_ifc_intr_is_set : To query whether ISI IFC interrupt status is set.
// @input: args["mask"] optional interrupt mask.
// @output: TestResult metrics include mask and interrupt_status.
TestResult ISIModule::IsiIfcIntrIsSet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto mask = common::args::get_string(ti.args, "mask", "all");

    // Pseudocode: read IFC interrupt status register and compare requested mask.

    return {"isi_ifc_intr_is_set", get_name(), true, {
        {"mask", mask},
        {"interrupt_status", "not_set"}
    }};
}

// isi_ifc_node_id_set : To program ISI IFC node ID.
// @input: args["node_id"] node identifier.
// @output: TestResult metrics include node_id and set_status.
TestResult ISIModule::IsiIfcNodeIdSet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto node_id = common::args::get_string(ti.args, "node_id", "0");

    // Pseudocode: write node ID register and verify readback.

    return {"isi_ifc_node_id_set", get_name(), true, {
        {"node_id", node_id},
        {"set_status", "done"}
    }};
}

// isi_xdc_addr_set : To program ISI XDC address mapping.
// @input: args["xdc_addr"] XDC address value.
// @output: TestResult metrics include xdc_addr and set_status.
TestResult ISIModule::IsiXdcAddrSet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto xdc_addr = common::args::get_string(ti.args, "xdc_addr", "0x0");

    // Pseudocode: write XDC address mapping register and verify readback.

    return {"isi_xdc_addr_set", get_name(), true, {
        {"xdc_addr", xdc_addr},
        {"set_status", "done"}
    }};
}

// isi_send_packet : To send one ISI packet through this ISI link.
// @input: args["packet_size"], args["pattern"].
// @output: TestResult metrics include packet_size, pattern, and send_status.
TestResult ISIModule::IsiSendPacket(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto packet_size = common::args::get_string(ti.args, "packet_size", "256");
    auto pattern = common::args::get_string(ti.args, "pattern", "incremental");

    // Pseudocode: build packet payload, program send descriptor, trigger send, poll done.

    return {"isi_send_packet", get_name(), true, {
        {"packet_size", packet_size},
        {"pattern", pattern},
        {"send_status", "done"}
    }};
}

// isi_reg_scan : To scan readable ISI registers for debug and sanity.
// @input: args["range"] optional register range.
// @output: TestResult metrics include range and scanned_registers.
TestResult ISIModule::IsiRegScan(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto range = common::args::get_string(ti.args, "range", "default");

    // Pseudocode: iterate safe register offsets, read values, store summary/artifact.

    return {"isi_reg_scan", get_name(), true, {
        {"range", range},
        {"scanned_registers", "64"}
    }};
}

// isi_poll_credit_regs : To poll ISI credit registers until expected credit state appears.
// @input: args["timeout_ms"] optional timeout.
// @output: TestResult metrics include timeout_ms and credit_status.
TestResult ISIModule::IsiPollCreditRegs(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto timeout_ms = common::args::get_string(ti.args, "timeout_ms", "1000");

    // Pseudocode: poll credit registers, stop on expected credit state or timeout.

    return {"isi_poll_credit_regs", get_name(), true, {
        {"timeout_ms", timeout_ms},
        {"credit_status", "ready"}
    }};
}

// isi_dump_debug_regs : To dump ISI debug registers for diagnosis.
// @input: args["range"] optional debug range.
// @output: TestResult metrics include range and dump_status.
TestResult ISIModule::IsiDumpDebugRegs(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto range = common::args::get_string(ti.args, "range", "debug");

    // Pseudocode: read debug registers and write a structured artifact in real code.

    return {"isi_dump_debug_regs", get_name(), true, {
        {"range", range},
        {"dump_status", "done"}
    }};
}

// isi_pcie_pmu_intr_is_set : To query ISI-visible PCIe PMU interrupt status.
// @input: args["mask"] optional interrupt mask.
// @output: TestResult metrics include mask and interrupt_status.
TestResult ISIModule::IsiPciePmuIntrIsSet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto mask = common::args::get_string(ti.args, "mask", "all");

    // Pseudocode: read ISI interrupt aggregation register for PCIe/PMU status.

    return {"isi_pcie_pmu_intr_is_set", get_name(), true, {
        {"mask", mask},
        {"interrupt_status", "not_set"}
    }};
}
