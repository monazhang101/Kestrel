#pragma once

#include "BaseDevice.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

class TPUDevice : public BaseDevice {
private:
    static constexpr uint64_t PCIE_REG_OFFSET = 0x0000;
    static constexpr uint64_t PCIE_REG_SIZE   = 0x1000;

    static constexpr uint64_t ISI0_REG_OFFSET = 0x1000;
    static constexpr uint64_t ISI1_REG_OFFSET = 0x2000;
    static constexpr uint64_t ISI_REG_SIZE    = 0x1000;

    static constexpr uint64_t PMU_REG_OFFSET  = 0x3000;
    static constexpr uint64_t PMU_REG_SIZE    = 0x1000;

    static constexpr uint64_t MC_REG_OFFSET   = 0x4000;
    static constexpr uint64_t MC_REG_SIZE     = 0x1000;

    static constexpr uint64_t DDP_REG_OFFSET  = 0x5000;
    static constexpr uint64_t DDP_REG_SIZE    = 0x1000;

    void* reg_addr(uint64_t reg_offset) const;
    void* pcie_reg_addr() const;
    void* isi_reg_addr(size_t index) const;
    void* pmu_reg_addr() const;
    void* memory_reg_addr() const;
    void* ddp_reg_addr() const;

    TestResult Identify(const TestArgs& args);
    TestResult ReadFru(const TestArgs& args);
    TestResult BarProbe(const TestArgs& args);
    TestResult RegisterBlockProbe(const TestArgs& args);
    TestResult PcieLinkStatusCheck(const TestArgs& args);
    TestResult IsiLinkUp(const TestArgs& args);
    TestResult PmuReadSensor(const TestArgs& args);
    TestResult MemoryGetInventory(const TestArgs& args);
    TestResult ErrorCounterSnapshot(const TestArgs& args);
    TestResult PcieDmaDataTransfer(const TestArgs& args);

public:
    TPUDevice(const std::string& logical_name, const DeviceContext& ctx);

    TPUDevice(const TPUDevice&) = delete;
    TPUDevice& operator=(const TPUDevice&) = delete;

    ~TPUDevice() override = default;

    void print_tree() const
    {
        std::cout << get_name() << " [TPU]"
                  << " bdf=" << ctx_.bdf
                  << " vid=0x" << std::hex << ctx_.vendor_id
                  << " did=0x" << ctx_.device_id
                  << " bar_size=0x" << ctx_.bar_size
                  << std::dec << std::endl;

        // PCIe/ISI/PMU/DDP/memory-controller are intentionally internal blocks.
        // They are not printed as logical child devices.
    }
};
