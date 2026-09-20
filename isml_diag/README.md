# ISML Diagnostic Development

`isml_diag` separates target construction, testcase implementation, and
platform configuration:

```text
include/diag/modules/        Module interfaces and testcase declarations
src/modules/                 Module construction and testcase registration
testcases/<target_type>/     Testcase implementations
policies/testcases/          Optional testcase argument ranges
policies/product/            Product topology and module instances
cli/                         Command-line entry point
bindings/                    Python bindings
```

## Add a testcase

1. Declare the testcase method in `include/diag/modules/<Module>.h`.
2. Register its public name, defaults, formats, and callback with `_add_test()`
   in `src/modules/<target_type>/<Module>.cpp`.
3. Implement the method in `testcases/<target_type>/*_cases.cpp`.
4. Add optional platform limits under the same testcase name in
   `policies/testcases/<target_type>.yaml`.
5. Add a new testcase source file to the root `CMakeLists.txt` when needed.

Defaults and argument formats belong in `_add_test()`. YAML policy files only
restrict values allowed on a platform; they do not redefine testcase behavior.
For a testcase that uses the short DMEM API or calls PHAL directly, pass `true`
as the final `_add_test(..., callback, true)` argument. The runner then prepares
both the module and PCIe PHAL contexts before invoking the callback. Register
IO itself does not require PHAL.

## Add a module type

1. Add the module interface under `include/diag/modules/` and its constructor
   under `src/modules/<target_type>/`.
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

PMU, DDP and ISI each register one `example` testcase in their own
`testcases/<module>/example_case.cpp`. They call the short functions declared
by `Common.h` directly:

```cpp
auto& ctx = ctx_;  // This target's existing DeviceContext; prepared by the runner.
auto status = block_ctx_enter(ctx, block_offset);
if (status != TestStatus::OK) return status;
status |= reg_write(ctx, reg_offset, value);
status |= reg_read(ctx, reg_offset, &actual);
status |= block_ctx_exit(ctx, block_offset);
status |= dmem_write(ctx, dmem_offset, value);
status |= dmem_read(ctx, dmem_offset, &actual);
```

Each access moves one 32-bit word; offsets are byte offsets. A successful read
assigns its output pointer; failed reads leave it unchanged. Register addresses
are `module base + block offset + reg_offset` in the module's BAR. `reg_read`
and `reg_write` call `common::bar` directly, without going through PHAL. The
log includes the target, module base, block offset, full BAR offset and value.
Enter/exit must be paired; nested blocks add their offsets. The runner resets
block state when a testcase returns or throws.

DMEM uses a separate scratch base, currently `0x10000000`, and a 1 MiB window
on BAR4/aperture0/identity0. Its PCIe PHAL context is initialized at the PCIe
root, independently of the current module. Every operation sets and verifies
the aperture before accessing the data window. Framework register blocks do
not modify PHAL's `env.base`, so DMEM works inside a framework register block
too. These bring-up scratch settings still need confirmation on the hardware.

Examples default to `write_enable=0`: they read the chosen register and DMEM
word. Set `--write-enable 1` only for a confirmed ordinary read/write scratch
register and memory range; the example then saves, writes, compares and restores
both words. This restore pattern is not valid for W1C/read-clear/command
registers. `--value` is a 32-bit pattern; `--block-offset`, `--reg-offset` and
`--dmem-offset` choose the locations. Defaults are declared only in `_add_test`.

```sh
./build/isml_diag example --target DDP_0_0 --reg-offset 0
./build/isml_diag example --target DDP_0_1 --reg-offset 0
./build/isml_diag example --target PMU_0_0 --reg-offset 0
./build/isml_diag example --target ISI_0_0 --reg-offset 0 --timeout-us 1000000
```

The ISI example additionally calls the actual PHAL entry
`phal_component_isi_linkup(ctx, timeout_us)` with its preinitialized ISI context,
even when register/DMEM writes are disabled. PMU and DDP have no direct PHAL
example section. Only PHAL read/write callbacks are installed; no delay, tick
or fence callbacks are added. Real project-specific linkup behavior requires
the real ProtonHal tree; the local mock deliberately reports failure.

`TestStatus` supports `|` and `|=` and prints combined flags. Native PHAL status
is converted explicitly: known OK/INVALID map to framework OK/INVALID, and
other native failures map to ERROR with the original value logged. Do not OR
raw bool or PHAL status values into `TestStatus`.

TPU remains the parent device without an `identify` testcase. DMC is no longer
a module or target. Existing PCIe BAR/aperture/DMA cases, HQC code, and the
kernel module are retained. Their raw BAR offsets and configurable buffer
`common::devmem::read/write` interfaces are unchanged. Run Python testcases
through `DeviceManager.run_testcase` so HAL preparation and policy are applied.
Same-device module execution is serialized; discovery cannot race manager-run
tests. Device contexts and PHAL pointers must not outlive their owning manager.

## Software verification

On Linux, configure the normal build with `-DISML_BUILD_TESTS=ON`, build, and
run `ctest --test-dir build --output-on-failure`.

For portable software checks (including Windows/MinGW), build the standalone
test project. It compiles the production dispatch, cases, PCIe/HQC code and
PHAL bridge against the mock, replacing only the Linux mapping/DMA owner with
a test fixture:

```sh
cmake -S tests -B build-io-tests -G Ninja
cmake --build build-io-tests
ctest --test-dir build-io-tests --output-on-failure
```

These tests check address dispatch, bounds, read-only mapping rejection,
context cleanup, serialization, new/legacy DMEM APIs, native PHAL callback
routing and the retained PCIe catalog. They do not verify physical aperture
translation, ISI linkup, Linux ioctl/mmap or actual DMA.
