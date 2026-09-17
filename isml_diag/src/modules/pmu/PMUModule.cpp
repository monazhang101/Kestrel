#include "diag/module/PMUModule.h"

#include <utility>

PMUModule::PMUModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<PMUImpl> impl)
    : BaseDevice(name, ctx),
      bar_index_(config.bar_index),
      reg_base_offset_(config.reg_base_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    _add_test("pmu_ipc_request_start", {{"request_id", "0", "u64"}},
              [this](TestInfo& ti) { return PmuIpcRequestStart(ti); });
    _add_test("pmu_ipc_request_exec", {{"opcode", "0", "u64"}},
              [this](TestInfo& ti) { return PmuIpcRequestExec(ti); });
    _add_test("pmu_ipc_request_finish", {{"timeout_ms", "1000", "ms"}},
              [this](TestInfo& ti) { return PmuIpcRequestFinish(ti); });
    _add_test("pmu_reg_read", {{"offset", "0", "offset"}},
              [this](TestInfo& ti) { return PmuRegRead(ti); });
    _add_test("pmu_reg_write",
              {{"offset", "0", "offset"}, {"value", "0", "u64"}},
              [this](TestInfo& ti) { return PmuRegWrite(ti); });
    _add_test("pmu_reg_check",
              {{"offset", "0", "offset"}, {"expected", "0", "u64"}},
              [this](TestInfo& ti) { return PmuRegCheck(ti); });
}
