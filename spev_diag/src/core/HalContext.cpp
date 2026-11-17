#include "diag/core/HalContext.h"
#include "diag/core/Common.h"
#include "diag/core/DevMem.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
}

namespace {

constexpr std::array<uint32_t, 3> PROBE_BAR_INDICES = {0, 2, 4};

// Standard PCI/PCIe configuration-space fields used by the host access path.
// Type-1 Bridge Control bit 6 is the standard Secondary Bus Reset control.
constexpr off_t PCI_COMMAND_OFFSET = 0x04;
constexpr uint16_t PCI_COMMAND_MEMORY_SPACE = 0x0002;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 0x0004;
constexpr uint16_t PCI_COMMAND_REQUIRED_BITS =
    PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
constexpr off_t PCI_HEADER_TYPE_OFFSET = 0x0e;             // Common Header Type.
constexpr uint8_t PCI_HEADER_TYPE_MASK = 0x7f;             // Ignore multifunction bit 7.
constexpr uint8_t PCI_HEADER_TYPE_BRIDGE = 0x01;           // PCI-to-PCI bridge header.
constexpr off_t PCI_BRIDGE_CONTROL_OFFSET = 0x3e;          // Type-1 Bridge Control.
constexpr uint16_t PCI_BRIDGE_CONTROL_BUS_RESET = 0x0040;  // Secondary Bus Reset, bit 6.

std::string bar_pair_name(uint32_t bar_index)
{
    return "bar" + std::to_string(bar_index) + std::to_string(bar_index + 1);
}

uint64_t expected_bar_size(uint32_t bar_index)
{
    constexpr uint64_t mib = 1024ull * 1024ull;
    constexpr uint64_t gib = 1024ull * mib;

    if (bar_index == 0) {
        return 1ull * gib;
    }
    if (bar_index == 2 || bar_index == 4) {
        return 512ull * gib;
    }
    return 0;
}

std::vector<std::string> bar_layout(uint32_t bar_index)
{
    if (bar_index == 0) {
        return {
            "2MiB MSI-X",
            "126MiB indirect CSR",
            "128MiB debug",
            "256MiB doorbell",
            "512MiB RCF"
        };
    }
    if (bar_index == 2) {
        return {
            "256GiB RDMA",
            "256GiB DF",
            "8 apertures: aperture0 256MiB VMEM/SMEM readonly, aperture1-7 configurable DMEM"
        };
    }
    if (bar_index == 4) {
        return {
            "512GiB DF",
            "8 configurable DMEM apertures"
        };
    }
    return {};
}

BarMapping make_bar_mapping(uint32_t bar_index)
{
    BarMapping bar;
    bar.bar_index = bar_index;
    bar.name = bar_pair_name(bar_index);
    bar.expected_size = expected_bar_size(bar_index);
    bar.layout = bar_layout(bar_index);
    return bar;
}

std::string setpci_enable_command(const std::string& bdf)
{
    return "sudo setpci -s " + bdf + " COMMAND=0006:0006";
}

bool upstream_bridge_config(const std::string& endpoint_bdf,
                            std::filesystem::path& config_path,
                            std::string& bridge_bdf,
                            std::string& error)
{
    // The endpoint's parent in sysfs is its immediate upstream PCI bridge.
    std::error_code filesystem_error;
    const auto endpoint_path = std::filesystem::canonical(
        std::filesystem::path("/sys/bus/pci/devices") / endpoint_bdf,
        filesystem_error);
    if (filesystem_error) {
        error = "resolve PCI device failed for " + endpoint_bdf + ": " +
                filesystem_error.message();
        return false;
    }

    const auto bridge_path = endpoint_path.parent_path();
    config_path = bridge_path / "config";
    if (bridge_path.empty() ||
        !std::filesystem::exists(config_path, filesystem_error) ||
        filesystem_error) {
        error = "upstream PCI bridge config is unavailable for " + endpoint_bdf;
        return false;
    }

    bridge_bdf = bridge_path.filename().string();
    return true;
}

bool is_permission_error(int error_code)
{
    return error_code == EACCES || error_code == EPERM;
}

// Ensure MMIO and bus mastering are enabled before BAR mmap.
// When sysfs config writes are blocked, report the exact setpci command
// needed to enable PCI COMMAND.Mem+ and BusMaster for this device.
bool ensure_pci_mmio_enabled(const std::string& bdf, std::string& error)
{
    const auto config_path = "/sys/bus/pci/devices/" + bdf + "/config";

    int fd = open(config_path.c_str(), O_RDONLY);
    if (fd < 0) {
        auto open_errno = errno;
        error = "open PCI config failed for " + bdf + ": " + std::strerror(open_errno);
        if (is_permission_error(open_errno)) {
            error += "; run: " + setpci_enable_command(bdf);
        }
        return false;
    }

    uint16_t command = 0;
    auto bytes = pread(fd, &command, sizeof(command), PCI_COMMAND_OFFSET);
    auto read_errno = errno;
    close(fd);

    if (bytes != static_cast<ssize_t>(sizeof(command))) {
        error = "read PCI COMMAND failed for " + bdf + ": " + std::strerror(read_errno);
        if (is_permission_error(read_errno)) {
            error += "; run: " + setpci_enable_command(bdf);
        }
        return false;
    }

    if ((command & PCI_COMMAND_REQUIRED_BITS) == PCI_COMMAND_REQUIRED_BITS) {
        return true;
    }

    fd = open(config_path.c_str(), O_RDWR);
    if (fd < 0) {
        auto open_errno = errno;
        error = "PCI COMMAND.Mem+ or BusMaster is disabled for " + bdf +
                ", and enabling it failed: " + std::strerror(open_errno) +
                "; run: " + setpci_enable_command(bdf);
        return false;
    }

    const uint16_t enabled_command = command | PCI_COMMAND_REQUIRED_BITS;
    bytes = pwrite(fd, &enabled_command, sizeof(enabled_command), PCI_COMMAND_OFFSET);
    auto write_errno = errno;
    close(fd);

    if (bytes != static_cast<ssize_t>(sizeof(enabled_command))) {
        error = "enable PCI COMMAND.Mem+ and BusMaster failed for " + bdf +
                ": " + std::strerror(write_errno) +
                "; run: " + setpci_enable_command(bdf);
        return false;
    }

    return true;
}

bool read_hex_u16(const std::filesystem::path& path, uint16_t& value)
{
    std::ifstream input(path);
    std::string text;
    if (!std::getline(input, text)) {
        return false;
    }

    try {
        value = static_cast<uint16_t>(std::stoul(text, nullptr, 0));
        return true;
    } catch (...) {
        return false;
    }
}

bool read_resource_info(const std::string& bdf,
                        uint32_t bar_index,
                        uint64_t& start,
                        uint64_t& size_bytes)
{
    std::ifstream input("/sys/bus/pci/devices/" + bdf + "/resource");
    std::string line;
    for (uint32_t i = 0; i <= bar_index; ++i) {
        if (!std::getline(input, line)) {
            return false;
        }
    }

    uint64_t end = 0;
    uint64_t flags = 0;
    std::istringstream stream(line);
    if (!(stream >> std::hex >> start >> end >> flags)) {
        return false;
    }
    if (end < start) {
        return false;
    }
    if (start == 0 && end == 0 && flags == 0) {
        return false;
    }

    size_bytes = end - start + 1;
    return size_bytes != 0;
}

}

