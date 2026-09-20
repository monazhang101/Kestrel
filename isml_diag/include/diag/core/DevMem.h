/*
 * Common device-memory access over the PCIe BAR path.
 *
 * These helpers initialize the PCIe PHAL context. The PHAL aperture API enters
 * its AXICLK block internally before programming and verifying one aperture.
 * Data is then read or written through the mapped BAR via common::bar.
 * These helpers are available to every diagnostic module.
 */
#pragma once

#include "diag/core/TestInfo.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct DeviceContext;
struct TestInfo;

namespace common::devmem {

struct Window {
    uint32_t bar_index = 0;
    uint8_t aperture_index = 0;
    uint8_t identity = 0;
    uint64_t target_addr = 0;
    uint64_t size = 0;
    uint64_t bar_offset = 0;
};

bool read(TestInfo& ti,
          const DeviceContext& ctx,
          const Window& window,
          uint64_t offset,
          void* data,
          size_t len,
          std::string* error = nullptr);

bool write(TestInfo& ti,
           const DeviceContext& ctx,
           const Window& window,
           uint64_t offset,
           const void* data,
           size_t len,
           std::string* error = nullptr);

}

namespace common {
// One 32-bit word at dmem_base + addr. Uses BAR4/aperture0/identity0;
// independent of the module register block. Framework prepares pcie_phal.
TestStatus dmem_read(DeviceContext& ctx, uint64_t addr, uint32_t* data);
TestStatus dmem_write(DeviceContext& ctx, uint64_t addr, uint32_t data);
}

using common::dmem_read;
using common::dmem_write;
