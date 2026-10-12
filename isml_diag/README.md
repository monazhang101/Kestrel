# ISML Diagnostic Development

`isml_diag` separates testcase intent, common hardware access, and
product-specific implementation:

```text
include/diag/modules/        Module interfaces and testcase declarations
testcases/<target_type>/     Module construction, registration, and testcase code
policies/testcases/          Ordered parameter lists used by -all
policies/product/            Product discovery profiles and module topology
cli/                         Command-line entry point
bindings/                    Optional pybind11 bindings
```

## Target and dispatch model

CLI targets are physical TPUs named `TPU0`, `TPU1`, and so on. `--target`
defaults to `TPU0`, and numbering currently follows discovery order.

```sh
./build-fpga/isml_diag pcie_example
./build-fpga/isml_diag ddp_example --target TPU1
```

The test name's module prefix routes the request to that TPU's instance-0
module object. Other policy-created DDP and ISI objects describe topology; they
are not CLI targets and do not supply per-instance base addresses. A testcase
owner decides which hardware instances to cover, loops over their generated CSR
offsets, and logs each instance. Testcase code has no `ti.module()` lookup API.

`DeviceManager` currently serializes discovery and testcase dispatch.

## Add a testcase

1. Declare the method in `include/diag/modules/<Module>.h`.
2. Register its local name, defaults, formats, and callback with `_add_test()`
   in `testcases/<target_type>/<Module>.cpp`.
3. Implement it under `testcases/<target_type>/` and add a new source file to
   the root `CMakeLists.txt` when needed.
4. Optionally add ordered `-all` runs to
   `policies/testcases/<target_type>.yaml`.

For example, `_add_test("example", ...)` in DDP registers the CLI command
`ddp_example`. Policy keys use the local name. Each policy line supplies one
run; omitted values use the `_add_test()` defaults:

```text
dma_data_transfer:
  direction=h2d, size_bytes=4096, pattern=incremental
  direction=d2h, size_bytes=0x100000, pattern=random, timeout=30000
```

In policy files, `timeout` maps to `timeout_ms`. `-all` cannot be combined
with CLI testcase arguments. CMake copies the policy tree beside the executable;
use `--policy-dir` only to override it.

Every testcase receives:

- `ti.args`: registered defaults plus CLI or policy values.
- `ti.logger`: framework logging selected by `--log-level`.
- `ti.hal`: the HAL context bound to the selected TPU.
- `ti.phal_ctx`: a framework-owned chip-level PHAL context.

The framework creates the chip PHAL context and a private PCIe PHAL context at
testcase entry, both rooted at `0x20000000`, and deinitializes them on return.
PCIe aperture and direct HQC operations use
`HalContext::with_pcie_context()`.

## Ownership boundaries

Testcase code owns diagnostic intent: instance loops, register sequences,
expected values, comparisons, save/restore behavior, and logs. Ordinary
register access stays in the testcase and uses common HAL/PHAL IO.

Common HAL code owns BAR/MMIO access, PHAL context lifetime, PCIe aperture
serialization, memory allocation, host DMA buffers, and shared access helpers.

Add an `Implementer` operation only when the same semantic operation has a
confirmed different realization across TPU generations. PCIe `dma_copy()` is
the current example: the testcase selects and verifies the transfer, while the
Atlas implementation owns HQC descriptor encoding and submission.
Product-specific memory maps and fixed capability tables may also live behind
`Implementer`. Register-heavy DDP/ISI/PMU cases alone do not require matching
implementer classes.

Use `common::pattern::generate()` and `common::pattern::compare()` for ordinary
DMA and memory-integrity payloads. Generate data locally only when its exact bit
layout is part of the testcase semantics.

## Register and memory access

Register operations use the testcase PHAL context and generated block offsets:

```cpp
auto* phal = ti.phal_ctx;
phal_block_ctx_enter(phal, block_offset);
auto status = phal_read(phal, reg_offset, &actual);
phal_block_ctx_exit(phal, block_offset);
```

Device memory must be allocated before access. Read/write offsets are relative
to the returned buffer:

