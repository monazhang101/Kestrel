#include "diag/module/ISIModule.h"

#include <utility>

ISIModule::ISIModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<ISIImpl> impl)
    : BaseDevice(name, ctx),
      link_id_(config.index),
      bar_index_(config.bar_index),
      reg_base_offset_(config.reg_base_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    _add_test("isi_linkup", {}, [this](TestInfo& ti) { return IsiLinkup(ti); });
    _add_test("isi_setup", {{"mode", "default", "string"}},
              [this](TestInfo& ti) { return IsiSetup(ti); });
    _add_test("isi_pcie_aperture_context",
              {{"bar_index", "4", "u64"},
               {"aperture_index", "0", "u64"}},
              [this](TestInfo& ti) {
        return IsiPcieApertureContext(ti);
    });
    _add_test("isi_common_devmem_read",
              {{"bar_index", "4", "u64"},
               {"aperture_index", "0", "u64"},
               {"identity", "0", "u64"},
               {"target_addr", "0x10000000", "offset"},
               {"aperture_size", "0x100000", "bytes"},
               {"bar_offset", "0", "offset"},
               {"offset", "0", "offset"}},
              [this](TestInfo& ti) {
        return IsiCommonDevMemRead(ti);
    });
}
