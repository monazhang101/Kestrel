#include "diag/modules/ISIModule.h"

#include "diag/core/DevMem.h"

#include <cstdint>
#include <string>

// linkup: Check whether this ISI link is physically present and link-up.
// @input: none.
// @output: TestStatus; link details are written to the testcase log.
TestStatus ISIModule::linkup(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "ISI implementation is not bound");
    }

    const auto link_status = impl_->linkup(ti);
    if (link_status != TestStatus::OK) {
        return link_status;
    }

    // Simple D-MEM access example for module testcase developers. BAR4 is the
    // low index of the BAR4/5 pair; offset 0 accesses target address 0x10000000.
    common::devmem::Window window;
    window.bar_index = 4;
    window.aperture_index = 0;
    window.identity = 0;
    window.target_addr = 0x10000000;
    window.size = 0x100000;
    window.bar_offset = 0;

    constexpr uint32_t WRITE_VALUE = 0x12345678;
    uint32_t read_value = 0;
    std::string error;

    if (!common::devmem::write(
            ti, ctx_, window, 0, &WRITE_VALUE, sizeof(WRITE_VALUE), &error)) {
        if (ti.logger != nullptr) {
            ti.logger->error("ISI D-MEM write failed: " + error);
        }
        return TestStatus::ERROR;
    }

    if (!common::devmem::read(
            ti, ctx_, window, 0, &read_value, sizeof(read_value), &error)) {
        if (ti.logger != nullptr) {
            ti.logger->error("ISI D-MEM read failed: " + error);
        }
        return TestStatus::ERROR;
    }

    if (read_value != WRITE_VALUE) {
        if (ti.logger != nullptr) {
            ti.logger->error("ISI D-MEM readback mismatch");
        }
        return TestStatus::ERROR;
    }

    return TestStatus::OK;
}
