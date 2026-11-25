#include "diag/core/PciHostRecovery.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::cerr << "CHECK failed at line " << __LINE__ << ": "          \
                      << #condition << std::endl;                                \
            ++failures;                                                         \
        }                                                                        \
    } while (false)

void write_u16(std::vector<uint8_t>& data, size_t offset, uint16_t value)
{
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void write_u32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint16_t read_u16(const std::vector<uint8_t>& data, size_t offset)
{
    return static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(data[offset + 1] << 8);
}

uint32_t read_u32(const std::vector<uint8_t>& data, size_t offset)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= uint32_t(data[offset + i]) << (8 * i);
    return value;
}

void write_binary(const std::filesystem::path& path,
                  const std::vector<uint8_t>& data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!output.good()) throw std::runtime_error("write_binary failed");
}

std::vector<uint8_t> read_binary(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

struct FakePciTree {
    std::filesystem::path root;
    std::filesystem::path device;
    std::filesystem::path config;
    std::filesystem::path resource;
    PciHostRecovery recovery;

    explicit FakePciTree(const std::string& name, size_t config_bytes = 256)
        : root(std::filesystem::temp_directory_path() / name),
#ifdef _WIN32
          device(root / "devices" / "0000_01_00.0"),
#else
          device(root / "devices" / "0000:01:00.0"),
#endif
          config(device / "config"),
          resource(device / "resource"),
          recovery(PciHostRecoveryPaths{
              root / "devices", root / "drivers", root / "boot_id", root / "locks"})
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(device);
        std::filesystem::create_directories(root / "drivers");
        std::filesystem::create_directories(root / "locks");
        std::ofstream(root / "boot_id") << "test-boot-id\n";

        std::vector<uint8_t> image(config_bytes, 0);
        if (config_bytes != 256 && config_bytes != 4096) {
            throw std::runtime_error("invalid fake config size");
        }
        write_u32(image, 0x00, 0xabcd16c3);
        write_u32(image, 0x04, 0x00100006); // Cap list; MSE+BME.
        write_u32(image, 0x08, 0x12000001);
        write_u32(image, 0x0c, 0x00000000); // Type-0, single function.
        write_u32(image, 0x10, 0x00000004); // 64-bit BAR0 at 0x1_00000000.
        write_u32(image, 0x14, 0x00000001);
        write_u32(image, 0x2c, 0x56781234);
        write_u32(image, 0x34, 0x00000040);
        write_u16(image, 0x40, 0x0010); // PCIe capability, end of list.
        write_u16(image, 0x42, 0x0000); // Endpoint type.
        write_u32(image, 0x44, 0x00000000); // MPS supported = 128 bytes.
        write_u16(image, 0x48, 0x0000); // MPS/MRRS = 128 bytes.
        write_binary(config, image);
        write_resources(0x100000000ull);
    }

    ~FakePciTree()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write_resources(uint64_t bar0_start)
    {
        std::ofstream output(resource, std::ios::trunc);
        output << std::hex
               << bar0_start << ' ' << bar0_start + 0xfff << " 200\n"
               << "0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n";
    }
};

