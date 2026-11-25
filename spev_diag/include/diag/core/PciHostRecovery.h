#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

struct PciResourceRow {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t flags = 0;

    bool operator==(const PciResourceRow& other) const;
};

struct PciUpstreamBridge {
    std::string bdf;
    std::vector<uint64_t> windows;

    bool operator==(const PciUpstreamBridge& other) const;
};

struct PciCapabilityEntry {
    uint16_t offset = 0;
    uint16_t id = 0;

    bool operator==(const PciCapabilityEntry& other) const;
};

struct PciHostEnvironment {
    std::string boot_id;
    uint64_t sysfs_inode = 0;
    std::string canonical_path;
    uint64_t config_size = 0;
    std::array<PciResourceRow, 6> resources{};
    std::vector<PciUpstreamBridge> bridges;

    bool operator==(const PciHostEnvironment& other) const;
};

struct PciHostSnapshot {
    uint32_t format = 1;
    std::string target;
    std::string bdf;
    std::string driver;
    bool driver_was_bound = false;
    std::string created_utc;
    std::vector<uint8_t> config;
    PciHostEnvironment environment;
    std::array<uint32_t, 16> header{};
    std::vector<PciCapabilityEntry> capabilities;
    std::vector<PciCapabilityEntry> extended_capabilities;
    uint16_t pcie_flags = 0;
    uint16_t pcie_control = 0;
    uint32_t pcie_devcap = 0;
};

struct PciHostRecoveryPaths {
    std::filesystem::path pci_devices = "/sys/bus/pci/devices";
    std::filesystem::path pci_drivers = "/sys/bus/pci/drivers";
    std::filesystem::path boot_id = "/proc/sys/kernel/random/boot_id";
    std::filesystem::path lock_directory = "/run/lock";
};

class PciHostRecoveryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PciHostRecovery {
public:
    class DeviceLock {
    public:
        DeviceLock() = default;
        ~DeviceLock();
        DeviceLock(const DeviceLock&) = delete;
        DeviceLock& operator=(const DeviceLock&) = delete;
        DeviceLock(DeviceLock&& other) noexcept;
        DeviceLock& operator=(DeviceLock&& other) noexcept;

    private:
        friend class PciHostRecovery;
        explicit DeviceLock(int fd) : fd_(fd) {}
        int fd_ = -1;
    };

    explicit PciHostRecovery(PciHostRecoveryPaths paths = {});

    DeviceLock lock_device(const std::string& bdf) const;
    PciHostSnapshot capture(const std::string& target,
                            const std::string& bdf) const;
    void save_new(const std::filesystem::path& output,
                  const PciHostSnapshot& snapshot) const;
    PciHostSnapshot load(const std::filesystem::path& input) const;

    void unbind_driver(const PciHostSnapshot& snapshot) const;
    void bind_driver(const PciHostSnapshot& snapshot) const;
    void make_unbound_device_safe(const PciHostSnapshot& snapshot) const;
    void restore(const PciHostSnapshot& snapshot,
                 std::chrono::milliseconds timeout,
                 const std::function<void(const std::string&)>& report = {}) const;
    void enforce_mmio_only_command(const std::string& bdf) const;

private:
    PciHostRecoveryPaths paths_;
};
