#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"
#include "diag/core/HalContext.h"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint8_t IDENTITY = 0;
constexpr uint64_t DMEM_BASE = 0x10000000; //DF space 256M can't be configured thru aperture
constexpr uint64_t APERTURE_SIZE = 0x100000;
constexpr uint64_t BAR23_FIRST_CONFIGURABLE_OFFSET = 0x10000000; //First 256M can't be configured thru aperture
constexpr size_t VERIFY_BYTES = 1024; //Verify first and last 1kb

struct ApertureCase {
    uint32_t bar_index = 0;
    uint8_t aperture_index = 0;
    uint32_t global_index = 0;
    uint64_t bar_offset = 0;
    uint64_t target_addr = 0;
    uint8_t pattern_byte = 0;
    std::vector<uint8_t> original_start;
    std::vector<uint8_t> original_end;
    bool pattern_written = false;
};

std::string hex_u64(uint64_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::vector<ApertureCase> make_aperture_cases()
{
    std::vector<ApertureCase> cases;
    cases.reserve(15);

    uint32_t global_index = 0;
    for (uint8_t aperture = 1; aperture <= 7; ++aperture) {
        cases.push_back({
            2,
            aperture,
            global_index,
            BAR23_FIRST_CONFIGURABLE_OFFSET +
                static_cast<uint64_t>(aperture - 1) * APERTURE_SIZE,
            DMEM_BASE + static_cast<uint64_t>(global_index) * APERTURE_SIZE,
            static_cast<uint8_t>(0x20 | aperture),
            {},
            {},
            false
        });
        ++global_index;
    }

    for (uint8_t aperture = 0; aperture <= 7; ++aperture) {
        cases.push_back({
            4,
            aperture,
            global_index,
            static_cast<uint64_t>(aperture) * APERTURE_SIZE,
            DMEM_BASE + static_cast<uint64_t>(global_index) * APERTURE_SIZE,
            static_cast<uint8_t>(0x40 | aperture),
            {},
            {},
            false
        });
        ++global_index;
    }

    return cases;
}

std::string pattern_name(uint8_t pattern_byte)
{
    std::ostringstream stream;
    stream << "byte=0x" << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<uint32_t>(pattern_byte);
    return stream.str();
}

std::vector<DevMemSpec> make_devmem_specs(uint32_t control_bar_index,
                                          uint64_t control_module_base,
                                          const std::vector<ApertureCase>& cases)
{
    std::vector<DevMemSpec> specs;
    specs.reserve(cases.size());

    for (const auto& aperture_case : cases) {
        DevMemSpec spec;
        spec.control_bar_index = control_bar_index;
        spec.control_module_base = control_module_base;
        spec.dmem_bar_index = static_cast<uint8_t>(aperture_case.bar_index);
        spec.aperture_index = aperture_case.aperture_index;
        spec.identity = IDENTITY;
        spec.target_addr = aperture_case.target_addr;
        spec.size = APERTURE_SIZE;
        spec.aperture_bar_offset = aperture_case.bar_offset;
        spec.pattern = pattern_name(aperture_case.pattern_byte);
        specs.push_back(std::move(spec));
    }

    return specs;
}

void log_aperture_step(Logger* logger,
                       const char* phase,
                       const ApertureCase& aperture_case)
{
    if (logger == nullptr) {
        return;
    }

    std::ostringstream stream;
    stream << "sequential_aperture_mapping " << phase
           << " aperture bar_id=" << aperture_case.bar_index
           << " apt_id=" << static_cast<uint32_t>(aperture_case.aperture_index)
           << " target_addr=" << hex_u64(aperture_case.target_addr)
           << " bar_offset=" << hex_u64(aperture_case.bar_offset)
           << " pattern=" << pattern_name(aperture_case.pattern_byte)
           << " complete";
    logger->info(stream.str());
}

bool read_window_edge(const DevMem& devmem,
                      uint64_t edge_offset,
                      std::vector<uint8_t>& data)
{
    data.assign(VERIFY_BYTES, 0);
    return devmem.read(edge_offset, data.data(), data.size());
}

bool write_window_edge(const DevMem& devmem,
                       uint64_t edge_offset,
                       const std::vector<uint8_t>& data)
{
    return devmem.write(edge_offset, data.data(), data.size());
}

std::string aperture_name(const ApertureCase& aperture_case)
{
    std::ostringstream stream;
    stream << "bar" << aperture_case.bar_index << (aperture_case.bar_index + 1)
           << "_aperture" << static_cast<uint32_t>(aperture_case.aperture_index);
    return stream.str();
}

}

// pcie_bar_read32 : To read one 32-bit word from this PCIe module BAR window.
// @input: args["offset"] offset relative to the PCIe module register window.
// @output: TestResult metrics include offset, absolute_bar_offset, and value.
TestResult PCIeModule::pcie_bar_read32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_read32(ti);
}