HalContext::HalContext() = default;

void HalContext::bind_device(const DeviceContext& ctx, Logger* logger)
{
    // A HalContext represents one currently bound physical device. Rebinding
    // releases contexts tied to the previous BAR mapping before replacing it.
    phal_context_ = {};
    aperture_phal_context_ = {};
    for (auto& ctx : ipc_contexts_) ctx = {};
    active_device_ = ctx;
    active_device_.logger = logger != nullptr ? logger : ctx.logger;
    active_memory_regions_ = ctx.memory_regions;
    active_memory_ = ctx.memory;
    if (active_memory_ == nullptr) {
        active_memory_ = std::make_shared<DeviceMemoryState>();
    }
    active_device_.memory = active_memory_;
    active_device_.memory_regions = active_memory_regions_;
    host_dma_mode_ = HostDmaMode::SpevDrv;
    device_bound_ = true;
}

HalContext::TestcaseScope HalContext::begin_testcase(
    std::string* error, const CtxResources& ctx_resources)
{
    if (error != nullptr) error->clear();
    if (!device_bound_) {
        if (error != nullptr) *error = "HAL is not bound to a device";
        return {};
    }
    const auto any_ipc_context_active = [this]() {
        for (const auto& ctx : ipc_contexts_) {
            if (ctx) return true;
        }
        return false;
    };
    if (phal_context_ || aperture_phal_context_ || any_ipc_context_active()) {
        if (error != nullptr) *error = "a testcase context is already active";
        return {};
    }

    std::string init_error;
    const auto project = phal_project_from_tpu_type(active_device_.tpu_type);
    phal_context_ = phal_bridge_.create_context(active_device_, project, &init_error);
    if (!phal_context_) {
        if (error != nullptr) *error = "root PHAL initialization failed: " + init_error;
        return {};
    }

    init_error.clear();
    aperture_phal_context_ =
        phal_bridge_.create_context(active_device_, project, &init_error);
    if (!aperture_phal_context_) {
        phal_context_ = {};
        if (error != nullptr) {
            *error = "aperture PHAL initialization failed: " + init_error;
        }
        return {};
    }

    for (uint32_t id = IPC_FW_ID_FIRST; id <= IPC_FW_ID_LAST; id++) {
        const auto fw = static_cast<IpcFwId>(id);
        if (!ctx_resources.requires_ipc(fw)) continue;
        // A fw whose region row is still a placeholder is skipped, not
        // failed. ipc_context(fw) returns nullptr until it is provisioned.
        if (!ipc_region_config(fw).provisioned) continue;
        init_error.clear();
        ipc_contexts_[id - IPC_FW_ID_FIRST] =
            phal_bridge_.create_ipc_context(active_device_, project, fw,
                                            &init_error);
        if (!ipc_contexts_[id - IPC_FW_ID_FIRST]) {
            // Roll everything back so a retry starts from a clean state.
            phal_context_ = {};
            aperture_phal_context_ = {};
            for (auto& ctx : ipc_contexts_) ctx = {};
            if (error != nullptr) {
                *error = "IPC PHAL initialization failed (fw=" +
                         std::string(to_string(fw)) + "): " + init_error;
            }
            return {};
        }
    }

    const auto* dmem = find_df_region(active_memory_regions_.df, DMEM);
    if (dmem != nullptr && dmem->supported &&
        aperture_set(*this, dmem->bar_index, dmem->aperture_index,
                     dmem->base, dmem->aperture_size,
                     dmem->identity) != PHAL_STATUS_OK) {
        phal_context_ = {};
        aperture_phal_context_ = {};
        for (auto& ctx : ipc_contexts_) ctx = {};
        if (error != nullptr) {
            *error = "default DMEM aperture initialization failed";
        }
        return {};
    }

    return TestcaseScope(this);
}

