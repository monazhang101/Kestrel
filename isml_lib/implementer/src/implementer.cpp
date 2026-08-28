#include "diag/implementer/Implementer.h"

#include "generic/generic_impl.h"

namespace atlas_impl {

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx);
std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);
std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx);
std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx);
std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx);
std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx);

}

namespace atlas_m_impl {

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx);
std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);
std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx);
std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx);
std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx);
std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx);

}

Implementer::Implementer(TPUType tpu_type)
    : tpu_type_(tpu_type)
{
}

std::unique_ptr<TPUImpl> Implementer::tpu_impl(const ModuleImplContext& ctx) const
{
    if (tpu_type_ == TPUType::Atlas) {
        return atlas_impl::make_tpu_impl(ctx);
    }
    if (tpu_type_ == TPUType::AtlasM) {
        return atlas_m_impl::make_tpu_impl(ctx);
    }
    return generic_impl::make_tpu_impl(ctx);
}

std::unique_ptr<PCIeImpl> Implementer::pcie_impl(const ModuleImplContext& ctx) const
{
    if (tpu_type_ == TPUType::Atlas) {
        return atlas_impl::make_pcie_impl(ctx);
    }
    if (tpu_type_ == TPUType::AtlasM) {
        return atlas_m_impl::make_pcie_impl(ctx);
    }
    return generic_impl::make_pcie_impl(ctx);
}

std::unique_ptr<PMUImpl> Implementer::pmu_impl(const ModuleImplContext& ctx) const
{
    if (tpu_type_ == TPUType::Atlas) {
        return atlas_impl::make_pmu_impl(ctx);
    }
    if (tpu_type_ == TPUType::AtlasM) {
        return atlas_m_impl::make_pmu_impl(ctx);
    }
    return generic_impl::make_pmu_impl(ctx);
}

std::unique_ptr<ISIImpl> Implementer::isi_impl(const ModuleImplContext& ctx) const
{
    if (tpu_type_ == TPUType::Atlas) {
        return atlas_impl::make_isi_impl(ctx);
    }
    if (tpu_type_ == TPUType::AtlasM) {
        return atlas_m_impl::make_isi_impl(ctx);
    }
    return generic_impl::make_isi_impl(ctx);
}

std::unique_ptr<DDPImpl> Implementer::ddp_impl(const ModuleImplContext& ctx) const
{
    if (tpu_type_ == TPUType::Atlas) {
        return atlas_impl::make_ddp_impl(ctx);
    }
    if (tpu_type_ == TPUType::AtlasM) {
        return atlas_m_impl::make_ddp_impl(ctx);
    }
    return generic_impl::make_ddp_impl(ctx);
}

std::unique_ptr<DMCImpl> Implementer::dmc_impl(const ModuleImplContext& ctx) const
{
    if (tpu_type_ == TPUType::Atlas) {
        return atlas_impl::make_dmc_impl(ctx);
    }
    if (tpu_type_ == TPUType::AtlasM) {
        return atlas_m_impl::make_dmc_impl(ctx);
    }
    return generic_impl::make_dmc_impl(ctx);
}
