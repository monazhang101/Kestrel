#ifndef ISML_MOCK_ATLAS_CSR_HQC_H
#define ISML_MOCK_ATLAS_CSR_HQC_H

#include <stdint.h>

/* Compile-only stand-in for the generated HQC wrapper header. Together with
 * PCIE_U_HQC_0_OFFSET=0x40000 this yields PCIe-relative 0x50000. Hardware builds
 * must use the generated wrapper hierarchy for the deployed RTL revision. */
static const uintptr_t HQC_U_PCIE_HQC_REG_0_OFFSET = 0x10000u;

#endif
