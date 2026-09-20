#include "diag/modules/DDPModule.h"

DDPModule::DDPModule(const std::string& name, const DeviceContext& ctx,
                         const ModuleInstanceConfig& config)
    : TestTarget(name, "ddp", ctx), ddp_id_(config.index)
{
    ctx_.reg_bar_index = config.bar_index;
    ctx_.reg_base_offset = config.reg_base_offset;
    ctx_.reg_size = config.reg_size;
    _add_test("example",
              {{"block_offset", "0", "offset"},
               {"reg_offset", "0", "offset"},
               {"dmem_offset", "0", "offset"},
               {"value", "0x12345678", "u32"},
               {"write_enable", "0", "bool"}},
              [this](TestInfo& ti) { return example(ti); }, true);
}
