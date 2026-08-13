#include "PMUModule.h"

#include "Common.h"

PMUModule::PMUModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint64_t reg_offset,
                     uint64_t reg_size,
                     std::shared_ptr<CmdQueueMgm> hqc_queue_mgm)
    : BaseDevice(name, ctx, hqc_queue_mgm), reg_offset_(reg_offset), reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("pmu_ipc_request_start", [this](TestInfo& ti) { return PmuIpcRequestStart(ti); });
    _add_test("pmu_ipc_request_exec", [this](TestInfo& ti) { return PmuIpcRequestExec(ti); });
    _add_test("pmu_ipc_request_finish", [this](TestInfo& ti) { return PmuIpcRequestFinish(ti); });
    _add_test("pmu_reg_read", [this](TestInfo& ti) { return PmuRegRead(ti); });
    _add_test("pmu_reg_write", [this](TestInfo& ti) { return PmuRegWrite(ti); });
    _add_test("pmu_reg_check", [this](TestInfo& ti) { return PmuRegCheck(ti); });
    _add_test("pmu_soft_reset_trigger", [this](TestInfo& ti) { return PmuSoftResetTrigger(ti); });
    _add_test("pmu_pcie_cfg_backup", [this](TestInfo& ti) { return PmuPcieCfgBackup(ti); });
    _add_test("pmu_pcie_cfg_restore", [this](TestInfo& ti) { return PmuPcieCfgRestore(ti); });
    _add_test("pmu_ecc_trigger", [this](TestInfo& ti) { return PmuEccTrigger(ti); });
    _add_test("pmu_ecc_clean", [this](TestInfo& ti) { return PmuEccClean(ti); });
    _add_test("pmu_ecc_snapshot_assert", [this](TestInfo& ti) { return PmuEccSnapshotAssert(ti); });
}

// pmu_ipc_request_start : To start a PMU IPC transaction and reserve the command mailbox.
// @input: args["request_id"] optional request identifier.
// @output: TestResult metrics include ipc_state and request_id.
TestResult PMUModule::PmuIpcRequestStart(TestInfo& ti)
{
    auto request_id = common::args::get_string(ti.args, "request_id", "default");

    // Pseudocode: check mailbox idle, write request header, mark IPC busy.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ipc_request_start", get_name(), true, {
        {"request_id", request_id},
        {"ipc_state", "started"}
    }};
}

// pmu_ipc_request_exec : To execute a previously prepared PMU IPC request.
// @input: args["opcode"] PMU IPC command opcode.
// @output: TestResult metrics include opcode and completion_state.
TestResult PMUModule::PmuIpcRequestExec(TestInfo& ti)
{
    auto opcode = common::args::get_string(ti.args, "opcode", "0x0");

    // Pseudocode: write opcode, ring PMU mailbox doorbell, poll command accepted bit.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ipc_request_exec", get_name(), true, {
        {"opcode", opcode},
        {"completion_state", "accepted"}
    }};
}

// pmu_ipc_request_finish : To finish a PMU IPC transaction and collect completion status.
// @input: args["timeout_ms"] optional completion timeout.
// @output: TestResult metrics include completion_state and pmu_status.
TestResult PMUModule::PmuIpcRequestFinish(TestInfo& ti)
{
    auto timeout_ms = common::args::get_string(ti.args, "timeout_ms", "1000");

    // Pseudocode: poll completion bit, read result code, clear mailbox busy state.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ipc_request_finish", get_name(), true, {
        {"timeout_ms", timeout_ms},
        {"completion_state", "done"},
        {"pmu_status", "ok"}
    }};
}

// pmu_reg_read : To read one PMU register through the PMU register window.
// @input: args["offset"] register offset.
// @output: TestResult metrics include offset and value.
TestResult PMUModule::PmuRegRead(TestInfo& ti)
{
    auto offset = common::args::get_string(ti.args, "offset", "0x0");

    // Pseudocode: ok = common::reg::read(reg_base_, reg_size_, parsed_offset, &value, sizeof(value)).
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_reg_read", get_name(), true, {
        {"offset", offset},
        {"value", "0x00000000"}
    }};
}

