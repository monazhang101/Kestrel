#include "diag/modules/PMUModule.h"

#include <utility>

PMUModule::PMUModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<PMUImpl> impl)
    : TestTarget(name, "pmu", ctx),
      bar_index_(config.bar_index),
      reg_base_offset_(config.reg_base_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    _add_test("ipc_request_start", {{"request_id", "0", "u64"}},
              [this](TestInfo& ti) { return ipc_request_start(ti); });
    _add_test("ipc_request_exec", {{"opcode", "0", "u64"}},
              [this](TestInfo& ti) { return ipc_request_exec(ti); });
    _add_test("ipc_request_finish", {{"timeout_ms", "1000", "ms"}},
              [this](TestInfo& ti) { return ipc_request_finish(ti); });
    _add_test("reg_read", {{"offset", "0", "offset"}},
              [this](TestInfo& ti) { return reg_read(ti); });
    _add_test("reg_write",
              {{"offset", "0", "offset"}, {"value", "0", "u64"}},
              [this](TestInfo& ti) { return reg_write(ti); });
    _add_test("reg_check",
              {{"offset", "0", "offset"}, {"expected", "0", "u64"}},
              [this](TestInfo& ti) { return reg_check(ti); });
}
