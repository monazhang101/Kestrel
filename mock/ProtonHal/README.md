# Mock ProtonHal

This directory mirrors the public include and source layout used by the real
`isml_ipc/external/ProtonHal` dependency. CMake selects it only when the real
PHAL sources/PCIe header are unavailable or `KESTREL_USE_MOCK_PHAL=ON` is set.

The mock provides PHAL context lifecycle and in-memory PCIe aperture set/get
state so the diagnostic framework can compile, link, and run local smoke paths.
It does not model AXICLK register access, BAR translation, or device memory.
