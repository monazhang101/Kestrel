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
    _add_test("isi_linkup", [this](TestInfo& ti) { return IsiLinkup(ti); });
    _add_test("isi_setup", [this](TestInfo& ti) { return IsiSetup(ti); });
    _add_test("isi_pcie_aperture_context", [this](TestInfo& ti) {
        return IsiPcieApertureContext(ti);
    });
    _add_test("isi_common_devmem_read", [this](TestInfo& ti) {
        return IsiCommonDevMemRead(ti);
    });
}
