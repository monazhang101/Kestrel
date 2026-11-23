cmake_minimum_required(VERSION 3.16)

project(SpevDiag LANGUAGES C CXX)

find_package(Threads REQUIRED)

# The production runtime depends on Linux PCI sysfs, mmap/ioctl, and the
# spev_diag kernel UAPI. Unsupported hosts should fail at configure time
# instead of compiling a HAL implementation that can only fail at runtime.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "SpevDiag supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPEV_BUILD_DIAG "Build the PCIe diagnostic library and tool" ON)
option(SPEV_BUILD_UART "Build the standalone UART tool" ON)
option(SPEV_BUILD_CLI "Build the SPEV diagnostic CLI" ON)
option(SPEV_BUILD_PYTHON_BINDINGS "Build pybind11 Python bindings" OFF)
option(SPEV_USE_MOCK_PHAL "Use the bundled compile/smoke PHAL mock" OFF)
option(SPEV_BUILD_TESTS "Build software IO contract tests (no hardware access)" OFF)

if(SPEV_BUILD_UART AND NOT SPEV_BUILD_DIAG)
    message(FATAL_ERROR
        "SPEV_BUILD_UART uses the shared module/testcase runtime and requires "
        "SPEV_BUILD_DIAG=ON")
endif()

set(SPEV_DIAG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/spev_diag)
set(SPEV_ADAPTER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/spev_adapter)
set(SPEV_IMPLEMENTER_DIR ${SPEV_ADAPTER_DIR}/implementer)
include(${CMAKE_CURRENT_SOURCE_DIR}/Uart.cmake)
set(SPEV_POLICY_SOURCE_DIR ${SPEV_DIAG_DIR}/policies)
set(SPEV_POLICY_BINARY_DIR ${CMAKE_BINARY_DIR}/policies)
set(SPEV_CICD_SOURCE_DIR ${SPEV_DIAG_DIR}/cicd)
set(SPEV_CICD_BINARY_DIR ${CMAKE_BINARY_DIR}/cicd)
set(SPEV_CICD_RUNTIME_FILES
    ${SPEV_CICD_SOURCE_DIR}/__init__.py
    ${SPEV_CICD_SOURCE_DIR}/requirements.txt
    ${SPEV_CICD_SOURCE_DIR}/run_regression.py)

# Keep a deployable policy copy next to the artifacts in the directory chosen
# by cmake -B. Configure creates it immediately; every build resynchronizes it.
file(REMOVE_RECURSE "${SPEV_POLICY_BINARY_DIR}")
file(COPY "${SPEV_POLICY_SOURCE_DIR}" DESTINATION "${CMAKE_BINARY_DIR}")
add_custom_target(spev_diag_policies ALL
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${SPEV_POLICY_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SPEV_POLICY_SOURCE_DIR}" "${SPEV_POLICY_BINARY_DIR}"
    COMMENT "Syncing SPEV diagnostic policies")

# Keep the Jenkins-facing regression runner beside the build artifacts. Copy
# only the declared runtime files so local Python bytecode is never deployed.
file(REMOVE_RECURSE "${SPEV_CICD_BINARY_DIR}")
file(MAKE_DIRECTORY "${SPEV_CICD_BINARY_DIR}")
file(COPY ${SPEV_CICD_RUNTIME_FILES} DESTINATION "${SPEV_CICD_BINARY_DIR}")
add_custom_target(spev_diag_cicd ALL
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${SPEV_CICD_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SPEV_CICD_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SPEV_CICD_RUNTIME_FILES} "${SPEV_CICD_BINARY_DIR}"
    COMMENT "Syncing SPEV diagnostic CI/CD runner")

# ProtonHal and the generated Atlas CSR headers are supplied by the external
# dependency tree. The legacy smoke option is retained for compatibility, but
# it now resolves to the same external ProtonHal tree.
set(PHAL_REAL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/spev_ipc/external/ProtonHal)
# The external tree is the only ProtonHal source now.  Keep the legacy option
# usable for local smoke builds, but point it at the same dependency tree.
set(PHAL_MOCK_ROOT ${PHAL_REAL_ROOT})

if(SPEV_USE_MOCK_PHAL)
    set(PHAL_ROOT ${PHAL_MOCK_ROOT})
    set(SPEV_USING_MOCK_PHAL ON)
    set(PHAL_CORE_SOURCES
        ${PHAL_ROOT}/src/phal/init.c
        ${PHAL_ROOT}/src/phal/ops.c
        ${PHAL_ROOT}/src/components/pcie/pcie.c
        ${PHAL_ROOT}/src/components/isi/isi.c
    )
    set(PHAL_PROJECT_IMPL_SOURCES)
    message(WARNING
        "PHAL implementation: external ProtonHal in smoke mode (${PHAL_ROOT})")