HalContext::TestcaseScope::~TestcaseScope()
{
    if (owner_ != nullptr) {
        owner_->phal_context_ = {};
        owner_->aperture_phal_context_ = {};
        for (auto& ctx : owner_->ipc_contexts_) ctx = {};
    }
}

HalContext::TestcaseScope::TestcaseScope(TestcaseScope&& other) noexcept
    : owner_(other.owner_)
{
    other.owner_ = nullptr;
}

HalContext::TestcaseScope& HalContext::TestcaseScope::operator=(TestcaseScope&& other) noexcept
{
    if (this != &other) {
        if (owner_ != nullptr) {
            owner_->phal_context_ = {};
            owner_->aperture_phal_context_ = {};
            for (auto& ctx : owner_->ipc_contexts_) ctx = {};
        }
        owner_ = other.owner_;
        other.owner_ = nullptr;
    }
    return *this;
}

const DeviceContext& HalContext::device_context() const
{
    return active_device_;
}

bool HalContext::read_bar32(uint32_t bar_index,
                            uint64_t offset,
                            uint32_t& value) const
{
    return device_bound_ &&
           common::bar::read32(active_device_, bar_index, offset, value);
}

bool HalContext::write_bar32(uint32_t bar_index,
                             uint64_t offset,
                             uint32_t value) const
{
    return device_bound_ &&
           common::bar::write32(active_device_, bar_index, offset, value);
}

