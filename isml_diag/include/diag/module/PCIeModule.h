#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/implementer/PCIeImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class PCIeModule : public BaseDevice {
private:
    uint32_t bar_index_ = 0;
    uint64_t reg_base_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<PCIeImpl> impl_;

    /* ----------- Register atomic tests here ----------- */
    TestStatus pcie_bar_read32(TestInfo& ti);
    TestStatus pcie_bar_scan32(TestInfo& ti);
    TestStatus sequential_aperture_mapping(TestInfo& ti);
    TestStatus pcie_dma_data_transfer(TestInfo& ti);
    /*
    TestStatus PcieEnumCheck(TestInfo& ti);
    TestStatus PcieCapListCheck(TestInfo& ti);
    TestStatus PcieExtCapListCheck(TestInfo& ti);
    TestStatus PcieRegScan(TestInfo& ti);
    TestStatus PciePmuRegScan(TestInfo& ti);
    TestStatus PcieBarSizeGet(TestInfo& ti);
    TestStatus PcieDmemMmioScan(TestInfo& ti);
    TestStatus PcieDmemHdmaScan(TestInfo& ti);
    TestStatus PcieDmemRegScan(TestInfo& ti);
    TestStatus PcieVfioHdmaIntrSetup(TestInfo& ti);
    TestStatus PcieVfioWaitMsiIntr(TestInfo& ti);
    TestStatus PcieParallelDmaWithCompare(TestInfo& ti);
    TestStatus PcieMcIntrIsSet(TestInfo& ti);
    TestStatus PciePmuIntrTrigger(TestInfo& ti);
    TestStatus PciePmuIntrIsSet(TestInfo& ti);
    TestStatus PcieIsiIntrIsSet(TestInfo& ti);
    */

public:
    PCIeModule(const std::string& name,
               const DeviceContext& ctx,
               const ModuleInstanceConfig& config,
               std::unique_ptr<PCIeImpl> impl);

    uint64_t reg_base_offset() const { return reg_base_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    uint32_t bar_index() const { return bar_index_; }
};
