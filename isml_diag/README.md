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