TestStatus HalContext::with_pcie_context(
    const std::function<TestStatus(phal_ctx_t*)>& operation,
    std::string* error)
{
    if (error != nullptr) error->clear();
    if (!operation) {
        if (error != nullptr) *error = "PCIe operation callback is empty";
        return PHAL_STATUS_INVALID;
    }
    if (!device_bound_ || !aperture_phal_context_) {
        if (error != nullptr) *error = "PCIe PHAL context is not initialized";
        return PHAL_STATUS_ERROR;
    }

    std::lock_guard<std::mutex> lock(active_memory_->pcie_context_mutex);
    auto* phal = aperture_phal_context_.get();
    phal_block_ctx_enter(phal, TOP_U_PCIE_0_OFFSET);
    TestStatus status = PHAL_STATUS_ERROR;
    try {
        status = operation(phal);
    } catch (...) {
        phal_block_ctx_exit(phal, TOP_U_PCIE_0_OFFSET);
        phal->env.base = PhalBridge::CHIP_RCF_BASE;
        throw;
    }
    phal_block_ctx_exit(phal, TOP_U_PCIE_0_OFFSET);
    // Keep the reusable context at the chip root even if a callback left a
    // nested block active after returning.
    phal->env.base = PhalBridge::CHIP_RCF_BASE;
    return status;
}

TestStatus HalContext::secondary_bus_reset(bool reset)
{
    const auto fail = [&](TestStatus status, const std::string& message) {
        if (active_device_.logger != nullptr) {
            active_device_.logger->error(message);
        }
        return status;
    };

    if (!device_bound_ || active_device_.bdf.empty()) {
        return fail(PHAL_STATUS_INVALID,
                    "secondary bus reset requires a bound PCI device");
    }
    // Resolve the selected TPU endpoint to its immediate upstream bridge.
    std::filesystem::path config_path;
    std::string bridge_bdf;
    std::string resolve_error;
    if (!upstream_bridge_config(active_device_.bdf,
                                config_path,
                                bridge_bdf,
                                resolve_error)) {
        return fail(PHAL_STATUS_ERROR, resolve_error);
    }

    // Bridge Control is accessed through the bridge's PCI config-space file.
    const int fd = open(config_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail(PHAL_STATUS_ERROR,
                    "open upstream PCI bridge config failed for " + bridge_bdf +
                    ": " + std::strerror(errno));
    }

    // Secondary Bus Reset is defined only in the Type-1 bridge header.
    uint8_t header_type = 0;
    auto bytes = pread(fd, &header_type, sizeof(header_type), PCI_HEADER_TYPE_OFFSET);
    if (bytes != static_cast<ssize_t>(sizeof(header_type))) {
        const auto read_error = errno;
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "read PCI header type failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(read_error))
                               : ": short read"));
    }
    if ((header_type & PCI_HEADER_TYPE_MASK) != PCI_HEADER_TYPE_BRIDGE) {
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "upstream PCI device is not a Type-1 bridge: " + bridge_bdf);
    }

    // Preserve every Bridge Control field except the requested reset bit.
    uint16_t bridge_control = 0;
    bytes = pread(fd,
                  &bridge_control,
                  sizeof(bridge_control),
                  PCI_BRIDGE_CONTROL_OFFSET);
    if (bytes != static_cast<ssize_t>(sizeof(bridge_control))) {
        const auto read_error = errno;
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "read PCI Bridge Control failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(read_error))
                               : ": short read"));
    }

    const uint16_t requested = reset
        ? static_cast<uint16_t>(bridge_control | PCI_BRIDGE_CONTROL_BUS_RESET)
        : static_cast<uint16_t>(bridge_control & ~PCI_BRIDGE_CONTROL_BUS_RESET);
    // Write the requested reset state to the standard Bridge Control register.
    bytes = pwrite(fd, &requested, sizeof(requested), PCI_BRIDGE_CONTROL_OFFSET);
    if (bytes != static_cast<ssize_t>(sizeof(requested))) {
        const auto write_error = errno;
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "write PCI Bridge Control failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(write_error))
                               : ": short write"));
    }

    // Read the register back and verify that bit 6 reached the requested state.
    uint16_t verified = 0;
    bytes = pread(fd, &verified, sizeof(verified), PCI_BRIDGE_CONTROL_OFFSET);
    const auto verify_error = errno;
    close(fd);
    if (bytes != static_cast<ssize_t>(sizeof(verified))) {
        return fail(PHAL_STATUS_ERROR,
                    "verify PCI Bridge Control failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(verify_error))
                               : ": short read"));
    }

    const bool observed_reset =
        (verified & PCI_BRIDGE_CONTROL_BUS_RESET) != 0;
    if (observed_reset != reset) {
        return fail(PHAL_STATUS_ERROR,
                    "Secondary Bus Reset readback mismatch for " + bridge_bdf);
    }

    if (active_device_.logger != nullptr) {
        active_device_.logger->flow(
            "Secondary Bus Reset " + std::string(reset ? "asserted" : "deasserted") +
            ": endpoint=" + active_device_.bdf + " bridge=" + bridge_bdf);
    }
    return PHAL_STATUS_OK;
}

