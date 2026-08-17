#include "diag/device/TPUDevice.h"

#include "diag/core/Common.h"

#include <iostream>

TPUDevice::TPUDevice(const std::string& logical_name,
                     const DeviceContext& ctx,
                     uint32_t tpu_index)
    : BaseDevice(logical_name, ctx), tpu_index_(tpu_index)
{
    _add_test("identify", [this](TestInfo& ti) { return Identify(ti); });
    _add_test("soc_gpio_dir_set", [this](TestInfo& ti) { return SocGpioDirSet(ti); });
    _add_test("soc_gpio_read", [this](TestInfo& ti) { return SocGpioRead(ti); });
    _add_test("soc_gpio_write", [this](TestInfo& ti) { return SocGpioWrite(ti); });

    pcie_ = std::make_unique<PCIeModule>(
        logical_name + ".PCIE_0", ctx_, PCIE_REG_OFFSET, PCIE_REG_SIZE);

    pmu_ = std::make_unique<PMUModule>(
        logical_name + ".PMU_0", ctx_, PMU_REG_OFFSET, PMU_REG_SIZE);

    for (size_t i = 0; i < DDP_COUNT; ++i) {
        ddp_modules_.push_back(std::make_unique<DDPModule>(
            logical_name + ".DDP_" + std::to_string(i),
            ctx_,
            static_cast<uint32_t>(i),
            DDP_REG_OFFSET + i * DDP_REG_SIZE,
            DDP_REG_SIZE));
    }

    for (size_t i = 0; i < ISI_COUNT; ++i) {
        isi_modules_.push_back(std::make_unique<ISIModule>(
            logical_name + ".ISI_" + std::to_string(i),
            ctx_,
            static_cast<uint32_t>(i),
            ISI_REG_OFFSET + i * ISI_REG_SIZE,
            ISI_REG_SIZE));
    }
}

ISIModule* TPUDevice::isi(size_t index) const
{
    if (index >= isi_modules_.size()) {
        return nullptr;
    }
    return isi_modules_[index].get();
}

DDPModule* TPUDevice::ddp(size_t index) const
{
    if (index >= ddp_modules_.size()) {
        return nullptr;
    }
    return ddp_modules_[index].get();
}

std::vector<BaseDevice*> TPUDevice::child_targets() const
{
    std::vector<BaseDevice*> children;
    children.push_back(pcie_.get());
    children.push_back(pmu_.get());
    for (const auto& ddp_module : ddp_modules_) {
        children.push_back(ddp_module.get());
    }
    for (const auto& isi_module : isi_modules_) {
        children.push_back(isi_module.get());
    }
    return children;
}

void TPUDevice::print_tree() const
{
    std::cout << get_name() << " [TPU]"
              << " bdf=" << ctx_.bdf
              << " vid=0x" << std::hex << ctx_.vendor_id
              << " did=0x" << ctx_.device_id
              << " bar_size=0x" << ctx_.bar_size
              << std::dec << std::endl;

    for (const auto* child : child_targets()) {
        std::cout << "  |-- " << child->get_name() << std::endl;
    }
}

// identify : To read ATLAS identity registers and report stable hardware facts.
// @input: none.
// @output: TestResult metrics include bdf, vendor_id, device_id, chip_id, and revision.
TestResult TPUDevice::Identify(TestInfo& ti)
{
    (void)ti.args;
    return {"identify", get_name(), true, {
        {"product", "ATLAS"},
        {"bdf", ctx_.bdf},
        {"vendor_id", "0x" + std::to_string(ctx_.vendor_id)},
        {"device_id", "0x" + std::to_string(ctx_.device_id)},
        {"chip_id", "ATLAS_CHIP_PSEUDO"},
        {"revision", "A0"}
    }};
}

// soc_gpio_dir_set : To configure a SoC GPIO pin direction.
// @input: args["pin"] GPIO pin index, args["direction"] input/output.
// @output: TestResult metrics include pin, direction, and status.
TestResult TPUDevice::SocGpioDirSet(TestInfo& ti)
{
    auto pin = common::args::get_string(ti.args, "pin", "0");
    auto direction = common::args::get_string(ti.args, "direction", "input");
    // Pseudocode: route GPIO direction control through PMU, PCIe, or GPIO index access.
    return {"soc_gpio_dir_set", get_name(), true, {
        {"pin", pin},
        {"direction", direction},
        {"status", "configured"}
    }};
}

// soc_gpio_read : To read a SoC GPIO pin value.
// @input: args["pin"] GPIO pin index.
// @output: TestResult metrics include pin and value.
TestResult TPUDevice::SocGpioRead(TestInfo& ti)
{
    auto pin = common::args::get_string(ti.args, "pin", "0");
    // Pseudocode: route GPIO read through PMU, PCIe, or GPIO index access.
    return {"soc_gpio_read", get_name(), true, {
        {"pin", pin},
        {"value", "1"}
    }};
}

// soc_gpio_write : To write a SoC GPIO pin value.
// @input: args["pin"] GPIO pin index, args["value"] GPIO value.
// @output: TestResult metrics include pin, value, and status.
TestResult TPUDevice::SocGpioWrite(TestInfo& ti)
{
    auto pin = common::args::get_string(ti.args, "pin", "0");
    auto value = common::args::get_string(ti.args, "value", "1");
    // Pseudocode: route GPIO write through PMU, PCIe, or GPIO index access.
    return {"soc_gpio_write", get_name(), true, {
        {"pin", pin},
        {"value", value},
        {"status", "written"}
    }};
}
