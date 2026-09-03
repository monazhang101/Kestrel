#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class PhalProject {
    Generic,
    Atlas,
    AtlasM,
};

PhalProject phal_project_from_tpu_type(TPUType tpu_type);

struct IhalIO {
    const DeviceContext* device_ctx = nullptr;
    uint32_t bar_index = 0;
    uint64_t module_base = 0;
};

class PhalBridge {
private:
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    PhalBridge();
    ~PhalBridge();

    PhalBridge(const PhalBridge&) = delete;
    PhalBridge& operator=(const PhalBridge&) = delete;
    PhalBridge(PhalBridge&&) noexcept;
    PhalBridge& operator=(PhalBridge&&) noexcept;

    void reset();
    bool init(const IhalIO& ihalIO, PhalProject project, std::string* error = nullptr);
    bool initialized() const;
    void* native_context() const;
};

struct DevMemSpec {
    uint32_t control_bar_index = 0;
    uint64_t control_module_base = 0;
    uint8_t dmem_bar_index = 4;
    uint8_t aperture_index = 0;
    uint8_t identity = 0;
    uint64_t target_addr = 0;
    uint64_t size = 0;
    uint64_t aperture_bar_offset = 0;
    bool readonly = false;
    std::string pattern;
    PhalProject project = PhalProject::Generic;
};

class Logger;

class DevMem {
private:
    DeviceContext ctx_;
    DevMemSpec spec_;
    PhalBridge* phal_ = nullptr;
    Logger* logger_ = nullptr;
    bool valid_ = false;
    std::string error_;

    bool range_ok(uint64_t offset, size_t len) const;
    void log_info(const std::string& message) const;
    void log_debug(const std::string& message) const;
    void log_error(const std::string& message) const;

public:
    DevMem() = default;
    DevMem(DeviceContext ctx, DevMemSpec spec, PhalBridge* phal, Logger* logger);

    static DevMem invalid(std::string error);

    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }

    bool configure();

    uint8_t dmem_bar_index() const { return spec_.dmem_bar_index; }
    uint8_t aperture_index() const { return spec_.aperture_index; }
    uint64_t target_addr() const { return spec_.target_addr; }
    uint64_t size() const { return spec_.size; }
    uint64_t aperture_bar_offset() const { return spec_.aperture_bar_offset; }
    bool readonly() const { return spec_.readonly; }

    bool read(uint64_t offset, void* data, size_t len) const;
    bool write(uint64_t offset, const void* data, size_t len) const;
};
