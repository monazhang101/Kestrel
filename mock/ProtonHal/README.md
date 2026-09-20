# Mock ProtonHal

This directory mirrors the public include and source layout used by the real
`isml_ipc/external/ProtonHal` dependency. CMake selects it only when
`ISML_USE_MOCK_PHAL=ON` is explicitly set. Missing real sources otherwise fail
configuration.

The mock provides PHAL context lifecycle and in-memory PCIe aperture set/get
state so the diagnostic framework can compile, link, and run local smoke paths.
It does not model AXICLK register access, BAR translation, or device memory.

The native `phal_component_isi_linkup(ctx, timeout_us)` symbol is provided for
compile/link checks. It returns `PHAL_STATUS_ERROR` for a valid context because
the mock cannot establish a physical ISI link; a mock ISI example must not PASS.
