# ISML Diagnostic Development

`isml_diag` separates target construction, testcase implementation, and
platform configuration:

```text
include/diag/modules/        Module interfaces and testcase declarations
testcases/<target_type>/     Module construction, registration, and testcase implementations
policies/testcases/          Optional testcase argument ranges
policies/product/            Product topology and module instances
cli/                         Command-line entry point
bindings/                    Python bindings
```

## Select a device and module

CLI targets select a physical TPU: `TPU0`, `TPU1`, etc. The default is `TPU0`;
numbering follows discovery order. Test names include the module prefix:

```sh
./build/isml_diag pcie_example
./build/isml_diag ddp_example --target TPU1
./build/isml_diag pcie_sequential_aperture_mapping --target TPU0
./build/isml_diag soc_example
```

A module testcase's `ctx_` is instance 0 on that TPU. Other instances are
obtained from policy by type and instance ID, not vector position:

```cpp
auto& ddp1 = ti.module("ddp", 1);
auto* phal = ti.phal_ctx;
phal_block_ctx_enter(phal, TOP_U_DDP_1_OFFSET);
auto status = phal_read(phal, offset, &value);
phal_block_ctx_exit(phal, TOP_U_DDP_1_OFFSET);
```

`ti.module()` returns a prepared `DeviceContext&`; it does not run another
case. Missing instances fail the case. Get contexts before starting worker
threads and join workers before returning. PHAL is initialized by the
framework as a chip-level context in `TestInfo::phal_ctx`; module contexts do
not own PHAL roots.

`SocModule` is a sibling under `TPUDevice`, with cases in `testcases/soc/`.
It has no register base; its cases obtain all hardware contexts via `ti.module()`.

## Add a testcase

1. Declare the testcase method in `include/diag/modules/<Module>.h`.
2. Register its local name, defaults, formats, and callback with `_add_test()`
   in `testcases/<target_type>/<Module>.cpp`. This is the only CLI registration
   point; the module prefix becomes the CLI target prefix (`pcie_`, `pmu_`, and
   so on).
3. Implement the method in `testcases/<target_type>/*.cpp` (or the module source
   for a small primitive).
4. Add optional platform limits under the same testcase name in
   `policies/testcases/<target_type>.yaml`.
5. Add a new testcase source file to the root `CMakeLists.txt` when needed.

`_add_test("example", ...)` in DDP registers `ddp_example`. Policy keys remain
local names (`example` in `ddp.yaml`).

Defaults and argument formats belong in `_add_test()`. YAML policy files only
restrict values allowed on a platform; they do not redefine testcase behavior.
The framework creates one chip-level register PHAL context and one private
aperture PHAL context when a testcase starts. Both start with
`env.base = 0x20000000` and are deinitialized when the testcase returns.
`_add_test()` has no PHAL-initialization option. Case code uses `ti.phal_ctx`
for direct register access, while an allocated buffer can be accessed with
`mem_read/write(buffer, offset, ...)`. The buffer keeps the HAL association
internally.

## Logging

Testcase code writes diagnostic output through `ti.logger`:

```cpp
ti.logger->error("operation failed");
ti.logger->info("starting register check");
ti.logger->debug("resolved offset=" + common::format::hex(offset));
ti.logger->trace("entering worker");
```

The logger filters messages by level at execution time. The default `info`
level prints errors and informational messages; `debug` also prints debug
messages; `trace` prints every level. Use the CLI option below to select the
filter for one run:

```sh
./build/isml_diag pcie_example --log-level debug
./build/isml_diag pcie_dma_data_transfer --log-level trace
```

`--log-level error` restricts output to errors. The selected level is shared
with framework and PHAL diagnostics for the active testcase.

## Add a module type

1. Add the module interface under `include/diag/modules/` and its constructor
   under `testcases/<target_type>/<Module>.cpp`.
2. Derive the module from `TestTarget` and pass its lowercase target type to the
   base constructor, for example `TestTarget(name, "foo", ctx)`.
3. Add testcase implementations under `testcases/<target_type>/`.
4. If policy limits are needed, add
   `policies/testcases/<target_type>.yaml`. The filename stem is the target type
   used during policy lookup, so `foo.yaml` matches target type `"foo"`.
5. Add the module to the parent target's topology construction and list its
   sources in the root `CMakeLists.txt`.

Adding a module type does not require a target-type enum or a parser change.

## Register and device-memory examples

Register operations use PHAL directly:

```cpp
auto* phal = ti.phal_ctx;
phal_block_ctx_enter(phal, block_offset);
auto status = phal_read(phal, reg_offset, &actual);
phal_block_ctx_exit(phal, block_offset);
```

The framework owns the context for the duration of one testcase and deinitializes
it after the testcase returns. Pair every block enter/exit. Module objects carry
topology and testcase registration; register and memory offsets are selected by
the testcase from chip-level interfaces.

Allocate memory before accessing it. The allocation selects the offset;
read/write offsets are relative to that buffer:

```cpp
auto buffer = ti.hal->device_mem_alloc(HQC_SRAM_0, 1024);
if (!buffer.valid()) return PHAL_STATUS_ERROR;
std::vector<uint8_t> expected(1024, 0xab), actual(1024);
auto status = mem_write(buffer, 0, expected);
if (status != PHAL_STATUS_OK) return status;
status = mem_read(buffer, 0, &actual);
// buffer.release() or automatic release at scope exit.
```

The third `device_mem_alloc` argument is optional. Omit it to let the allocator
choose the first available range, or pass a fixed offset within the selected
memory region when the testcase must use a known location:

