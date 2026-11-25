#include "diag/core/PciHostRecovery.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>

#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t SNAPSHOT_FORMAT = 1;
constexpr uint16_t MPS_MRRS_MASK = 0x70e0;
constexpr uint16_t COMMAND_IO = 0x0001;
constexpr uint16_t COMMAND_MEMORY = 0x0002;
constexpr uint16_t COMMAND_MASTER = 0x0004;
constexpr uint16_t COMMAND_INTX_DISABLE = 0x0400;
constexpr uint16_t COMMAND_WRITABLE_MASK = 0x07ff;
constexpr uint32_t PCIE_DEVCAP_POWER_VALUE_MASK = 0x03fc0000;
constexpr uint32_t PCIE_DEVCAP_POWER_SCALE_MASK = 0x0c000000;
constexpr uint32_t PCIE_DEVCAP_CAPTURED_POWER_MASK =
    PCIE_DEVCAP_POWER_VALUE_MASK | PCIE_DEVCAP_POWER_SCALE_MASK;
constexpr uint64_t IORESOURCE_MEM = 0x00000200;
constexpr uint64_t IORESOURCE_UNSET = 0x20000000;
constexpr uint64_t IORESOURCE_DISABLED = 0x10000000;
constexpr size_t MAX_SNAPSHOT_SIZE = 1024 * 1024;

const std::regex BDF_PATTERN("[0-9a-f]{4}:[0-9a-f]{2}:[0-1][0-9a-f]\\.[0-7]");
const std::regex DRIVER_PATTERN("[A-Za-z0-9_.-]+");

void require(bool condition, const std::string& message)
{
    if (!condition) throw PciHostRecoveryError(message);
}

std::string hex_value(uint64_t value, size_t width)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(width) << value;
    return output.str();
}

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    return value.substr(begin);
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(input.good(), "open text file failed: " + path.string());
    std::ostringstream output;
    output << input.rdbuf();
    require(!input.bad(), "read text file failed: " + path.string());
    return trim(output.str());
}

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path);
    require(output.good(), "open control file failed: " + path.string());
    output << text;
    output.flush();
    require(output.good(), "write control file failed: " + path.string());
}

uint64_t inode_of(const std::filesystem::path& path)
{
#ifdef __linux__
    struct stat info {};
    require(::stat(path.c_str(), &info) == 0,
            "stat PCI device failed: " + path.string() + ": " +
                std::strerror(errno));
    return static_cast<uint64_t>(info.st_ino);
#else
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    require(!error, "resolve PCI device failed: " + error.message());
    return static_cast<uint64_t>(std::hash<std::string>{}(canonical.string()));
#endif
}

uint64_t config_size(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    require(!error && (size == 256 || size == 4096),
            "PCI config size must be 256 or 4096 bytes: " + path.string());
    return size;
}

std::vector<uint8_t> read_config_image(const std::filesystem::path& path)
{
    const auto size = config_size(path);
    std::vector<uint8_t> data(static_cast<size_t>(size));
#ifdef __linux__
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "open PCI config failed: " + path.string() + ": " +
                         std::strerror(errno));
    size_t done = 0;
    while (done < data.size()) {
        const auto count = ::pread(fd, data.data() + done,
                                   data.size() - done,
                                   static_cast<off_t>(done));
        if (count <= 0) {
            const auto saved_errno = errno;
            ::close(fd);
            throw PciHostRecoveryError(
                "read PCI config failed at 0x" + [&] {
                    std::ostringstream value;
                    value << std::hex << done;
                    return value.str();
                }() + ": " + (count < 0 ? std::strerror(saved_errno) : "short read"));
        }
        done += static_cast<size_t>(count);
    }
    ::close(fd);
#else
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "open PCI config failed: " + path.string());
    input.read(reinterpret_cast<char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    require(input.gcount() == static_cast<std::streamsize>(data.size()),
            "short PCI config read: " + path.string());
#endif
    return data;
}

uint64_t read_le(const std::vector<uint8_t>& data, size_t offset, size_t width)
{
    require((width == 1 || width == 2 || width == 4) &&
                offset % width == 0 && offset + width <= data.size(),
            "invalid configuration read offset/width");
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        value |= static_cast<uint64_t>(data[offset + i]) << (8 * i);
    }
    return value;
}

uint64_t read_config(const std::filesystem::path& path,
                     size_t offset,
                     size_t width)
{
    require(width == 1 || width == 2 || width == 4,
            "invalid configuration access width");
    require(offset % width == 0, "unaligned configuration access");
    uint32_t value = 0;
#ifdef __linux__
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "open PCI config failed: " + path.string() + ": " +
                         std::strerror(errno));
    const auto count = ::pread(fd, &value, width, static_cast<off_t>(offset));
    const auto saved_errno = errno;
    ::close(fd);
    require(count == static_cast<ssize_t>(width),
            "read PCI config failed at offset " + std::to_string(offset) +
                (count < 0 ? ": " + std::string(std::strerror(saved_errno))
                           : ": short read"));
#else
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "open PCI config failed: " + path.string());
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(width));
    require(input.gcount() == static_cast<std::streamsize>(width),
            "short PCI config read at offset " + std::to_string(offset));
#endif
    if (width == 1) return value & 0xff;
    if (width == 2) return value & 0xffff;
    return value;
}

void write_config(const std::filesystem::path& path,
                  size_t offset,
                  size_t width,
                  uint64_t value)
{
    require(width == 1 || width == 2 || width == 4,
            "invalid configuration write width");
    require(offset % width == 0, "unaligned configuration write");
    require(width == 4 || value < (uint64_t{1} << (width * 8)),
            "configuration value is too large");
    const uint32_t raw = static_cast<uint32_t>(value);
#ifdef __linux__
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    require(fd >= 0, "open PCI config for write failed: " + path.string() +
                         ": " + std::strerror(errno));
    const auto count = ::pwrite(fd, &raw, width, static_cast<off_t>(offset));
    const auto saved_errno = errno;
    ::close(fd);
    require(count == static_cast<ssize_t>(width),
            "write PCI config failed at offset " + std::to_string(offset) +
                (count < 0 ? ": " + std::string(std::strerror(saved_errno))
                           : ": short write"));
#else
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    require(output.good(), "open PCI config for write failed: " + path.string());
    output.seekp(static_cast<std::streamoff>(offset));
    output.write(reinterpret_cast<const char*>(&raw),
                 static_cast<std::streamsize>(width));
    output.flush();
    require(output.good(), "write PCI config failed at offset " +
                               std::to_string(offset));
#endif
}