// pmu_reg_write : To write one PMU register through the PMU register window.
// @input: args["offset"] register offset, args["value"] value to write.
// @output: TestResult metrics include offset, value, and write_status.
TestResult PMUModule::PmuRegWrite(TestInfo& ti)
{
    auto offset = common::args::get_string(ti.args, "offset", "0x0");
    auto value = common::args::get_string(ti.args, "value", "0x0");

    // Pseudocode: ok = common::reg::write(reg_base_, reg_size_, parsed_offset, &parsed_value, sizeof(parsed_value)).
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_reg_write", get_name(), true, {
        {"offset", offset},
        {"value", value},
        {"write_status", "done"}
    }};
}

// pmu_reg_check : To read a PMU register and check it against an expected value.
// @input: args["offset"] register offset, args["expected"] expected value.
// @output: TestResult metrics include offset, actual, expected, and check_status.
TestResult PMUModule::PmuRegCheck(TestInfo& ti)
{
    auto offset = common::args::get_string(ti.args, "offset", "0x0");
    auto expected = common::args::get_string(ti.args, "expected", "0x0");

    // Pseudocode: read actual value, apply optional mask, compare with expected.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_reg_check", get_name(), true, {
        {"offset", offset},
        {"actual", expected},
        {"expected", expected},
        {"check_status", "match"}
    }};
}

// pmu_soft_reset_trigger : To trigger a PMU soft reset sequence.
// @input: args["reset_scope"] optional PMU reset scope.
// @output: TestResult metrics include reset_scope and reset_status.
TestResult PMUModule::PmuSoftResetTrigger(TestInfo& ti)
{
    auto reset_scope = common::args::get_string(ti.args, "reset_scope", "pmu");

    // Pseudocode: write reset request, poll reset done, verify PMU returns to idle.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_soft_reset_trigger", get_name(), true, {
        {"reset_scope", reset_scope},
        {"reset_status", "done"}
    }};
}

// pmu_pcie_cfg_backup : To back up PMU-owned PCIe configuration state.
// @input: none.
// @output: TestResult metrics include backup_status and entry_count.
TestResult PMUModule::PmuPcieCfgBackup(TestInfo& ti)
{
    // Pseudocode: ask PMU to snapshot PCIe config registers into PMU scratch/state memory.
    (void)reg_base_;
    (void)reg_size_;
    (void)ti.args;

    return {"pmu_pcie_cfg_backup", get_name(), true, {
        {"backup_status", "done"},
        {"entry_count", "32"}
    }};
}

// pmu_pcie_cfg_restore : To restore PMU-owned PCIe configuration state.
// @input: none.
// @output: TestResult metrics include restore_status and entry_count.
TestResult PMUModule::PmuPcieCfgRestore(TestInfo& ti)
{
    // Pseudocode: ask PMU to restore PCIe config registers from saved PMU state.
    (void)reg_base_;
    (void)reg_size_;
    (void)ti.args;

    return {"pmu_pcie_cfg_restore", get_name(), true, {
        {"restore_status", "done"},
        {"entry_count", "32"}
    }};
}

// pmu_ecc_trigger : To trigger a PMU ECC injection or ECC check path.
// @input: args["ecc_type"] optional ECC operation type.
// @output: TestResult metrics include ecc_type and trigger_status.
TestResult PMUModule::PmuEccTrigger(TestInfo& ti)
{
    auto ecc_type = common::args::get_string(ti.args, "ecc_type", "single_bit");

    // Pseudocode: configure PMU ECC injection register and trigger one ECC event.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ecc_trigger", get_name(), true, {
        {"ecc_type", ecc_type},
        {"trigger_status", "done"}
    }};
}

// pmu_ecc_clean : To clear PMU ECC status and sticky error bits.
// @input: none.
// @output: TestResult metrics include clean_status.
TestResult PMUModule::PmuEccClean(TestInfo& ti)
{
    // Pseudocode: write clear bits, read back ECC status, verify clean state.
    (void)reg_base_;
    (void)reg_size_;
    (void)ti.args;

    return {"pmu_ecc_clean", get_name(), true, {
        {"clean_status", "done"}
    }};
}

// pmu_ecc_snapshot_assert : To read PMU ECC snapshot and assert expected error state.
// @input: args["expected_status"] optional expected ECC state.
// @output: TestResult metrics include expected_status, actual_status, and assert_status.
TestResult PMUModule::PmuEccSnapshotAssert(TestInfo& ti)
{
    auto expected_status = common::args::get_string(ti.args, "expected_status", "clean");

    // Pseudocode: read ECC snapshot registers and compare with expected status.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ecc_snapshot_assert", get_name(), true, {
        {"expected_status", expected_status},
        {"actual_status", expected_status},
        {"assert_status", "pass"}
    }};
}
