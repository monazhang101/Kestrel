# ISML Diagnostic Development

`isml_diag` separates testcase intent, common hardware access, and
product-specific implementation:

```text
include/diag/modules/        Module interfaces and testcase declarations
testcases/<target_type>/     Registration and testcase implementation
policies/testcases/          Parameter lists used by -all
policies/product/            Product discovery and module topology
cli/                         Command-line entry point
bindings/                    Optional pybind11 bindings
```

`DeviceManager` discovers PCI devices, applies product policy, maps BARs, and
creates each TPU and its modules. Testcases own register sequences, hardware
constraints, comparisons, restore behavior, and logs. `HalContext` owns PHAL
contexts, mappings, memory allocation, and shared access locks. Add a product
`Implementer` operation only when one semantic operation has different hardware
realizations; direct HQC DMA and CP copy are the current examples.

## Add a testcase

1. Declare the method in `include/diag/modules/<Module>.h`.
2. Register its local name, arguments, defaults, formats, and callback with
   `_add_test()` in `testcases/<target_type>/<Module>.cpp`.
3. Implement it under `testcases/<target_type>/` and add a new source file to
   the root `CMakeLists.txt` when needed.
4. Optionally add ordered `-all` runs to
   `policies/testcases/<target_type>.yaml`.

`_add_test("example", ...)` in PCIe registers `pcie_example`. `_add_test()` is
the source of truth for the testcase contract; do not repeat defaults in the
CLI or testcase body. Supported formats are `string`, `u64`, `u32`, `bytes`,
`ms`, `us`, `offset`, and `bool`. Numeric values accept decimal or `0x` syntax.
The framework rejects unknown arguments and invalid numeric formats. Testcases
must validate string choices, cross-argument rules, and hardware constraints.

CLI options use kebab case and are normalized to snake case:

```text
--size-bytes 4096  ->  ti.args["size_bytes"]
```

Every testcase receives:

- `ti.args`: registered defaults plus CLI or policy values.
- `ti.logger`: `error`, `info`, `flow`, and `regio` logging.
- `ti.hal`: the HAL context bound to the selected TPU.
- `ti.phal_ctx`: a framework-owned chip-level PHAL context.

Use `common::args::get_string()` and `get_u64()` for resolved arguments,
`common::pattern::generate()` and `compare()` for standard payloads, and
`common::format` for values and addresses. Return `PHAL_STATUS_OK`,
`PHAL_STATUS_TIMEOUT`, `PHAL_STATUS_ERROR`, `PHAL_STATUS_UNIMPLEMENTED`, or
`PHAL_STATUS_INVALID`; statuses may be combined with `|`.

The framework creates the chip PHAL context and a private PCIe PHAL context at
testcase entry, both rooted at `0x20000000`, and deinitializes them on return.
Use `ti.phal_ctx` for ordinary register access and
`HalContext::with_pcie_context()` for PCIe aperture or HQC register operations.

## Run testcases

CLI targets are physical TPUs named `TPU0`, `TPU1`, and so on. `--target`
defaults to `TPU0`; numbering currently follows discovery order. The testcase
name's module prefix routes it to that TPU's instance-0 module object.

```sh
sudo ./build-fpga/isml_diag discover
sudo ./build-fpga/isml_diag pcie_dma_data_transfer
sudo ./build-fpga/isml_diag pcie_bar_read32 --target TPU1 --bar-index 0 --offset 0x20f80
sudo ./build-fpga/isml_diag pcie_sequential_aperture_mapping --log-level regio
```

Multiword options use kebab case. Global options are `--target`, `--log-level`,
and `--policy-dir`. `DeviceManager` currently serializes discovery and testcase
dispatch.

### Policy runs

Each YAML line is one invocation of the local testcase name. Omitted values use
the `_add_test()` defaults; `timeout` is shorthand for `timeout_ms`.

```text
dma_data_transfer:
  direction=h2d, size_bytes=4096, pattern=incremental
  direction=d2h, size_bytes=0x100000, pattern=random, timeout=30000
```

Run every configured line for one testcase with:

```sh
sudo ./build-fpga/isml_diag pcie_dma_data_transfer -all
```

The option is `-all`, not `--all`. It does not run every testcase in the YAML,
and it cannot be combined with testcase arguments. CMake copies the policy tree
beside the executable; use `--policy-dir` only to override it.

## Register and memory access

### Atlas resource inventory

Atlas testcase implementations can query the discovered, read-only resource
snapshot through their owning TPU. The inventory reports presence only; the
testcase owner still selects each instance and its register base.
Known profiles provide the exact discover tree. A structurally valid but
unmatched profile keeps the basic tree while its CXL, ISI, and AIC fields remain
available to testcase APIs.

```cpp
const auto& resource = device_->resource_info();
const std::string& profile = resource.profile_name;
const bool profile_matched = resource.profile_matched;

if (!resource.has_isi(4)) {
    return make_unimplemented_status(ti, "ISI4 is not present");
}

for (uint32_t isi : resource.isi_indices()) {
    // Select the instance-specific base and operation in the testcase.
}

const uint32_t dmc_mask = resource.dmc_mask; // bit = ddp * 3 + dmc
const bool has_ddp2_dmc0 = resource.has_dmc(2, 0);
const uint32_t cxl_mask = resource.cxl_mask; // same bit layout
const bool has_ddp2_dmc0_cxl = resource.has_cxl(2, 0);
const auto dmc_type = has_ddp2_dmc0
    ? resource.get_dmc_type(2, 0) : AtlasDmcType::FakeDmem;
const bool has_tpe2 = resource.has_aic_blocks(ATLAS_AIC_TPE2);
const uint64_t fake_dmem_size_bytes = resource.fake_dmem_size_bytes;
```