void write_verified(const std::filesystem::path& config,
                    uint32_t identity,
                    size_t offset,
                    size_t width,
                    uint64_t value,
                    uint64_t mask = ~uint64_t{0})
{
    require(read_config(config, 0, 4) == identity,
            "endpoint became inaccessible or changed before config write");
    write_config(config, offset, width, value);
    const auto actual = read_config(config, offset, width);
    require((actual & mask) == (value & mask),
            "configuration write readback mismatch at offset " +
                std::to_string(offset));
}

bool valid_identity(uint32_t value)
{
    const auto vendor = static_cast<uint16_t>(value & 0xffff);
    return vendor != 0 && vendor != 1 && vendor != 0xffff;
}

std::filesystem::path device_path(const PciHostRecoveryPaths& paths,
                                  const std::string& bdf)
{
    require(std::regex_match(bdf, BDF_PATTERN),
            "invalid full PCI BDF: " + bdf);
    auto directory = bdf;
#ifdef _WIN32
    // Windows is test-only; ':' is not a legal fake-sysfs directory name.
    std::replace(directory.begin(), directory.end(), ':', '_');
#endif
    return paths.pci_devices / directory;
}

std::string driver_name(const std::filesystem::path& device)
{
    std::error_code error;
    const auto link = device / "driver";
    if (!std::filesystem::is_symlink(link, error)) return {};
    const auto resolved = std::filesystem::canonical(link, error);
    require(!error, "resolve bound driver failed: " + error.message());
    return resolved.filename().string();
}

void check_single_function(const PciHostRecoveryPaths& paths,
                           const std::string& bdf)
{
    const auto device = device_path(paths, bdf);
    require(std::filesystem::is_directory(device),
            "device disappeared from sysfs; no PCI rescan is performed");
    std::error_code error;
    require(!std::filesystem::is_symlink(device / "physfn", error),
            "PCI virtual functions are unsupported");
    const auto sriov = device / "sriov_totalvfs";
    if (std::filesystem::exists(sriov, error)) {
        require(read_text(sriov) == "0", "SR-IOV devices are unsupported");
    }
    auto prefix = bdf.substr(0, bdf.rfind('.') + 1);
#ifdef _WIN32
    std::replace(prefix.begin(), prefix.end(), ':', '_');
#endif
    size_t functions = 0;
    for (const auto& entry : std::filesystem::directory_iterator(paths.pci_devices,
                                                                  error)) {
        if (error) break;
        if (entry.path().filename().string().rfind(prefix, 0) == 0) ++functions;
    }
    require(!error && functions == 1,
            "multiple functions share this PCI device; coordinated recovery is required");
}

PciHostEnvironment capture_environment(const PciHostRecoveryPaths& paths,
                                       const std::string& bdf)
{
    check_single_function(paths, bdf);
    const auto device = device_path(paths, bdf);
    PciHostEnvironment environment;
    environment.boot_id = read_text(paths.boot_id);
    environment.sysfs_inode = inode_of(device);
    std::error_code error;
    environment.canonical_path = std::filesystem::canonical(device, error).string();
    require(!error, "resolve PCI sysfs path failed: " + error.message());
    environment.config_size = config_size(device / "config");

    std::ifstream resources(device / "resource");
    require(resources.good(), "open PCI resource table failed");
    for (auto& row : environment.resources) {
        std::string line;
        require(static_cast<bool>(std::getline(resources, line)),
                "PCI resource table is incomplete");
        std::istringstream parser(line);
        require(static_cast<bool>(parser >> std::hex >> row.start >> row.end >> row.flags),
                "invalid PCI resource table row");
        std::string extra;
        require(!(parser >> extra), "invalid extra PCI resource table field");
    }

    const std::array<std::pair<size_t, size_t>, 8> window_fields = {{
        {0x04, 2}, {0x18, 4}, {0x1c, 2}, {0x20, 4},
        {0x24, 4}, {0x28, 4}, {0x2c, 4}, {0x30, 4},
    }};
    auto parent = std::filesystem::path(environment.canonical_path).parent_path();
    while (!parent.empty()) {
        const auto name = parent.filename().string();
        if (std::regex_match(name, BDF_PATTERN)) {
            const auto config = parent / "config";
            require((read_config(config, 0x0e, 1) & 0x7f) == 1,
                    "unexpected upstream PCI header: " + name);
            PciUpstreamBridge bridge;
            bridge.bdf = name;
            for (const auto& field : window_fields) {
                bridge.windows.push_back(read_config(config, field.first, field.second));
            }
            environment.bridges.push_back(std::move(bridge));
        }
        const auto next_parent = parent.parent_path();
        if (next_parent == parent) break;
        parent = next_parent;
    }
    return environment;
}

std::vector<PciCapabilityEntry> standard_capabilities(
    const std::vector<uint8_t>& config,
    const std::array<uint32_t, 16>& header)
{
    if ((header[1] & (0x10u << 16)) == 0) return {};
    uint16_t offset = static_cast<uint16_t>(header[13] & 0xff);
    std::vector<PciCapabilityEntry> capabilities;
    std::vector<uint16_t> seen;
    while (offset != 0) {
        require(offset >= 0x40 && offset <= 0xfc && offset % 4 == 0 &&
                    std::find(seen.begin(), seen.end(), offset) == seen.end(),
                "invalid or looping standard capability chain");
        seen.push_back(offset);
        const auto value = static_cast<uint16_t>(read_le(config, offset, 2));
        const auto id = static_cast<uint16_t>(value & 0xff);
        require(id != 0 && id != 0xff, "unreadable standard capability header");
        capabilities.push_back({offset, id});
        offset = static_cast<uint16_t>((value >> 8) & 0xff);
    }
    for (const uint16_t id : {1u, 5u, 0x10u, 0x11u}) {
        require(std::count_if(capabilities.begin(), capabilities.end(),
                              [id](const PciCapabilityEntry& entry) {
                                  return entry.id == id;
                              }) <= 1,
                "duplicate standard PCI capability is unsupported");
    }
    return capabilities;
}