// pcie_bar_scan32 : To scan a small range of 32-bit words from this PCIe module BAR window.
// @input: args["offset"] start offset, args["words"] number of 32-bit words.
// @output: TestResult metrics include offset, words, and word_N values.
TestResult PCIeModule::pcie_bar_scan32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_scan32(ti);
}

// sequential_aperture_mapping : Configure all configurable BAR23/BAR45 apertures
// and verify each data window with unique start/end patterns.
TestResult PCIeModule::sequential_aperture_mapping(TestInfo& ti)
{
    TestMetrics metrics = {
        {"aperture_count", "15"},
        {"aperture_size", hex_u64(APERTURE_SIZE)},
        {"verify_bytes_per_edge", std::to_string(VERIFY_BYTES)},
    };

    auto make_result = [&](bool passed,
                           const std::string& error_description = {},
                           const std::string& error_details = {}) {
        return TestResult{
            ti.test_name,
            ti.target_name,
            passed,
            metrics,
            error_description,
            error_details
        };
    };

    if (ti.hal == nullptr) {
        return make_result(false, "HAL context is not available");
    }

    auto cases = make_aperture_cases();
    auto devmems = ti.hal->open_multi_devmem(
        ctx_,
        make_devmem_specs(bar_index_, reg_offset_, cases),
        ti.logger);

    if (devmems.size() != cases.size()) {
        const auto details = devmems.empty() ? "no devmem windows were returned" : devmems.front().error();
        return make_result(false, "open devmem windows failed", details);
    }

    for (size_t i = 0; i < devmems.size(); ++i) {
        if (!devmems[i].valid()) {
            return make_result(false,
                               "open devmem window failed",
                               aperture_name(cases[i]) + ": " + devmems[i].error());
        }
        log_aperture_step(ti.logger, "configured", cases[i]);
    }

    std::string failure_description;
    std::string failure_details;

    for (size_t i = 0; i < cases.size(); ++i) {
        auto& aperture_case = cases[i];
        const auto& devmem = devmems[i];

        if (!read_window_edge(devmem, 0, aperture_case.original_start) ||
            !read_window_edge(devmem, APERTURE_SIZE - VERIFY_BYTES, aperture_case.original_end)) {
            failure_description = "aperture original read failed";
            failure_details = aperture_name(aperture_case);
            break;
        }
        log_aperture_step(ti.logger, "read_original", aperture_case);

        const std::vector<uint8_t> expected(VERIFY_BYTES, aperture_case.pattern_byte);
        if (!write_window_edge(devmem, 0, expected) ||
            !write_window_edge(devmem, APERTURE_SIZE - VERIFY_BYTES, expected)) {
            failure_description = "aperture pattern write failed";
            failure_details = aperture_name(aperture_case);
            break;
        }

        aperture_case.pattern_written = true;
        log_aperture_step(ti.logger, "wrote", aperture_case);
    }

    if (failure_description.empty()) {
        for (size_t i = 0; i < cases.size(); ++i) {
            const auto& aperture_case = cases[i];
            const auto& devmem = devmems[i];
            const std::vector<uint8_t> expected(VERIFY_BYTES, aperture_case.pattern_byte);
            std::vector<uint8_t> actual_start;
            std::vector<uint8_t> actual_end;

            if (!read_window_edge(devmem, 0, actual_start) ||
                !read_window_edge(devmem, APERTURE_SIZE - VERIFY_BYTES, actual_end)) {
                failure_description = "aperture pattern read failed";
                failure_details = aperture_name(aperture_case);
                break;
            }
            log_aperture_step(ti.logger, "read", aperture_case);

            if (!common::pattern::compare(expected, actual_start) ||
                !common::pattern::compare(expected, actual_end)) {
                failure_description = "aperture pattern compare failed";
                failure_details = aperture_name(aperture_case);
                break;
            }

            log_aperture_step(ti.logger, "compared", aperture_case);
        }
    }

    bool restore_ok = true;
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& aperture_case = cases[i];
        if (!aperture_case.pattern_written) {
            continue;
        }

        const auto& devmem = devmems[i];
        const auto restored = write_window_edge(devmem, 0, aperture_case.original_start) &&
                              write_window_edge(devmem,
                                                APERTURE_SIZE - VERIFY_BYTES,
                                                aperture_case.original_end);
        restore_ok = restored &&
                     restore_ok;
        if (restored) {
            log_aperture_step(ti.logger, "restored", aperture_case);
        }
    }

    metrics["configured"] = std::to_string(cases.size());
    metrics["restore_status"] = restore_ok ? "ok" : "failed";

    if (!failure_description.empty()) {
        if (!restore_ok) {
            failure_details += "; restore failed for one or more aperture windows";
        }
        return make_result(false, failure_description, failure_details);
    }

    metrics["verified"] = std::to_string(cases.size());

    return make_result(restore_ok,
                       restore_ok ? "" : "aperture restore failed",
                       restore_ok ? "" : "one or more aperture windows failed restore");
}