```cpp
auto automatic = ti.hal->device_mem_alloc(DMEM, 1024);
auto fixed = ti.hal->device_mem_alloc(DMEM, 1024, 0x2000);
status = mem_write(fixed, 0, expected);
status |= mem_read(fixed, 0, &actual);
```

- Allocations, offsets, and IO lengths require 4-byte alignment.
- Allocation is first-fit and protected per device. Fixed ranges must be free
  and inside the selected region.
- Buffers are move-only and release their allocation automatically. Hardware
  completion must be known before release.
- RCF pools cover their configured regions. Direct HQC submission dynamically
  allocates descriptor storage from `HQC_SRAM_0`.
- Atlas DMEM is currently configured as `[0x10000000, 0x8100000000)` and uses
  BAR4/aperture0 for IO. A mutex covers aperture configuration and payload IO.
- AIC SMEM/IMEM/VMEM allocation is not implemented.

Host DMA buffers use `ti.hal->host_mem_alloc(size_bytes)`. The current host-DMA
path is iHal-only, limited to 128 MiB per allocation, and requires the matching
`/dev/isml_diag*` device.

## Build and verify

- The production build supports Linux. It requires a complete ProtonHal/CSR tree
at `isml_ipc/external/`; git command to clone / update submodules:
```sh
git clone --recurse-submodules https://gitlab.infiscale.dev/isml/isml_suite2
```
or
```sh
git pull
git submodule update --init --recursive
```
- Build isml_diag and copy to the target fpga/emu tester:
```sh
cmake -S . -B build-fpga
cmake --build build-fpga
```
The `build-fpga/` directory automatically includes the policy tree. Copy both
`build-fpga/` and `isml_kmod/` to the target FPGA/EMU tester.

- Build the kernel module on the tester so it matches the running kernel. The
  tester must provide build tools and `/lib/modules/$(uname -r)/build`:
```sh
make -C isml_kmod
```
The kernel module is built separately from CMake. Loading it and binding the PCI device are environment-specific operations.
- Remove proton driver on the target tester:
```sh
sudo dpkg -P proton-drv-vfio-pci-kernel-dkms
sudo reboot
```
- Load isml_kmod by:
```sh
sudo insmod ./isml_kmod/build/isml_diag.ko
```
- Check whether isml_diag is ready to run test cases:
```sh
sudo ./build-fpga/isml_diag discover --tree
```

## PCIe DMA and aperture cases

`pcie_dma_data_transfer` defaults to TPU0, H2D, 4096 bytes, random pattern, and
a 1000 ms timeout. It supports H2D, D2H, D2I2D, and D2S2D; sizes must be nonzero
multiples of 32 bytes. D2V2D remains unavailable.

H2D performs one DMA transfer followed by one DMEM MMIO readback and comparison.
The testcase allocates its device offsets, and the HQC descriptor represents
length in 32-byte units. If completion is uncertain, the direct path retains
the affected device-memory and descriptor-SRAM allocations.

`pcie_sequential_aperture_mapping` allocates 16 MiB and checks 15 distinct
1 MiB ranges. It saves, writes, verifies, and restores both 1 KiB edges while
holding the PCIe aperture mutex.

`pcie_bar_read32` performs a raw read from an explicit BAR index and absolute
BAR offset; it does not add a module base.

`HalContext::secondary_bus_reset(bool reset)` sets or clears the standard
Secondary Bus Reset bit on the selected TPU's upstream bridge and verifies it
by readback. `pcie_example` demonstrates assert, delay, deassert, and recovery
delay; the helper itself does not perform link retraining or reinitialization.

## Confirmed TODO

- Post-silicon multi-card discovery: bind each canonical TPU name to its
  expected BDF, VID, and DID, and reject missing, duplicate, or mismatched
  devices. Pre-silicon discovery currently matches VID/DID and assigns TPU names
  in discovery order because BDF is unstable.
- HQC concurrency: support concurrent submission and request/completion matching
  after the required hardware/FW contract is confirmed.
- Concurrent aperture-window use. This is low priority and is planned for
  approximately December 2026.

## Hardware assumptions to confirm

- FW-reserved ranges and physical DMEM capacity before changing the allocator's
  configured usable regions.
- D2I2D and D2S2D scratch coverage, currently limited conservatively to
  `[0, 256)` and `[0, 128)` respectively.