std::vector<PciCapabilityEntry> extended_capabilities(
    const std::vector<uint8_t>& config)
{
    if (config.size() <= 256) return {};
    uint16_t offset = 0x100;
    std::vector<PciCapabilityEntry> capabilities;
    std::vector<uint16_t> seen;
    while (offset != 0) {
        require(offset >= 0x100 && offset <= 0xffc && offset % 4 == 0 &&
                    std::find(seen.begin(), seen.end(), offset) == seen.end(),
                "invalid or looping extended capability chain");
        seen.push_back(offset);
        const auto value = static_cast<uint32_t>(read_le(config, offset, 4));
        if (offset == 0x100 && value == 0) break;
        const auto id = static_cast<uint16_t>(value & 0xffff);
        require(value != 0xffffffff && id != 0 && id != 0xffff,
                "unreadable extended capability header");
        require(id != 0x10 && id != 0x15 && id != 0x24,
                "SR-IOV or Resizable BAR recovery is unsupported");
        capabilities.push_back({offset, id});
        offset = static_cast<uint16_t>((value >> 20) & 0xfff);
    }
    return capabilities;
}

uint16_t capability_offset(const std::vector<PciCapabilityEntry>& entries,
                           uint16_t id)
{
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [id](const PciCapabilityEntry& entry) {
                                        return entry.id == id;
                                    });
    return found == entries.end() ? 0 : found->offset;
}

void check_transfer_sizes(uint16_t control, uint32_t devcap)
{
    const auto mps = static_cast<uint16_t>((control >> 5) & 7);
    const auto mrrs = static_cast<uint16_t>((control >> 12) & 7);
    const auto supported = static_cast<uint16_t>(devcap & 7);
    require(mps <= 5 && mrrs <= 5 && supported <= 5,
            "reserved MPS/MRRS/Max Payload Supported encoding");
    require(mps <= supported, "MPS exceeds endpoint Max Payload Supported");
}

struct MemoryBar {
    uint16_t index = 0;
    uint16_t offset = 0;
    uint32_t low = 0;
    bool is_64 = false;
    uint32_t high = 0;
};

std::vector<MemoryBar> memory_bars(const PciHostSnapshot& snapshot,
                                  bool require_address)
{
    std::vector<MemoryBar> bars;
    uint16_t index = 0;
    while (index < 6) {
        const auto low = snapshot.header[4 + index];
        const auto& resource = snapshot.environment.resources[index];
        require((low & 1) == 0, "I/O BARs are unsupported by MMIO recovery");
        const auto kind = static_cast<uint16_t>((low >> 1) & 3);
        require(kind == 0 || kind == 2, "unsupported memory BAR type");
        const bool is_64 = kind == 2;
        require(!is_64 || index < 5, "64-bit BAR cannot start at BAR5");
        const auto high = is_64 ? snapshot.header[5 + index] : 0;
        if (resource.start != 0 || resource.end != 0 || resource.flags != 0) {
            require((resource.flags & IORESOURCE_MEM) != 0 &&
                        (resource.flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED)) == 0 &&
                        resource.start > 0 && resource.end >= resource.start,
                    "BAR" + std::to_string(index) +
                        " has no usable Host memory allocation");
            const auto size = resource.end - resource.start + 1;
            require(size >= 16 && (size & (size - 1)) == 0,
                    "BAR" + std::to_string(index) + " has an invalid allocation size");
            const auto address = (static_cast<uint64_t>(high) << 32) | (low & ~0xfu);
            if (require_address) {
                require(address != 0 && address % size == 0,
                        "BAR" + std::to_string(index) +
                            " has no valid pre-reset bus address");
            }
            bars.push_back({index, static_cast<uint16_t>(0x10 + 4 * index),
                            low, is_64, high});
        } else {
            require((low & ~0xfu) == 0 && high == 0,
                    "unallocated BAR has an unexpected bus address");
        }
        if (is_64) {
            require(snapshot.environment.resources[index + 1] == PciResourceRow{},
                    "high half of a 64-bit BAR has a separate resource");
        }
        index += is_64 ? 2 : 1;
    }
    require(!bars.empty(), "no assigned memory BARs found");
    return bars;
}

PciHostSnapshot inspect_device(const PciHostRecoveryPaths& paths,
                               const std::string& target,
                               const std::string& bdf,
                               bool require_bar_address)
{
    const auto device = device_path(paths, bdf);
    PciHostSnapshot snapshot;
    snapshot.target = target;
    snapshot.bdf = bdf;
    snapshot.driver = driver_name(device);
    snapshot.driver_was_bound = !snapshot.driver.empty();
    snapshot.environment = capture_environment(paths, bdf);
    snapshot.config = read_config_image(device / "config");
    for (size_t i = 0; i < snapshot.header.size(); ++i) {
        snapshot.header[i] = static_cast<uint32_t>(read_le(snapshot.config, i * 4, 4));
    }
    require(valid_identity(snapshot.header[0]),
            "endpoint is not config-ready (invalid Vendor ID or CRS)");
    require(((snapshot.header[3] >> 16) & 0xff) == 0,
            "only single-function Type-0 endpoints are supported");
    snapshot.capabilities = standard_capabilities(snapshot.config, snapshot.header);
    snapshot.extended_capabilities = extended_capabilities(snapshot.config);
    const auto pcie = capability_offset(snapshot.capabilities, 0x10);
    require(pcie != 0 && pcie <= 0xf4, "missing or truncated PCIe capability");
    snapshot.pcie_flags = static_cast<uint16_t>(read_le(snapshot.config, pcie + 2, 2));
    require(((snapshot.pcie_flags >> 4) & 0xf) <= 1,
            "PCI function is not a PCIe endpoint");
    snapshot.pcie_devcap = static_cast<uint32_t>(read_le(snapshot.config, pcie + 4, 4));
    snapshot.pcie_control = static_cast<uint16_t>(read_le(snapshot.config, pcie + 8, 2));
    require((snapshot.pcie_control & 0x8000) == 0,
            "PCIe FLR/BCR trigger is active");
    check_transfer_sizes(snapshot.pcie_control, snapshot.pcie_devcap);
    memory_bars(snapshot, require_bar_address);
    require(capture_environment(paths, bdf) == snapshot.environment,
            "Host PCI device/resources changed while capturing configuration");
    return snapshot;
}