Use `has_dmc(ddp, dmc)` before entering a DMC register base.
Use `has_cxl(ddp, dmc)` to identify CXL-backed DMCs.

Register operations use the testcase PHAL context and generated block offsets:

```cpp
auto* phal = ti.phal_ctx;
phal_block_ctx_enter(phal, block_offset);
auto status = phal_read(phal, reg_offset, &actual);
phal_block_ctx_exit(phal, block_offset);
```

Allocate device memory before access. IO offsets are relative to the returned
buffer:

```cpp
auto automatic = ti.hal->device_mem_alloc(DMEM, 1024);
auto fixed = ti.hal->device_mem_alloc(DMEM, 1024, 0x2000);
status = mem_write(fixed, 0, expected);
status |= mem_read(fixed, 0, &actual);
```

- Automatic allocation starts on a 512-byte boundary. A custom power-of-two
  alignment of at least 4 bytes may be requested.
- Explicit offsets are honored exactly. Allocation sizes, explicit offsets,
  and ordinary IO lengths require 4-byte alignment.
- `mem_read64()` and `mem_write64()` support DMEM only and require 8-byte
  alignment.
- Allocations are first-fit and protected per physical TPU. Buffers are
  move-only and release their range automatically.
- Do not release a buffer while hardware may still use it. Retain uncertain
  DMA allocations until reset or device teardown.
- DMEM uses BAR4/aperture0; RCF regions use direct BAR access. AIC
  SMEM/IMEM/VMEM allocation is not implemented.

## Host DMA

Allocate host memory with `ti.hal->host_mem_alloc(size_bytes)`. A testcase that
wants driver selection must register a `drv` argument.

- `drv=isml_drv` is the default. It supports allocations up to 128 MiB and
  requires the matching `/dev/isml_drv*` device.
- `drv=none` supports up to 2 MiB. Every device in the target's IOMMU group
  must already be unbound, and the process needs permission to read physical
  addresses through `/proc/self/pagemap`.

The driverless path switches the IOMMU group to identity mode and does not
restore it. It uses an ordinary page for small requests and a physically
contiguous 2 MiB HugeTLB page for larger requests, growing the HugeTLB pool by
one when possible. It fails rather than expose fragmented memory to DMA.
Host DMA addresses must be 512-byte aligned.

## Useful hardware helpers

- `HqcDmaEngine` and `DmaDescriptor` provide direct HQC DMA submission.
- `HqcCpQueue::copy()` and `PCIeImpl::cp_copy()` provide synchronous CP copy.
- `HalContext::secondary_bus_reset()` sets or clears the upstream bridge SBR
  bit and verifies it by readback; the caller owns delays and recovery.
- `pcie_bar_read32` reads an explicit BAR index and absolute BAR offset.

Current PCIe testcase catalog:

| Testcase | Purpose |
| --- | --- |
| `pcie_dma_data_transfer` | H2D, D2H, H2I, I2H, H2S, S2H, D2I2D, and D2S2D with payload verification |
| `pcie_cp_queue_data_transfer` | Host/DMEM to or from HQC SRAM through COMP0-3 or MEM0-3 |
| `pcie_sequential_aperture_mapping` | Stateful aperture remap and restore check |
| `pcie_bar_read32` | Raw BAR debug read |
| `pcie_dma_h2d_example` | Minimal direct DMA example |
| `pcie_cp_h2sram_example` | Minimal CP copy example |
| `pcie_example` | Register access and disruptive Secondary Bus Reset example |

DMA sizes must be nonzero multiples of 32 bytes. CP sizes must be nonzero
multiples of 128 bytes. CP submissions are currently serialized even when
different queues are selected.

## Build and verify

Production builds support Linux and require the complete ProtonHal/CSR tree at
`isml_ipc/external/`:

```sh
git submodule update --init --recursive
cmake -S . -B build-fpga
cmake --build build-fpga
```

Copy `build-fpga/` and `isml_kmod/` to the FPGA/EMU tester. Build the kernel
module on that tester so it matches the running kernel:

```sh
make -C isml_kmod
sudo insmod ./isml_kmod/build/isml_drv.ko
sudo ./build-fpga/isml_diag discover
```

Loading and binding the driver are environment-specific. Remove a conflicting
proton driver before binding `isml_drv`.

Software contract tests do not require hardware behavior:

```sh
cmake -S . -B build-io-tests -DISML_USE_MOCK_PHAL=ON -DISML_BUILD_TESTS=ON
cmake --build build-io-tests
ctest --test-dir build-io-tests --output-on-failure
```

Mock PHAL validates compilation and software flow only; it does not prove BAR,
aperture, DMA, or register behavior on hardware.

## Current limitations

- Multi-card discovery still assigns `TPU<n>` by discovery order instead of a
  stable physical locator.
- Direct HQC and CP paths do not support concurrent submission and completion
  attribution.
- CP copy currently limits one request to one 512 KiB HQC SRAM bank. TODO:
  expose the four contiguous banks as one 2 MiB CP range and add hardware
  coverage for 512 KiB, cross-bank, 2 MiB, and over-limit boundaries.
- Concurrent aperture-window use is not supported.
- FW-reserved memory ranges and physical DMEM capacity must be confirmed before
  changing allocator regions. IMEM/SMEM DMA tests currently use only `[0, 256)`.