elseif(NOT EXISTS "${PHAL_REAL_ROOT}/src/phal/init.c" OR
       NOT EXISTS "${PHAL_REAL_ROOT}/include/phal/components/pcie/pcie.h")
    message(FATAL_ERROR
        "Real ProtonHal is missing or incomplete at ${PHAL_REAL_ROOT}. "
        "Hardware builds must provide the complete ProtonHal tree. "
        "For local compile/smoke testing only, explicitly set "
        "-DSPEV_USE_MOCK_PHAL=ON.")
else()
    set(PHAL_ROOT ${PHAL_REAL_ROOT})
    set(SPEV_USING_MOCK_PHAL OFF)

    if(NOT DEFINED FLOWRA_SOURCES_DIR)
        set(FLOWRA_SOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/spev_ipc/external)
    endif()
    message(STATUS "PHAL flowra sources dir: ${FLOWRA_SOURCES_DIR}")

    set(PHAL_CSR_INCLUDE_DIRS
        ${FLOWRA_SOURCES_DIR}
        ${FLOWRA_SOURCES_DIR}/atlas_csr_sw
        ${FLOWRA_SOURCES_DIR}/atlas_csr_de
    )

    set(PHAL_CORE_SOURCES
        ${PHAL_ROOT}/src/phal/init.c
        ${PHAL_ROOT}/src/phal/ops.c
        ${PHAL_ROOT}/src/components/pcie/pcie.c
        ${PHAL_ROOT}/src/components/pcie/generic_impl.c
        ${PHAL_ROOT}/src/components/xdma/xdma.c
        ${PHAL_ROOT}/src/components/xdma/generic_impl.c
        ${PHAL_ROOT}/src/components/isi/isi.c
        ${PHAL_ROOT}/src/components/isi/generic_impl.c
        ${PHAL_ROOT}/src/components/syncflag/syncflag.c
        ${PHAL_ROOT}/src/components/syncflag/generic_impl.c
        ${PHAL_ROOT}/src/components/aic/aic.c
        ${PHAL_ROOT}/src/components/aic/generic_impl.c
        ${PHAL_ROOT}/src/components/atbtu/atbtu.c
        ${PHAL_ROOT}/src/components/atbtu/generic_impl.c
        ${PHAL_ROOT}/src/components/hqc/hqc.c
        ${PHAL_ROOT}/src/components/hqc/generic_impl.c
    )

    set(PHAL_PROJECT_IMPL_SOURCES
        ${PHAL_ROOT}/projects/atlas/pcie/pcie_impl.c
        ${PHAL_ROOT}/projects/atlas/isi/isi_impl.c
        ${PHAL_ROOT}/projects/atlas/xdma/xdma_impl.c
        ${PHAL_ROOT}/projects/atlas/syncflag/syncflag_impl.c
        ${PHAL_ROOT}/projects/atlas/atbtu/atbtu_impl.c
        ${PHAL_ROOT}/projects/atlas/hqc/hqc_impl.c
        ${PHAL_ROOT}/projects/atlas/aic/aic_impl.c
        ${PHAL_ROOT}/projects/atlas_m/pcie/pcie_impl.c
        ${PHAL_ROOT}/projects/atlas_m/isi/isi_impl.c
        ${PHAL_ROOT}/projects/atlas_m/xdma/xdma_impl.c
        ${PHAL_ROOT}/projects/atlas_m/syncflag/syncflag_impl.c
    )
    message(STATUS "PHAL implementation: real ProtonHal (${PHAL_ROOT})")
endif()

add_library(phal STATIC ${PHAL_CORE_SOURCES} ${PHAL_PROJECT_IMPL_SOURCES})

target_include_directories(phal
    PUBLIC
        ${PHAL_ROOT}
        ${PHAL_ROOT}/include
)

if(SPEV_USING_MOCK_PHAL)
    target_include_directories(phal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/spev_ipc/external)
endif()

if(NOT SPEV_USING_MOCK_PHAL)
    target_include_directories(phal PRIVATE ${PHAL_CSR_INCLUDE_DIRS})
endif()

target_compile_definitions(phal
    PUBLIC
        HAL_DV_MODE=1
        PHAL_TARGET_ENV_DV=1
)