void test_round_trip_and_restore()
{
    FakePciTree tree("spev-pci-host-recovery-roundtrip");
    const auto snapshot_path = tree.root / "snapshot.json";
    auto powered_image = read_binary(tree.config);
    write_u32(powered_image, 0x44, 0x012c0000); // Captured Slot Power Value = 75.
    write_binary(tree.config, powered_image);
    const auto snapshot = tree.recovery.capture("TPU0", "0000:01:00.0");
    CHECK(snapshot.config.size() == 256);
    CHECK(snapshot.driver.empty());
    tree.recovery.save_new(snapshot_path, snapshot);
    const auto loaded = tree.recovery.load(snapshot_path);
    CHECK(loaded.target == "TPU0");
    CHECK(loaded.bdf == "0000:01:00.0");
    CHECK(loaded.config == snapshot.config);
    CHECK(loaded.environment == snapshot.environment);

    bool overwrite_rejected = false;
    try {
        tree.recovery.save_new(snapshot_path, snapshot);
    } catch (const PciHostRecoveryError&) {
        overwrite_rejected = true;
    }
    CHECK(overwrite_rejected);

    auto reset_image = read_binary(tree.config);
    write_u16(reset_image, 0x04, 0x0400);
    write_u32(reset_image, 0x10, 0x00000004);
    write_u32(reset_image, 0x14, 0x00000000);
    write_u32(reset_image, 0x44, 0x00000000); // PMU reset clears captured power.
    write_u16(reset_image, 0x48, 0x1000); // Change MRRS so restore is observable.
    write_binary(tree.config, reset_image);

    std::string diagnostic;
    tree.recovery.restore(
        loaded, std::chrono::milliseconds(0),
        [&diagnostic](const std::string& message) { diagnostic = message; });
    const auto restored = read_binary(tree.config);
    CHECK(read_u32(restored, 0x10) == 0x00000004);
    CHECK(read_u32(restored, 0x14) == 0x00000001);
    CHECK((read_u16(restored, 0x48) & 0x70e0) == 0);
    CHECK((read_u16(restored, 0x04) & 0x0407) == 0x0402);
    CHECK(diagnostic.find("Captured Slot Power Limit changed") !=
          std::string::npos);
}

void test_environment_change_prevents_writes()
{
    FakePciTree tree("spev-pci-host-recovery-environment");
    const auto snapshot = tree.recovery.capture("TPU0", "0000:01:00.0");
    auto reset_image = read_binary(tree.config);
    write_u16(reset_image, 0x04, 0x0400);
    write_u32(reset_image, 0x14, 0);
    write_binary(tree.config, reset_image);
    tree.write_resources(0x200000000ull);
    const auto before = read_binary(tree.config);

    bool rejected = false;
    try {
        tree.recovery.restore(snapshot, std::chrono::milliseconds(0));
    } catch (const PciHostRecoveryError&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(read_binary(tree.config) == before);
}

void test_full_config_capture()
{
    FakePciTree tree("spev-pci-host-recovery-full-config", 4096);
    auto image = read_binary(tree.config);
    image[0x3ff] = 0xa5;
    write_binary(tree.config, image);
    const auto snapshot = tree.recovery.capture("TPU0", "0000:01:00.0");
    CHECK(snapshot.config.size() == 4096);
    CHECK(snapshot.config[0x3ff] == 0xa5);
    const auto path = tree.root / "full.json";
    tree.recovery.save_new(path, snapshot);
    CHECK(tree.recovery.load(path).config == snapshot.config);
}

void test_immutable_devcap_change_prevents_writes()
{
    FakePciTree tree("spev-pci-host-recovery-devcap");
    const auto snapshot = tree.recovery.capture("TPU0", "0000:01:00.0");
    auto reset_image = read_binary(tree.config);
    write_u16(reset_image, 0x04, 0x0400);
    write_u32(reset_image, 0x14, 0);
    write_u32(reset_image, 0x44, 0x00000020); // Extended Tag capability changed.
    write_binary(tree.config, reset_image);
    const auto before = read_binary(tree.config);

    bool rejected = false;
    try {
        tree.recovery.restore(snapshot, std::chrono::milliseconds(0));
    } catch (const PciHostRecoveryError&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(read_binary(tree.config) == before);
}

void test_corrupt_snapshot_rejected()
{
    FakePciTree tree("spev-pci-host-recovery-corrupt");
    const auto corrupt = tree.root / "corrupt.json";
    std::ofstream(corrupt) << "{\"format\":1,\"bdf\":false}";
    bool rejected = false;
    try {
        (void)tree.recovery.load(corrupt);
    } catch (const PciHostRecoveryError&) {
        rejected = true;
    }
    CHECK(rejected);
}

} // namespace

int main()
{
    try {
        test_round_trip_and_restore();
        test_environment_change_prevents_writes();
        test_full_config_capture();
        test_immutable_devcap_change_prevents_writes();
        test_corrupt_snapshot_rejected();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << std::endl;
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "pci_host_recovery_tests passed" << std::endl;
    return 0;
}