std::string utc_now()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm value {};
#ifdef _WIN32
    gmtime_s(&value, &time);
#else
    gmtime_r(&time, &value);
#endif
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string hex_encode(const std::vector<uint8_t>& data)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : data) output << std::setw(2) << unsigned(byte);
    return output.str();
}

std::vector<uint8_t> hex_decode(const std::string& value)
{
    require(value.size() == 512 || value.size() == 8192,
            "snapshot config_hex must encode 256 or 4096 bytes");
    std::vector<uint8_t> data;
    data.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        require(std::isxdigit(static_cast<unsigned char>(value[i])) &&
                    std::isxdigit(static_cast<unsigned char>(value[i + 1])),
                "snapshot config_hex contains a non-hex character");
        data.push_back(static_cast<uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
    }
    return data;
}

std::string json_escape(const std::string& value)
{
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            require(character >= 0x20, "control character in snapshot string");
            output << character;
        }
    }
    return output.str();
}

std::string serialize_snapshot(const PciHostSnapshot& snapshot)
{
    std::ostringstream output;
    output << "{\n"
           << "  \"format\": " << snapshot.format << ",\n"
           << "  \"target\": \"" << json_escape(snapshot.target) << "\",\n"
           << "  \"bdf\": \"" << json_escape(snapshot.bdf) << "\",\n"
           << "  \"driver\": \"" << json_escape(snapshot.driver) << "\",\n"
           << "  \"driver_was_bound\": "
           << (snapshot.driver_was_bound ? "true" : "false") << ",\n"
           << "  \"created_utc\": \"" << json_escape(snapshot.created_utc) << "\",\n"
           << "  \"config_hex\": \"" << hex_encode(snapshot.config) << "\",\n"
           << "  \"environment\": {\n"
           << "    \"boot_id\": \"" << json_escape(snapshot.environment.boot_id) << "\",\n"
           << "    \"inode\": " << snapshot.environment.sysfs_inode << ",\n"
           << "    \"path\": \"" << json_escape(snapshot.environment.canonical_path) << "\",\n"
           << "    \"config_size\": " << snapshot.environment.config_size << ",\n"
           << "    \"resources\": [\n";
    for (size_t i = 0; i < snapshot.environment.resources.size(); ++i) {
        const auto& row = snapshot.environment.resources[i];
        output << "      [" << row.start << ", " << row.end << ", " << row.flags << "]"
               << (i + 1 == snapshot.environment.resources.size() ? "\n" : ",\n");
    }
    output << "    ],\n    \"bridges\": [";
    for (size_t i = 0; i < snapshot.environment.bridges.size(); ++i) {
        const auto& bridge = snapshot.environment.bridges[i];
        output << (i == 0 ? "\n" : ",\n")
               << "      {\"bdf\": \"" << json_escape(bridge.bdf)
               << "\", \"windows\": [";
        for (size_t j = 0; j < bridge.windows.size(); ++j) {
            if (j != 0) output << ", ";
            output << bridge.windows[j];
        }
        output << "]}";
    }
    if (!snapshot.environment.bridges.empty()) output << '\n';
    output << "    ]\n  },\n  \"header\": [";
    for (size_t i = 0; i < snapshot.header.size(); ++i) {
        if (i != 0) output << ", ";
        output << snapshot.header[i];
    }
    const auto emit_capabilities = [&output](const char* name,
                                             const std::vector<PciCapabilityEntry>& entries) {
        output << "  \"" << name << "\": [";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) output << ", ";
            output << '[' << entries[i].offset << ", " << entries[i].id << ']';
        }
        output << "],\n";
    };
    output << "],\n";
    emit_capabilities("capabilities", snapshot.capabilities);
    emit_capabilities("extended_capabilities", snapshot.extended_capabilities);
    output << "  \"pcie_flags\": " << snapshot.pcie_flags << ",\n"
           << "  \"pcie_control\": " << snapshot.pcie_control << ",\n"
           << "  \"pcie_devcap\": " << snapshot.pcie_devcap << "\n}\n";
    return output.str();
}

struct JsonValue {
    enum class Kind { Null, Boolean, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    uint64_t number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    JsonValue parse()
    {
        auto result = value();
        whitespace();
        require(position_ == text_.size(), "trailing data in snapshot JSON");
        return result;
    }

private:
    std::string text_;
    size_t position_ = 0;

    void whitespace()
    {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
    }

    bool consume(char expected)
    {
        whitespace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue value()
    {
        whitespace();
        require(position_ < text_.size(), "unexpected end of snapshot JSON");
        if (text_[position_] == '{') return object();
        if (text_[position_] == '[') return array();
        if (text_[position_] == '"') {
            JsonValue result;
            result.kind = JsonValue::Kind::String;
            result.string = string();
            return result;
        }
        if (std::isdigit(static_cast<unsigned char>(text_[position_]))) return number();
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            JsonValue result;
            result.kind = JsonValue::Kind::Boolean;
            result.boolean = true;
            return result;
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            JsonValue result;
            result.kind = JsonValue::Kind::Boolean;
            return result;
        }
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            return {};
        }
        throw PciHostRecoveryError("unsupported token in snapshot JSON");
    }

    JsonValue object()
    {
        require(consume('{'), "expected JSON object");
        JsonValue result;
        result.kind = JsonValue::Kind::Object;
        if (consume('}')) return result;
        do {
            whitespace();
            require(position_ < text_.size() && text_[position_] == '"',
                    "expected JSON object key");
            const auto key = string();
            require(consume(':'), "expected colon after JSON key");
            require(result.object.emplace(key, value()).second,
                    "duplicate JSON object key: " + key);
        } while (consume(','));
        require(consume('}'), "expected end of JSON object");
        return result;
    }