set_target_properties(phal PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

add_library(ProtonHal::phal ALIAS phal)

add_library(spev_test_core STATIC
    spev_diag/src/core/TestTarget.cpp
    spev_diag/src/core/PolicyParser.cpp
    spev_diag/src/core/PhalContext.cpp)
target_include_directories(spev_test_core PUBLIC ${SPEV_DIAG_DIR}/include)
target_link_libraries(spev_test_core PUBLIC phal)

add_library(spev_uart_runtime STATIC spev_diag/src/core/UartPhalBridge.cpp)
target_link_libraries(spev_uart_runtime PUBLIC spev_test_core spev_uart_transport)
set_target_properties(spev_test_core spev_uart_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

# GCC 8 and earlier ship std::filesystem in a separate compatibility library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(spev_test_core PUBLIC stdc++fs)
    target_link_libraries(spev_uart_transport PUBLIC stdc++fs)
endif()


if(SPEV_BUILD_DIAG)
add_library(spev_adapter
    spev_adapter/implementer/src/implementer.cpp
    spev_adapter/implementer/src/generic/generic_impl.cpp
    spev_adapter/implementer/src/atlas/memory_regions.cpp
    spev_adapter/implementer/src/atlas/resource.cpp
    spev_adapter/implementer/src/atlas/pcie/hqc_dma.cpp
    spev_adapter/implementer/src/atlas/pcie/hqc_cp_queue.cpp
    spev_adapter/implementer/src/atlas/pcie/pcie_impl.cpp
    spev_adapter/implementer/src/atlas_m/pcie/pcie_impl.cpp
)

target_include_directories(spev_adapter
    PUBLIC
        ${SPEV_IMPLEMENTER_DIR}/include
        ${SPEV_DIAG_DIR}/include
    PRIVATE
        ${SPEV_IMPLEMENTER_DIR}/src
)

# AtlasResource.h is a public header that includes generated CSR headers, so
# the CSR include paths must propagate to spev_adapter's dependents.
if(NOT SPEV_USING_MOCK_PHAL)
    target_include_directories(spev_adapter PUBLIC ${PHAL_CSR_INCLUDE_DIRS})
endif()

target_link_libraries(spev_adapter PUBLIC phal)

add_library(spev_diag
    spev_diag/src/core/TestTargetPcie.cpp
    spev_diag/src/core/DevMem.cpp
    spev_diag/src/core/PhalBridge.cpp
    spev_diag/src/core/IpcBridge.cpp
    spev_diag/src/core/HalContext.cpp
    spev_diag/src/core/HalContextDma.cpp
    spev_diag/src/device/DeviceManager.cpp
    spev_diag/src/device/TPUDevice.cpp
)

add_subdirectory(spev_diag/testcases/ddp)
add_subdirectory(spev_diag/testcases/isi)
add_subdirectory(spev_diag/testcases/pcie)
add_subdirectory(spev_diag/testcases/pmu)
add_subdirectory(spev_diag/testcases/soc)

target_include_directories(spev_diag
    PUBLIC
        ${SPEV_DIAG_DIR}/include
        ${SPEV_IMPLEMENTER_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/spev_kmod
)
if(NOT SPEV_USING_MOCK_PHAL)
    target_include_directories(spev_diag PRIVATE ${PHAL_CSR_INCLUDE_DIRS})
endif()

target_link_libraries(spev_diag PUBLIC spev_adapter spev_test_core spev_uart_runtime Threads::Threads)


set_target_properties(spev_diag PROPERTIES OUTPUT_NAME spev_diag)
add_dependencies(spev_diag spev_diag_policies spev_diag_cicd)

if(SPEV_BUILD_CLI)
    add_executable(spev_diag_cli spev_diag/cli/main.cpp)
    target_link_libraries(spev_diag_cli PRIVATE spev_diag)
    set_target_properties(spev_diag_cli PROPERTIES OUTPUT_NAME spev_diag)
endif()

if(SPEV_BUILD_UART AND SPEV_BUILD_CLI)
    # Both executables use the same module constructors and testcase catalog.
    # Only the entrypoint selects the active PCIe or UART backend.
    add_executable(spev_uart spev_diag/cli/uart_main.cpp)
    target_include_directories(spev_uart PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/spev_ipc/external)
    if(NOT SPEV_USING_MOCK_PHAL)
        target_include_directories(spev_uart PRIVATE ${PHAL_CSR_INCLUDE_DIRS})
    endif()
    target_link_libraries(spev_uart PRIVATE spev_diag)
    add_dependencies(spev_uart spev_diag_policies spev_diag_cicd)
endif()

if(SPEV_BUILD_PYTHON_BINDINGS)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(tpu_hal spev_diag/bindings/PyBinding.cpp)
    target_link_libraries(tpu_hal PRIVATE spev_diag)
endif()

endif() # SPEV_BUILD_DIAG

if(SPEV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
