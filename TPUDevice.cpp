#include "TPUDevice.h"

TPUDevice::TPUDevice(const std::string& logical_name, const DeviceContext& ctx)
    : BaseDevice(logical_name, ctx)
{
    _add_test("identify", [this](const TestArgs& args) { return Identify(args); });
    _add_test("read_fru", [this](const TestArgs& args) { return ReadFru(args); });
    _add_test("bar_probe", [this](const TestArgs& args) { return BarProbe(args); });
    _add_test("register_block_probe", [this](const TestArgs& args) { return RegisterBlockProbe(args); });
    _add_test("pcie_link_status_check", [this](const TestArgs& args) { return PcieLinkStatusCheck(args); });
    _add_test("isi_link_up", [this](const TestArgs& args) { return IsiLinkUp(args); });
    _add_test("pmu_read_sensor", [this](const TestArgs& args) { return PmuReadSensor(args); });
    _add_test("memory_get_inventory", [this](const TestArgs& args) { return MemoryGetInventory(args); });
    _add_test("error_counter_snapshot", [this](const TestArgs& args) { return ErrorCounterSnapshot(args); });
    _add_test("pcie_dma_data_transfer", [this](const TestArgs& args) { return PcieDmaDataTransfer(args); });
}

void* TPUDevice::reg_addr(uint64_t reg_offset) const
{
    return static_cast<uint8_t*>(ctx_.mapped_bar_base) + reg_offset;
}

void* TPUDevice::pcie_reg_addr() const
{
    return reg_addr(PCIE_REG_OFFSET);
}

void* TPUDevice::isi_reg_addr(size_t index) const
{
    if (index == 0) {
        return reg_addr(ISI0_REG_OFFSET);
    }

    if (index == 1) {
        return reg_addr(ISI1_REG_OFFSET);
    }

    return nullptr;
}

void* TPUDevice::pmu_reg_addr() const
{
    return reg_addr(PMU_REG_OFFSET);
}

void* TPUDevice::memory_reg_addr() const
{
    return reg_addr(MC_REG_OFFSET);
}

void* TPUDevice::ddp_reg_addr() const
{
    return reg_addr(DDP_REG_OFFSET);
}
