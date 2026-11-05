#pragma once

#include "diag/core/IpcBridge.h"

#include <iostream>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <phal/phal.h>
}

using TestArgs = std::unordered_map<std::string, std::string>;

class HalContext;

enum class CtxResourceType {
    Ipc,
};

struct CtxResource {
    CtxResourceType ctx_type;
    std::optional<IpcFwId> ipc_fw;

    static CtxResource ipc(IpcFwId fw)
    {
        return {CtxResourceType::Ipc, fw};
    }

};

// Optional contexts declared by a testcase at registration. The default
// register context is supplied by the executable and needs no declaration.
class CtxResources {
private:
    std::vector<CtxResource> resources_;

public:
    CtxResources() = default;
    CtxResources(std::initializer_list<CtxResource> resources)
        : resources_(resources)
    {
    }

    bool requires_ipc(IpcFwId fw) const
    {
        for (const auto& resource : resources_) {
            if (resource.ctx_type == CtxResourceType::Ipc &&
                resource.ipc_fw.has_value() &&
                *resource.ipc_fw == fw) return true;
        }
        return false;
    }

};

// Testcases and PHAL share one status type and bit layout.
using TestStatus = phal_status_t;
static_assert(PHAL_STATUS_OK == 0 && PHAL_STATUS_TIMEOUT == 1 &&
              PHAL_STATUS_ERROR == 2 && PHAL_STATUS_UNIMPLEMENTED == 4 &&
              PHAL_STATUS_INVALID == 8,
              "PHAL status bits differ from the diagnostic status contract");

constexpr TestStatus operator|(TestStatus lhs, TestStatus rhs)
{
    return static_cast<TestStatus>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

inline TestStatus& operator|=(TestStatus& lhs, TestStatus rhs)
{
    return lhs = lhs | rhs;
}

inline std::string test_status_name(TestStatus status)
{
    if (status == PHAL_STATUS_OK) return "OK";
    std::string result;
    const auto append = [&](const char* name) {
        if (!result.empty()) result += '|';
        result += name;
    };
    const auto bits = static_cast<unsigned>(status);
    if (bits & static_cast<unsigned>(PHAL_STATUS_TIMEOUT)) append("TIMEOUT");
    if (bits & static_cast<unsigned>(PHAL_STATUS_ERROR)) append("ERROR");
    if (bits & static_cast<unsigned>(PHAL_STATUS_UNIMPLEMENTED)) append("UNIMPLEMENTED");
    if (bits & static_cast<unsigned>(PHAL_STATUS_INVALID)) append("INVALID");
    if (bits & ~15u) {
        append("UNKNOWN");
    }
    return result;
}

enum class LogLevel {
    Error = 0,
    Info = 1,
    Flow = 2,
    RegIo = 3,
    // Backward-compatible names. New code should use Flow and RegIo.
    Debug = Flow,
    Trace = RegIo,
};

class Logger {
private:
    LogLevel level_ = LogLevel::Info;

    bool should_log(LogLevel message_level) const
    {
        return static_cast<int>(message_level) <= static_cast<int>(level_);
    }

    void write(LogLevel message_level,
               const std::string& level_name,
               const std::string& message) const
    {
        if (!should_log(message_level)) {
            return;
        }

        std::cout << "[" << level_name << "] " << message << std::endl;
    }

public:
    void set_level(LogLevel level) { level_ = level; }
    LogLevel get_level() const { return level_; }

    void error(const std::string& message) const { write(LogLevel::Error, "ERROR", message); }
    void info(const std::string& message) const { write(LogLevel::Info, "INFO", message); }
    void flow(const std::string& message) const { write(LogLevel::Flow, "FLOW", message); }
    void regio(const std::string& message) const { write(LogLevel::RegIo, "REGIO", message); }
    void debug(const std::string& message) const { flow(message); }
    void trace(const std::string& message) const { regio(message); }
};

enum class RegisterTransport { Pcie, Uart };

struct TestInfo {
    std::string target_name;
    std::string test_name;
    TestArgs args;
    Logger* logger = nullptr;
    HalContext* hal = nullptr;
    // Framework-owned default register context. PCIe provides BAR/RCF ops;
    // isml_uart provides UART ops.
    phal_ctx_t* phal_ctx = nullptr;
    RegisterTransport transport = RegisterTransport::Pcie;
    // PCIe-only resource APIs and optional FW IPC contexts are accessed via hal.
};

inline TestStatus make_unimplemented_status(const TestInfo& ti,
                                             const std::string& reason)
{
    if (ti.logger != nullptr) {
        ti.logger->error(reason);
    }
    return PHAL_STATUS_UNIMPLEMENTED;
}