std::vector<DeviceContext> HalContext::scan_pci_devices() const
{
    std::vector<DeviceContext> devices;
    const std::filesystem::path sysfs_devices("/sys/bus/pci/devices");

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(sysfs_devices, ec)) {
        if (ec) {
            break;
        }

        uint16_t vendor_id = 0;
        uint16_t device_id = 0;
        if (!read_hex_u16(entry.path() / "vendor", vendor_id) ||
            !read_hex_u16(entry.path() / "device", device_id)) {
            continue;
        }

        DeviceContext ctx;
        ctx.bdf = entry.path().filename().string();
        ctx.vendor_id = vendor_id;
        ctx.device_id = device_id;
        devices.push_back(ctx);
    }
    return devices;
}

DeviceContext HalContext::mmap_bar_space(DeviceContext ctx)
{
    ctx.bar_mappings.clear();

    std::string command_error;
    if (!ensure_pci_mmio_enabled(ctx.bdf, command_error)) {
        for (auto bar_index : PROBE_BAR_INDICES) {
            auto bar = make_bar_mapping(bar_index);
            bar.error = command_error;
            ctx.bar_mappings.push_back(std::move(bar));
        }
        return ctx;
    }

    for (auto bar_index : PROBE_BAR_INDICES) {
        auto bar = make_bar_mapping(bar_index);

        uint64_t bar_start = 0;
        uint64_t bar_size = 0;
        if (!read_resource_info(ctx.bdf, bar_index, bar_start, bar_size)) {
            bar.error = "resource" + std::to_string(bar_index) + " is not present";
            ctx.bar_mappings.push_back(std::move(bar));
            continue;
        }

        auto resource = "/sys/bus/pci/devices/" + ctx.bdf + "/resource" +
                        std::to_string(bar_index);
        int fd = open(resource.c_str(), O_RDWR | O_SYNC);
        int prot = PROT_READ | PROT_WRITE;
        if (fd < 0) {
            fd = open(resource.c_str(), O_RDONLY | O_SYNC);
            prot = PROT_READ;
        }
        if (fd < 0) {
            bar.device_base = bar_start;
            bar.resource_size = bar_size;
            bar.error = "open resource" + std::to_string(bar_index) +
                        " failed: " + std::strerror(errno);
            ctx.bar_mappings.push_back(std::move(bar));
            continue;
        }

        const uint64_t map_size = bar_size;
        void* mapped = mmap(nullptr,
                            static_cast<size_t>(map_size),
                            prot,
                            MAP_SHARED,
                            fd,
                            0);
        auto mmap_errno = errno;
        close(fd);

        bar.device_base = bar_start;
        bar.resource_size = bar_size;
        bar.mapped_size = map_size;
        if (mapped == MAP_FAILED) {
            bar.error = "mmap resource" + std::to_string(bar_index) +
                        " failed: " + std::strerror(mmap_errno);
            ctx.bar_mappings.push_back(std::move(bar));
            continue;
        }

        bar.mapped_base = mapped;
        bar.mapped = true;
        bar.writable = (prot & PROT_WRITE) != 0;
        mapped_bar_mappings_.push_back({mapped, map_size});

        ctx.bar_mappings.push_back(std::move(bar));
    }
    return ctx;
}

void HalContext::clear_mappings()
{
    phal_context_ = {};
    aperture_phal_context_ = {};
    for (auto& ctx : ipc_contexts_) ctx = {};
    for (const auto& mapping : mapped_bar_mappings_) {
        if (mapping.first != nullptr && mapping.second != 0) {
            munmap(mapping.first, static_cast<size_t>(mapping.second));
        }
    }
    mapped_bar_mappings_.clear();
}

void HalContext::clear()
{
    clear_mappings();
    active_device_ = {};
    active_memory_regions_ = {};
    active_memory_ = std::make_shared<DeviceMemoryState>();
    host_dma_mode_ = HostDmaMode::SpevDrv;
    device_bound_ = false;
}
