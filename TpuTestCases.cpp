#include "TPUDevice.h"

TestResult TPUDevice::Identify(const TestArgs& args)
{
    // Pseudocode:
    // 1. read chip id / revision / stepping registers
    // 2. read board id or SKU id if exposed through BAR/ROM
    // 3. return observed identity; skucheck compares with policy
    (void)args;

    return {"identify", get_name(), true, {
        {"bdf", ctx_.bdf},
        {"vendor_id", "0x1d0f"},
        {"device_id", "0x1000"},
        {"chip_revision", "A0"},
        {"board_id", "TPU_BOARD_PSEUDO"}
    }};
}

TestResult TPUDevice::ReadFru(const TestArgs& args)
{
    // Pseudocode:
    // 1. read resume EEPROM / FRU / serial provider
    // 2. return serial, part number, SKU string
    (void)args;

    return {"read_fru", get_name(), true, {
        {"serial_number", "TPU-SN-PSEUDO"},
        {"part_number", "TPU-PN-PSEUDO"},
        {"fru_readable", "true"}
    }};
}

TestResult TPUDevice::BarProbe(const TestArgs& args)
{
    // Pseudocode:
    // 1. verify BAR is mapped
    // 2. verify BAR size is enough for known register blocks
    // 3. optionally read a safe scratch/version register
    (void)args;

    bool ok = ctx_.mapped_bar_base != nullptr && ctx_.bar_size >= DDP_REG_OFFSET + DDP_REG_SIZE;
    return {"bar_probe", get_name(), ok, {
        {"mapped", ctx_.mapped_bar_base == nullptr ? "false" : "true"},
        {"bar_size", std::to_string(ctx_.bar_size)}
    }};
}

TestResult TPUDevice::RegisterBlockProbe(const TestArgs& args)
{
    // Pseudocode:
    // 1. read module ID/version/status registers from each internal block
    // 2. report observed block presence and version facts
    // 3. keep policy comparison outside HAL
    (void)args;

    return {"register_block_probe", get_name(), true, {
        {"pcie_block", "present"},
        {"isi0_block", "present"},
        {"isi1_block", "present"},
        {"pmu_block", "present"},
        {"memory_controller_block", "present"},
        {"ddp_block", "present"}
    }};
}

TestResult TPUDevice::ErrorCounterSnapshot(const TestArgs& args)
{
    // Pseudocode:
    // 1. snapshot PCIe AER, ISI link error, HBM ECC, firmware health counters
    // 2. return raw counter values; threshold comparison happens in skucheck/connectivity
    (void)args;

    return {"error_counter_snapshot", get_name(), true, {
        {"pcie_correctable_errors", "0"},
        {"pcie_uncorrectable_errors", "0"},
        {"isi_error_count", "0"},
        {"hbm_ecc_error_count", "0"}
    }};
}
