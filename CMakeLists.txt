cmake_minimum_required(VERSION 3.16)

project(IsmlDiag LANGUAGES C CXX)

# The production runtime depends on Linux PCI sysfs, mmap/ioctl, and the
# isml_diag kernel UAPI. Unsupported hosts should fail at configure time
# instead of compiling an iHal backend that can only fail at runtime.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "IsmlDiag supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ISML_BUILD_CLI "Build the ISML diagnostic CLI" ON)
option(ISML_BUILD_PYTHON_BINDINGS "Build pybind11 Python bindings" OFF)
option(ISML_USE_MOCK_PHAL "Use the bundled compile/smoke PHAL mock" OFF)
option(ISML_BUILD_TESTS "Build software IO contract tests (no hardware access)" OFF)

set(ISML_DIAG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isml_diag)
set(ISML_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isml_lib)
set(ISML_IMPLEMENTER_DIR ${ISML_LIB_DIR}/implementer)

# Keep the same PHAL target contract in real and local-mock builds. Hardware
# builds require the in-tree ProtonHal; the small API-compatible mock must be
# explicitly enabled and is only for local compile/smoke testing.
set(PHAL_REAL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/isml_ipc/external/ProtonHal)
set(PHAL_MOCK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/mock/ProtonHal)

if(ISML_USE_MOCK_PHAL)
    set(PHAL_ROOT ${PHAL_MOCK_ROOT})
    set(ISML_USING_MOCK_PHAL ON)
    set(PHAL_CORE_SOURCES
        ${PHAL_ROOT}/src/phal/init.c
        ${PHAL_ROOT}/src/phal/ops.c
        ${PHAL_ROOT}/src/components/pcie/pcie.c
        ${PHAL_ROOT}/src/components/isi/isi.c
    )
    set(PHAL_PROJECT_IMPL_SOURCES)
    message(WARNING
        "PHAL backend: bundled mock (${PHAL_ROOT}); "
        "compile/smoke testing only, hardware validation is unsupported")
elseif(NOT EXISTS "${PHAL_REAL_ROOT}/src/phal/init.c" OR
       NOT EXISTS "${PHAL_REAL_ROOT}/include/phal/components/pcie/pcie.h")
    message(FATAL_ERROR
        "Real ProtonHal is missing or incomplete at ${PHAL_REAL_ROOT}. "
        "Hardware builds must provide the complete ProtonHal tree. "
        "For local compile/smoke testing only, explicitly set "
        "-DISML_USE_MOCK_PHAL=ON.")
else()
    set(PHAL_ROOT ${PHAL_REAL_ROOT})
    set(ISML_USING_MOCK_PHAL OFF)

    if(NOT DEFINED FLOWRA_SOURCES_DIR)
        set(FLOWRA_SOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.flowra_sources)
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
        ${PHAL_ROOT}/projects/atlas_m/pcie/pcie_impl.c
        ${PHAL_ROOT}/projects/atlas_m/isi/isi_impl.c
        ${PHAL_ROOT}/projects/atlas_m/xdma/xdma_impl.c
        ${PHAL_ROOT}/projects/atlas_m/syncflag/syncflag_impl.c
    )
    message(STATUS "PHAL backend: real ProtonHal (${PHAL_ROOT})")
endif()

add_library(phal STATIC ${PHAL_CORE_SOURCES} ${PHAL_PROJECT_IMPL_SOURCES})

target_include_directories(phal
    PUBLIC
        ${PHAL_ROOT}
        ${PHAL_ROOT}/include
)

if(NOT ISML_USING_MOCK_PHAL)
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

add_library(isml_lib
    isml_lib/implementer/src/implementer.cpp
    isml_lib/implementer/src/generic/generic_impl.cpp
    isml_lib/implementer/src/atlas/pcie/hqc_admin_queue.cpp
    isml_lib/implementer/src/atlas/pcie/pcie_impl.cpp
    isml_lib/implementer/src/atlas_m/pcie/pcie_impl.cpp
)

target_include_directories(isml_lib
    PUBLIC
        ${ISML_IMPLEMENTER_DIR}/include
        ${ISML_DIAG_DIR}/include
    PRIVATE
        ${ISML_IMPLEMENTER_DIR}/src
)

target_link_libraries(isml_lib PUBLIC phal)

add_library(isml_diag
    isml_diag/src/core/TestTarget.cpp
    isml_diag/src/core/DevMem.cpp
    isml_diag/src/core/PolicyParser.cpp
    isml_diag/src/core/PhalBridge.cpp
    isml_diag/src/core/HalContext.cpp
    isml_diag/src/device/DeviceManager.cpp
    isml_diag/src/device/TPUDevice.cpp
    isml_diag/src/modules/ddp/DDPModule.cpp
    isml_diag/src/modules/isi/ISIModule.cpp
    isml_diag/src/modules/pcie/PCIeModule.cpp
    isml_diag/src/modules/pmu/PMUModule.cpp
    isml_diag/testcases/pmu/example.cpp
    isml_diag/testcases/ddp/example.cpp
    isml_diag/testcases/isi/example.cpp
    isml_diag/testcases/pcie/example.cpp
    isml_diag/testcases/pcie/dma_copy.cpp
    isml_diag/testcases/pcie/aperture_mapping.cpp
)

target_include_directories(isml_diag
    PUBLIC
        ${ISML_DIAG_DIR}/include
        ${ISML_IMPLEMENTER_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/isml_kmod/include/uapi
)

target_link_libraries(isml_diag PUBLIC isml_lib phal)

# GCC 8 and earlier ship std::filesystem in a separate compatibility library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(isml_diag PUBLIC stdc++fs)
endif()

set_target_properties(isml_diag PROPERTIES OUTPUT_NAME isml_diag)

if(ISML_BUILD_CLI)
    add_executable(isml_diag_cli isml_diag/cli/main.cpp)
    target_link_libraries(isml_diag_cli PRIVATE isml_diag)
    set_target_properties(isml_diag_cli PROPERTIES OUTPUT_NAME isml_diag)
endif()

if(ISML_BUILD_PYTHON_BINDINGS)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(tpu_hal isml_diag/bindings/PyBinding.cpp)
    target_link_libraries(tpu_hal PRIVATE isml_diag)
endif()

if(ISML_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