```cpp
auto automatic = ti.hal->device_mem_alloc(DMEM, 1024);
auto fixed = ti.hal->device_mem_alloc(DMEM, 1024, 0x2000);
```

Both buffers are accessed with offsets relative to their own allocation, for
example `mem_read(fixed, 0, &actual)`. The fixed range must be aligned, inside
the region, and free at allocation time.

The same API accepts `DMEM`, `DDP0_ILM`, `DDP1_ILM`, `PMU_DLM`, etc. Device-memory regions
are owned by the active chip HAL session and do not use a module base. Pass a
fixed offset as the third argument when a case must probe a specific address;
omit it to allocate the first available range. Host DMA allocation uses
`ti.hal->host_mem_alloc(size_bytes)`.

- Start addresses, allocation sizes, read/write offsets and lengths use
  4-byte alignment. Zero sizes and unsupported regions fail allocation.
- A per-device mutex protects allocation/free. First fit reuses free gaps;
  insufficient contiguous space returns an invalid buffer without waiting.
- Buffers are move-only. Keep them within the owning HAL/test execution's
  lifetime. Wait for hardware completion before release. Sharing one buffer
  between threads requires caller synchronization.
- RCF pools currently cover whole regions. DMEM's configured allocatable
  range is `[0x10000000, 0x8100000000)`; the Atlas table defines the capacity
  and aperture geometry in `isml_lib/implementer/src/atlas/memory_regions.cpp`.
  Atlas tables live in `isml_lib/implementer/src/atlas/memory_regions.cpp`.
- DMEM IO configures BAR4/aperture0 to cover the current test range. A separate
  mutex serializes configuration and payload access. AIC S/I/VMEM allocation
  remains unimplemented.
- RCF regions use the same allocated-buffer API. Components with fixed layouts,
  such as the HQC admin queue, reserve their fixed range once and then use
  `mem_read/write(buffer, offset, ...)` internally.
  Raw BAR access likewise bypasses the allocation table.

### Run DMA H2D

Rebuild on the FPGA host after syncing source changes:

```sh
cmake --build build-fpga -j 4
# Defaults: TPU0, H2D, 4096 bytes (4 KiB), random pattern, 1000 ms.
sudo ./build-fpga/isml_diag pcie_dma_data_transfer
# 128 MiB H2D, with a 30-second completion timeout:
sudo ./build-fpga/isml_diag pcie_dma_data_transfer \
  --target TPU0 --direction h2d --size-bytes 0x08000000 \
  --pattern incremental --timeout-ms 30000
```

The CLI name includes `pcie_`; `_add_test("dma_data_transfer", ...)` adds that
prefix automatically. Arguments are optional because registration supplies
defaults through `ti.args`. Size is in bytes: use `--size-bytes`, not `--size`.
H2D initializes the host source, submits one DMA transfer, then reads DMEM once
through MMIO and compares the data. It does no preliminary DMEM write/read or
backup/restore. Host DMA requests are limited to 128 MiB, subject to allocation
success; `timeout_ms` is not a time limit for the final MMIO verification.
MMIO transfers of at least 1 MiB print progress at start, every 10 seconds, and
at completion. Progress is checked every 64 KiB and is visible at info level.

The DMA case allocates its DMEM source/destination instead of accepting
`device_offset` or `return_offset`. FW still requires DMA lengths in 32-byte
units. Failed DMA requests retain their device allocations because completion
is uncertain; do not rediscover/reuse the device until hardware has stopped.
The existing host-buffer timeout/drain behavior is unchanged.

The aperture case allocates space for its 15 ranges (plus alignment padding),
then holds the aperture mutex through all explicit configuration and IO.
Runner testcase execution remains serialized. The allocator and normal DMEM
IO support threads inside a case; no parallel testcase scheduler is added.

TODO: FW reserved ranges, confirmed DMEM capacity, concurrent aperture windows,
and multi-process coordination. The existing HQC/FW prototype is left as-is;
it is scheduled for replacement, so dynamic queue integration is deferred.

The PMU, DDP, and ISI examples use the same framework-owned chip PHAL context
and device-memory allocation path. Register block offsets must come from the
generated Atlas CSR headers; module constructors retain construction, topology,
testcase registration, and concurrency orchestration, while testcase code owns
the register and memory offsets it needs.

The PCIe example enters `TOP_U_PCIE_0_OFFSET`, reads the PCIe register through
PHAL, and scans the head and tail of an allocated DMEM buffer with explicit
buffer offsets. The framework supplies the chip root `env.base = 0x20000000`.

For raw BAR debugging, `pcie_bar_read32` takes an explicit `--bar-index` and an
absolute BAR `--offset`. It does not add a module base. For example,
`pcie_bar_read32 --target TPU0 --bar-index 0 --offset 0x36100000` checks the
known PCIe DID/VID location. It can also address DDP/ISI offsets through the
PCIe target, but it issues the same MMIO read as `phal_read` at that location.
In particular, do not read `0x32080800` on `pcie-b7` until the hardware access
that stalled that server is resolved.



## Software verification

On Linux, make sure flowra_sources folder is cloned outside isml_suite2. Configure
the normal build with `-DISML_BUILD_TESTS=ON`, build by execute:

```sh
make -C isml_kmod
cmake -S . -B build
cmake --build build
```

To unbind ProtonHal VFIO driver, remove DKMS package first, then reboot and
load our isml_kmod driver:

```sh
sudo dpkg -P proton-drv-vfio-pci-kernel-dkms
sudo reboot

sudo insmod ./isml_kmod/isml_diag.ko
```