    JsonValue array()
    {
        require(consume('['), "expected JSON array");
        JsonValue result;
        result.kind = JsonValue::Kind::Array;
        if (consume(']')) return result;
        do {
            result.array.push_back(value());
        } while (consume(','));
        require(consume(']'), "expected end of JSON array");
        return result;
    }

    JsonValue number()
    {
        whitespace();
        const auto begin = position_;
        while (position_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        require(begin != position_, "expected unsigned JSON integer");
        JsonValue result;
        result.kind = JsonValue::Kind::Number;
        try {
            result.number = std::stoull(text_.substr(begin, position_ - begin));
        } catch (const std::exception&) {
            throw PciHostRecoveryError("snapshot JSON integer is out of range");
        }
        return result;
    }

    std::string string()
    {
        require(consume('"'), "expected JSON string");
        std::string result;
        while (position_ < text_.size()) {
            const auto character = text_[position_++];
            if (character == '"') return result;
            require(static_cast<unsigned char>(character) >= 0x20,
                    "control character in JSON string");
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            require(position_ < text_.size(), "truncated JSON escape");
            switch (text_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: throw PciHostRecoveryError("unsupported JSON string escape");
            }
        }
        throw PciHostRecoveryError("unterminated JSON string");
    }
};

const JsonValue& member(const JsonValue& value,
                        const char* name,
                        JsonValue::Kind kind)
{
    require(value.kind == JsonValue::Kind::Object, "expected JSON object");
    const auto found = value.object.find(name);
    require(found != value.object.end(), "missing snapshot field: " + std::string(name));
    require(found->second.kind == kind,
            "invalid type for snapshot field: " + std::string(name));
    return found->second;
}

uint64_t number_member(const JsonValue& value, const char* name)
{
    return member(value, name, JsonValue::Kind::Number).number;
}

std::string string_member(const JsonValue& value, const char* name)
{
    return member(value, name, JsonValue::Kind::String).string;
}

std::vector<PciCapabilityEntry> parse_capabilities(const JsonValue& root,
                                                   const char* name,
                                                   uint16_t minimum,
                                                   uint16_t maximum)
{
    const auto& array = member(root, name, JsonValue::Kind::Array).array;
    std::vector<PciCapabilityEntry> entries;
    for (const auto& item : array) {
        require(item.kind == JsonValue::Kind::Array && item.array.size() == 2 &&
                    item.array[0].kind == JsonValue::Kind::Number &&
                    item.array[1].kind == JsonValue::Kind::Number,
                "invalid capability entry in snapshot");
        require(item.array[0].number >= minimum && item.array[0].number <= maximum &&
                    item.array[0].number % 4 == 0 && item.array[1].number <= 0xffff,
                "capability entry is out of range");
        entries.push_back({static_cast<uint16_t>(item.array[0].number),
                           static_cast<uint16_t>(item.array[1].number)});
    }
    return entries;
}

void validate_snapshot(PciHostSnapshot& snapshot)
{
    require(snapshot.format == SNAPSHOT_FORMAT, "unsupported snapshot format");
    require(std::regex_match(snapshot.bdf, BDF_PATTERN), "invalid snapshot BDF");
    require(!snapshot.target.empty(), "snapshot target is empty");
    require(snapshot.driver_was_bound == !snapshot.driver.empty(),
            "snapshot driver fields are inconsistent");
    require(!snapshot.driver_was_bound ||
                (snapshot.driver != "." && snapshot.driver != ".." &&
                 std::regex_match(snapshot.driver, DRIVER_PATTERN)),
            "snapshot driver name is invalid");
    require(snapshot.config.size() == snapshot.environment.config_size,
            "snapshot config size does not match environment");
    require(valid_identity(snapshot.header[0]) &&
                ((snapshot.header[3] >> 16) & 0xff) == 0,
            "snapshot does not contain a supported endpoint");
    for (size_t i = 0; i < snapshot.header.size(); ++i) {
        require(snapshot.header[i] == read_le(snapshot.config, i * 4, 4),
                "snapshot header disagrees with config image");
    }
    const auto parsed_capabilities =
        standard_capabilities(snapshot.config, snapshot.header);
    const auto parsed_extended = extended_capabilities(snapshot.config);
    require(snapshot.capabilities == parsed_capabilities,
            "snapshot capability list disagrees with config image");
    require(snapshot.extended_capabilities == parsed_extended,
            "snapshot extended capability list disagrees with config image");
    const auto pcie = capability_offset(snapshot.capabilities, 0x10);
    require(pcie != 0 && pcie <= 0xf4,
            "snapshot has no PCIe capability");
    require(snapshot.pcie_flags == read_le(snapshot.config, pcie + 2, 2) &&
                snapshot.pcie_devcap == read_le(snapshot.config, pcie + 4, 4) &&
                snapshot.pcie_control == read_le(snapshot.config, pcie + 8, 2),
            "snapshot PCIe fields disagree with config image");
    require(((snapshot.pcie_flags >> 4) & 0xf) <= 1,
            "snapshot PCIe capability is not an endpoint");
    require((snapshot.pcie_control & 0x8000) == 0,
            "snapshot contains an active FLR/BCR trigger");
    check_transfer_sizes(snapshot.pcie_control, snapshot.pcie_devcap);
    memory_bars(snapshot, true);
}

PciHostSnapshot parse_snapshot(const std::string& text)
{
    const auto root = JsonParser(text).parse();
    PciHostSnapshot snapshot;
    const auto format = number_member(root, "format");
    require(format <= UINT32_MAX, "snapshot format is out of range");
    snapshot.format = static_cast<uint32_t>(format);
    snapshot.target = string_member(root, "target");
    snapshot.bdf = string_member(root, "bdf");
    snapshot.driver = string_member(root, "driver");
    snapshot.driver_was_bound =
        member(root, "driver_was_bound", JsonValue::Kind::Boolean).boolean;
    snapshot.created_utc = string_member(root, "created_utc");
    snapshot.config = hex_decode(string_member(root, "config_hex"));

    const auto& environment = member(root, "environment", JsonValue::Kind::Object);
    snapshot.environment.boot_id = string_member(environment, "boot_id");
    snapshot.environment.sysfs_inode = number_member(environment, "inode");
    snapshot.environment.canonical_path = string_member(environment, "path");
    snapshot.environment.config_size = number_member(environment, "config_size");
    const auto& resources = member(environment, "resources", JsonValue::Kind::Array).array;
    require(resources.size() == 6, "snapshot must contain six PCI resources");
    for (size_t i = 0; i < resources.size(); ++i) {
        const auto& row = resources[i];
        require(row.kind == JsonValue::Kind::Array && row.array.size() == 3 &&
                    std::all_of(row.array.begin(), row.array.end(), [](const JsonValue& item) {
                        return item.kind == JsonValue::Kind::Number;
                    }),
                "invalid snapshot resource row");
        snapshot.environment.resources[i] = {
            row.array[0].number, row.array[1].number, row.array[2].number};
    }
    const auto& bridges = member(environment, "bridges", JsonValue::Kind::Array).array;
    for (const auto& item : bridges) {
        PciUpstreamBridge bridge;
        bridge.bdf = string_member(item, "bdf");
        require(std::regex_match(bridge.bdf, BDF_PATTERN), "invalid bridge BDF");
        const auto& windows = member(item, "windows", JsonValue::Kind::Array).array;
        require(windows.size() == 8, "invalid bridge window register count");
        for (const auto& window : windows) {
            require(window.kind == JsonValue::Kind::Number,
                    "invalid bridge window value");
            bridge.windows.push_back(window.number);
        }
        snapshot.environment.bridges.push_back(std::move(bridge));
    }

    const auto& header = member(root, "header", JsonValue::Kind::Array).array;
    require(header.size() == snapshot.header.size(), "invalid Type-0 header length");
    for (size_t i = 0; i < header.size(); ++i) {
        require(header[i].kind == JsonValue::Kind::Number &&
                    header[i].number <= UINT32_MAX,
                "invalid Type-0 header value");
        snapshot.header[i] = static_cast<uint32_t>(header[i].number);
    }
    snapshot.capabilities = parse_capabilities(root, "capabilities", 0x40, 0xfc);
    snapshot.extended_capabilities =
        parse_capabilities(root, "extended_capabilities", 0x100, 0xffc);
    require(number_member(root, "pcie_flags") <= UINT16_MAX &&
                number_member(root, "pcie_control") <= UINT16_MAX &&
                number_member(root, "pcie_devcap") <= UINT32_MAX,
            "PCIe snapshot field is out of range");
    snapshot.pcie_flags = static_cast<uint16_t>(number_member(root, "pcie_flags"));
    snapshot.pcie_control = static_cast<uint16_t>(number_member(root, "pcie_control"));
    snapshot.pcie_devcap = static_cast<uint32_t>(number_member(root, "pcie_devcap"));
    validate_snapshot(snapshot);
    return snapshot;
}

void require_unbound(const PciHostRecoveryPaths& paths,
                     const std::string& bdf)
{
    check_single_function(paths, bdf);
    require(driver_name(device_path(paths, bdf)).empty(),
            "a Host driver is bound; recovery requires an unbound endpoint");
}

void check_same_device(const PciHostSnapshot& saved,
                       const PciHostSnapshot& live,
                       const std::function<void(const std::string&)>& report)
{
    require(saved.environment == live.environment,
            "Host boot/device object/path/resources/upstream windows changed; refusing replay");
    for (const size_t index : {0u, 2u, 11u}) {
        require(saved.header[index] == live.header[index],
                "endpoint identity/class/subsystem changed");
    }
    require(saved.capabilities == live.capabilities,
            "endpoint standard capability layout changed");
    require(saved.extended_capabilities == live.extended_capabilities,
            "endpoint extended capability layout changed");
    require(saved.pcie_flags == live.pcie_flags,
            "endpoint PCIe capability flags changed");
    require((saved.pcie_devcap & ~PCIE_DEVCAP_CAPTURED_POWER_MASK) ==
                (live.pcie_devcap & ~PCIE_DEVCAP_CAPTURED_POWER_MASK),
            "endpoint immutable PCIe Device Capabilities changed");
    if (saved.pcie_devcap != live.pcie_devcap && report) {
        const auto saved_value =
            (saved.pcie_devcap & PCIE_DEVCAP_POWER_VALUE_MASK) >> 18;
        const auto saved_scale =
            (saved.pcie_devcap & PCIE_DEVCAP_POWER_SCALE_MASK) >> 26;
        const auto live_value =
            (live.pcie_devcap & PCIE_DEVCAP_POWER_VALUE_MASK) >> 18;
        const auto live_scale =
            (live.pcie_devcap & PCIE_DEVCAP_POWER_SCALE_MASK) >> 26;
        report("PCIe Captured Slot Power Limit changed across chip reset; "
               "continuing recovery: saved_devcap=" +
               hex_value(saved.pcie_devcap, 8) + " live_devcap=" +
               hex_value(live.pcie_devcap, 8) + " saved_value=" +
               std::to_string(saved_value) + " saved_scale=" +
               std::to_string(saved_scale) + " live_value=" +
               std::to_string(live_value) + " live_scale=" +
               std::to_string(live_scale));
    }
    uint16_t index = 0;
    while (index < 6) {
        const auto old_bar = saved.header[4 + index];
        const auto new_bar = live.header[4 + index];
        require((old_bar & 0xf) == (new_bar & 0xf),
                "BAR type/prefetch attributes changed");
        index += ((old_bar >> 1) & 3) == 2 ? 2 : 1;
    }
}

} // namespace

