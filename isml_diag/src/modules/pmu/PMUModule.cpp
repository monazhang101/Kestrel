#include "diag/module/PMUModule.h"

#include "diag/core/Common.h"

#include <utility>

PMUModule::PMUModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<PMUImpl> impl)
    : BaseDevice(name, ctx),
      bar_index_(config.bar_index),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    auto* bar_base = static_cast<uint8_t*>(common::bar::mapped_base(ctx_, bar_index_));
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("pmu_ipc_request_start", [this](TestInfo& ti) { return PmuIpcRequestStart(ti); });
    _add_test("pmu_ipc_request_exec", [this](TestInfo& ti) { return PmuIpcRequestExec(ti); });
    _add_test("pmu_ipc_request_finish", [this](TestInfo& ti) { return PmuIpcRequestFinish(ti); });
    _add_test("pmu_reg_read", [this](TestInfo& ti) { return PmuRegRead(ti); });
    _add_test("pmu_reg_write", [this](TestInfo& ti) { return PmuRegWrite(ti); });
    _add_test("pmu_reg_check", [this](TestInfo& ti) { return PmuRegCheck(ti); });
}
