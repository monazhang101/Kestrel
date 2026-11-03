# ISML Diagnostic Development

Commands below run from the repository root.

## Build

Linux builds require the complete ProtonHal/CSR tree under
`isml_ipc/external/`.

```bash
cmake -S . -B build-fpga
cmake --build build-fpga -j
```

For host DMA through `isml_drv`, build/load the driver on the tester:

```bash
make -C isml_kmod
sudo insmod isml_kmod/build/isml_drv.ko
```

## Write a testcase

1. Declare the method in `isml_diag/include/diag/modules/<Module>.h`.
2. Register its name, arguments and callback in
   `isml_diag/testcases/<module>/<Module>.cpp` using `_add_test()`.
3. Put examples in that module's `example.cpp`; add other source files to
   `CMakeLists.txt` when needed.

Registration defines defaults and argument formats. The module prefix is added
implicitly: `uart_read32_example` in PMU becomes `pmu_uart_read32_example`.

```cpp
_add_test("uart_read32_example", {{"addr", "", "u64"}},
          [this](TestInfo& ti) { return uart_read32_example(ti); },
          {CtxResource::uart()});
```

Use `ti.args` (`common::args::get_u64/get_string`) for arguments and `ti.logger`
(`error/info/flow/regio`) for logs. Return a `PHAL_STATUS_*` value.

- Direct register IO: `phal_read/write(ti.phal_ctx, addr, ...)`.
- UART IO: register `{CtxResource::uart()}` and use `ti.hal->uart_context()`.
- FW IPC IO: register `{CtxResource::ipc(fw)}` and use `ti.hal->ipc_context(fw)`.
- Device memory: allocate with `ti.hal->device_mem_alloc(region, size)`, then
  `mem_read/write(buffer, offset, ...)`.
- Host DMA memory: allocate with `ti.hal->host_mem_alloc(size)`.

Testcase setup configures the default Atlas DMEM aperture: BAR4/aperture 0,
identity 0, target `0x10000000`, size `0x8000000000`. DMEM IO does not
reconfigure it. To change the mapping:

```cpp
aperture_set(*ti.hal, 4, 0, 0x10000000, 0x8000000000);
```

The testcase owns the mapping after reconfiguration.

Include `diag/core/HalContext.h` when accessing HAL resources. Context creation
and cleanup are framework-owned. For UART, the testcase calls PHAL normally:

```cpp
auto* ctx = ti.hal->uart_context();
uint32_t value = 0;
auto status = phal_read(ctx, address, &value);
if (status != PHAL_STATUS_OK) return status;
ti.logger->info("value=" + common::format::hex(value, 8));
return phal_write(ctx, address, expected);
```

## Run testcases

```bash
./build-fpga/isml_diag discover
./build-fpga/isml_diag ddp_example --target TPU0 --log-level regio
./build-fpga/isml_diag pcie_dma_h2d_example --target TPU0
./build-fpga/isml_diag pcie_bar_read32 --target TPU0 --bar-index 0 --offset 0x20f80
```

`--target` defaults to `TPU0`. Use `--log-level error|info|flow|regio` and
`--policy-dir <path>` as needed. Multiword CLI flags use kebab case;
`--size-bytes` becomes `ti.args["size_bytes"]`.

For ordered parameter runs, add entries under the local testcase name in
`isml_diag/policies/testcases/<module>.yaml`, then run
`isml_diag <module_test_name> -all`. CMake copies policies beside the binaries.

## Run PMU UART examples (FPGA only)

Use two shell terminals on the same tester with one UART endpoint.
Terminal A: open the tester's UART endpoint and keep the console running:

```bash
./build-fpga/isml_uart_console --target TPU0 --port /dev/ttyUSB0 \
  --baud 115200 --retry-count 3 --retry-interval-ms 100 --response-timeout-ms 5000
```

Terminal B: run a testcase, which connects to the console over a local Unix
socket (use the same target as terminal A):

```bash
./build-fpga/isml_diag pmu_uart_read32_example --target TPU0 --addr 0x02105060
./build-fpga/isml_diag pmu_uart_write32_example --target TPU0 --addr 0x02105060 --value 0x12345678
```

Run both terminals as the same user with the same `XDG_RUNTIME_DIR` setting.
Only the console opens the port and prints PMU logs. Testcases receive command
results and print their own logs. `retry-count=N` allows N retries after the
first send; all retry/timeout settings belong to the console CLI.
LOG/RSP from other sessions are discarded and counted as `stale_session_frames`,
following the Python reference. Current-session LOG text is printed even while
waiting for a response; unlike Python's `wait_rsp()`, it is not skipped.

`--addr` is a 4-byte-aligned absolute byte address in the PMU firmware's address
space. The UART PHAL context has base 0 and forwards the address unchanged;
it adds no module base or host BAR translation.

`0x36100000` is the tester's host BAR0 offset for PCIe DID/VID, not a verified
PMU UART address. Its chip-internal address is `0x16100000`; UART access still
requires firmware address-map and whitelist support. The Python reference
describes a PMU_DIAG-only read/write whitelist and uses `0x02105060` (REG0,
boot counter). Confirm current firmware support before using other registers.