bool PciResourceRow::operator==(const PciResourceRow& other) const
{
    return start == other.start && end == other.end && flags == other.flags;
}

bool PciUpstreamBridge::operator==(const PciUpstreamBridge& other) const
{
    return bdf == other.bdf && windows == other.windows;
}

bool PciCapabilityEntry::operator==(const PciCapabilityEntry& other) const
{
    return offset == other.offset && id == other.id;
}

bool PciHostEnvironment::operator==(const PciHostEnvironment& other) const
{
    return boot_id == other.boot_id && sysfs_inode == other.sysfs_inode &&
           canonical_path == other.canonical_path && config_size == other.config_size &&
           resources == other.resources && bridges == other.bridges;
}

PciHostRecovery::PciHostRecovery(PciHostRecoveryPaths paths)
    : paths_(std::move(paths))
{
}

PciHostRecovery::DeviceLock::~DeviceLock()
{
#ifdef __linux__
    if (fd_ >= 0) ::close(fd_);
#endif
}

PciHostRecovery::DeviceLock::DeviceLock(DeviceLock&& other) noexcept
    : fd_(other.fd_)
{
    other.fd_ = -1;
}

PciHostRecovery::DeviceLock& PciHostRecovery::DeviceLock::operator=(
    DeviceLock&& other) noexcept
{
    if (this != &other) {
#ifdef __linux__
        if (fd_ >= 0) ::close(fd_);
#endif
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

PciHostRecovery::DeviceLock PciHostRecovery::lock_device(
    const std::string& bdf) const
{
    require(std::regex_match(bdf, BDF_PATTERN), "invalid PCI BDF for lock");
#ifdef __linux__
    const auto path = paths_.lock_directory / ("spev-chip-reset-" + bdf + ".lock");
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(fd >= 0, "open device recovery lock failed: " + path.string() +
                         ": " + std::strerror(errno));
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const auto saved_errno = errno;
        ::close(fd);
        throw PciHostRecoveryError(
            "another prepare/recover process holds the device lock: " +
            std::string(std::strerror(saved_errno)));
    }
    return DeviceLock(fd);
#else
    return DeviceLock();
#endif
}

PciHostSnapshot PciHostRecovery::capture(const std::string& target,
                                         const std::string& bdf) const
{
    auto snapshot = inspect_device(paths_, target, bdf, true);
    snapshot.created_utc = utc_now();
    return snapshot;
}

void PciHostRecovery::save_new(const std::filesystem::path& output,
                               const PciHostSnapshot& snapshot) const
{
    auto validated = snapshot;
    validate_snapshot(validated);
    const auto text = serialize_snapshot(snapshot);
#ifdef __linux__
    const int fd = ::open(output.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    require(fd >= 0, "create snapshot failed (existing files are not overwritten): " +
                         output.string() + ": " + std::strerror(errno));
    size_t done = 0;
    while (done < text.size()) {
        const auto count = ::write(fd, text.data() + done, text.size() - done);
        if (count <= 0) {
            const auto saved_errno = errno;
            ::close(fd);
            throw PciHostRecoveryError("write snapshot failed: " +
                                       std::string(std::strerror(saved_errno)));
        }
        done += static_cast<size_t>(count);
    }
    if (::fsync(fd) != 0) {
        const auto saved_errno = errno;
        ::close(fd);
        throw PciHostRecoveryError("fsync snapshot failed: " +
                                   std::string(std::strerror(saved_errno)));
    }
    ::close(fd);
#else
    require(!std::filesystem::exists(output),
            "snapshot already exists and will not be overwritten: " + output.string());
    std::ofstream stream(output, std::ios::binary);
    require(stream.good(), "create snapshot failed: " + output.string());
    stream << text;
    stream.flush();
    require(stream.good(), "write snapshot failed: " + output.string());
#endif
}

PciHostSnapshot PciHostRecovery::load(const std::filesystem::path& input) const
{
    std::error_code error;
    const auto size = std::filesystem::file_size(input, error);
    require(!error && size <= MAX_SNAPSHOT_SIZE,
            "snapshot is missing or unexpectedly large: " + input.string());
    std::ifstream stream(input, std::ios::binary);
    require(stream.good(), "open snapshot failed: " + input.string());
    std::ostringstream text;
    text << stream.rdbuf();
    require(!stream.bad(), "read snapshot failed: " + input.string());
    return parse_snapshot(text.str());
}

void PciHostRecovery::unbind_driver(const PciHostSnapshot& snapshot) const
{
    const auto device = device_path(paths_, snapshot.bdf);
    const auto current = driver_name(device);
    if (!snapshot.driver_was_bound) {
        require(current.empty(), "unexpected driver is bound before reset");
        return;
    }
    require(current == snapshot.driver,
            "bound driver changed before unbind: expected " + snapshot.driver +
                ", found " + current);
    write_text(device / "driver" / "unbind", snapshot.bdf);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (driver_name(device).empty()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw PciHostRecoveryError("driver did not unbind from " + snapshot.bdf);
}

void PciHostRecovery::bind_driver(const PciHostSnapshot& snapshot) const
{
    if (!snapshot.driver_was_bound) return;
    const auto device = device_path(paths_, snapshot.bdf);
    const auto current = driver_name(device);
    if (current == snapshot.driver) return;
    require(current.empty(), "a different driver is already bound after reset");
    write_text(paths_.pci_drivers / snapshot.driver / "bind", snapshot.bdf);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto bound = driver_name(device);
        if (bound == snapshot.driver) return;
        require(bound.empty(), "a different driver bound during recovery");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw PciHostRecoveryError("driver did not bind to " + snapshot.bdf);
}

void PciHostRecovery::make_unbound_device_safe(
    const PciHostSnapshot& snapshot) const
{
    require_unbound(paths_, snapshot.bdf);
    require(capture_environment(paths_, snapshot.bdf) == snapshot.environment,
            "Host environment changed before post-unbind safety checks");
    const auto config = device_path(paths_, snapshot.bdf) / "config";
    const auto identity = snapshot.header[0];
    require(read_config(config, 0, 4) == identity,
            "endpoint identity changed before post-unbind safety checks");
    const auto command = static_cast<uint16_t>(read_config(config, 4, 2));
    const auto safe_command = static_cast<uint16_t>(
        (command & COMMAND_WRITABLE_MASK &
         ~(COMMAND_IO | COMMAND_MEMORY | COMMAND_MASTER)) |
        COMMAND_INTX_DISABLE);
    write_verified(config, identity, 4, 2, safe_command, COMMAND_WRITABLE_MASK);

    for (const auto& capability : snapshot.capabilities) {
        require(read_config(config, capability.offset, 1) == capability.id,
                "capability layout changed before interrupt disable");
        if (capability.id == 5) {
            const auto control = static_cast<uint16_t>(
                read_config(config, capability.offset + 2, 2));
            write_verified(config, identity, capability.offset + 2, 2,
                           static_cast<uint16_t>(control & ~uint16_t{1}), 1);
        } else if (capability.id == 0x11) {
            const auto control = static_cast<uint16_t>(
                read_config(config, capability.offset + 2, 2));
            write_verified(config, identity, capability.offset + 2, 2,
                           static_cast<uint16_t>(control & ~uint16_t{0x8000}),
                           0x8000);
        } else if (capability.id == 1) {
            require((read_config(config, capability.offset + 4, 2) & 3) == 0,
                    "endpoint is not in PCI power state D0");
        }
    }
    const auto rom = static_cast<uint32_t>(read_config(config, 0x30, 4));
    if ((rom & 1) != 0) {
        write_verified(config, identity, 0x30, 4, rom & ~uint32_t{1}, 1);
    }
    const auto verified = static_cast<uint16_t>(read_config(config, 4, 2));
    require((verified & (COMMAND_IO | COMMAND_MEMORY | COMMAND_MASTER)) == 0 &&
                (verified & COMMAND_INTX_DISABLE) != 0,
            "could not establish safe post-unbind Command state");
}

void PciHostRecovery::restore(const PciHostSnapshot& snapshot,
                              std::chrono::milliseconds timeout,
                              const std::function<void(const std::string&)>& report) const
{
    auto validated = snapshot;
    validate_snapshot(validated);
    require(timeout.count() >= 0 && timeout <= std::chrono::hours(1),
            "config-ready timeout must be between 0 and 3600000 ms");
    const auto config = device_path(paths_, snapshot.bdf) / "config";
    const auto identity = snapshot.header[0];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string reason = "endpoint not ready";
    while (true) {
        bool config_read = false;
        uint32_t current = 0;
        try {
            require_unbound(paths_, snapshot.bdf);
            current = static_cast<uint32_t>(read_config(config, 0, 4));
            config_read = true;
        } catch (const PciHostRecoveryError& error) {
            reason = error.what();
        }
        if (config_read && valid_identity(current)) {
            require(current == identity, "endpoint identity changed at original BDF");
            break;
        }
        if (config_read) reason = "endpoint config identity is not ready";
        const auto now = std::chrono::steady_clock::now();
        require(now < deadline,
                "timed out waiting for original BDF: " + reason);
        std::this_thread::sleep_for(
            std::min(std::chrono::milliseconds(200),
                     std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }

    auto live = inspect_device(paths_, snapshot.target, snapshot.bdf, false);
    require(!live.driver_was_bound, "driver bound during restore inspection");
    check_same_device(snapshot, live, report);
    const auto bars = memory_bars(snapshot, true);
    const auto pcie = capability_offset(live.capabilities, 0x10);
    const auto control = static_cast<uint16_t>(
        (live.pcie_control & ~MPS_MRRS_MASK) |
        (snapshot.pcie_control & MPS_MRRS_MASK));
    const auto live_command = static_cast<uint16_t>(live.header[1] & 0xffff);
    const auto disabled = static_cast<uint16_t>(
        (live_command & COMMAND_WRITABLE_MASK &
         ~(COMMAND_IO | COMMAND_MEMORY | COMMAND_MASTER)) |
        COMMAND_INTX_DISABLE);
    const auto saved_command = static_cast<uint16_t>(snapshot.header[1] & 0xffff);
    const auto final_command = static_cast<uint16_t>(
        (saved_command & COMMAND_WRITABLE_MASK &
         ~(COMMAND_IO | COMMAND_MASTER | COMMAND_INTX_DISABLE)) |
        COMMAND_MEMORY | COMMAND_INTX_DISABLE);

    bool began = false;
    try {
        began = true;
        write_verified(config, identity, 4, 2, disabled, COMMAND_WRITABLE_MASK);
        for (const auto& bar : bars) {
            require((read_config(config, 4, 2) & 7) == 0,
                    "decode or bus mastering changed during BAR restore");
            if (bar.is_64) {
                write_verified(config, identity, bar.offset + 4, 4, bar.high);
            }
            write_verified(config, identity, bar.offset, 4, bar.low);
        }
        require((read_config(config, 4, 2) & 7) == 0,
                "decode or bus mastering changed during PCIe control restore");
        write_verified(config, identity, pcie + 8, 2, control, MPS_MRRS_MASK);
        for (const auto& bar : bars) {
            require(read_config(config, bar.offset, 4) == bar.low,
                    "BAR changed before MMIO enable");
            if (bar.is_64) {
                require(read_config(config, bar.offset + 4, 4) == bar.high,
                        "64-bit BAR high half changed before MMIO enable");
            }
        }
        require(capture_environment(paths_, snapshot.bdf) == snapshot.environment,
                "Host environment changed during config restore");
        require((read_config(config, 4, 2) & 7) == 0,
                "decode or bus mastering changed before MMIO enable");
        write_verified(config, identity, 4, 2, final_command,
                       COMMAND_WRITABLE_MASK);
    } catch (...) {
        if (began) {
            try {
                const auto current = static_cast<uint16_t>(read_config(config, 4, 2));
                const auto safe = static_cast<uint16_t>(
                    (current & COMMAND_WRITABLE_MASK &
                     ~(COMMAND_IO | COMMAND_MEMORY | COMMAND_MASTER)) |
                    COMMAND_INTX_DISABLE);
                write_verified(config, identity, 4, 2, safe,
                               COMMAND_WRITABLE_MASK);
            } catch (...) {
                // Preserve the original recovery failure.
            }
        }
        throw;
    }
}

void PciHostRecovery::enforce_mmio_only_command(const std::string& bdf) const
{
    require_unbound(paths_, bdf);
    const auto config = device_path(paths_, bdf) / "config";
    const auto identity = static_cast<uint32_t>(read_config(config, 0, 4));
    require(valid_identity(identity), "endpoint unavailable after framework discovery");
    const auto command = static_cast<uint16_t>(read_config(config, 4, 2));
    const auto safe = static_cast<uint16_t>(
        (command & COMMAND_WRITABLE_MASK & ~(COMMAND_IO | COMMAND_MASTER)) |
        COMMAND_MEMORY | COMMAND_INTX_DISABLE);
    write_verified(config, identity, 4, 2, safe, COMMAND_WRITABLE_MASK);
}
