#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/implementer/PCIeImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class PCIeModule : public BaseDevice {
private:
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<PCIeImpl> impl_;

    /* ----------- Register atomic tests here ----------- */
    TestResult pcie_link_status_get(TestInfo& ti);
    TestResult pcie_dma_data_transfer(TestInfo& ti);
    /*
    TestResult PcieEnumCheck(TestInfo& ti);
    TestResult PcieCapListCheck(TestInfo& ti);
    TestResult PcieExtCapListCheck(TestInfo& ti);
    TestResult PcieRegScan(TestInfo& ti);
    TestResult PciePmuRegScan(TestInfo& ti);
    TestResult PcieBarSizeGet(TestInfo& ti);
    TestResult PcieDmemMmioScan(TestInfo& ti);
    TestResult PcieDmemHdmaScan(TestInfo& ti);
    TestResult PcieDmemRegScan(TestInfo& ti);
    TestResult PcieVfioHdmaIntrSetup(TestInfo& ti);
    TestResult PcieVfioWaitMsiIntr(TestInfo& ti);
    TestResult PcieParallelDmaWithCompare(TestInfo& ti);
    TestResult PcieMcIntrIsSet(TestInfo& ti);
    TestResult PciePmuIntrTrigger(TestInfo& ti);
    TestResult PciePmuIntrIsSet(TestInfo& ti);
    TestResult PcieIsiIntrIsSet(TestInfo& ti);
    TestResult PcieLinkSpeedChange(TestInfo& ti);
    */

public:
    PCIeModule(const std::string& name,
               const DeviceContext& ctx,
               const ModuleInstanceConfig& config,
               std::unique_ptr<PCIeImpl> impl);

    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
};
