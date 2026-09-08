cmake_minimum_required(VERSION 3.16)

project(KestrelDiag LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KESTREL_BUILD_EXAMPLES "Build Kestrel diagnostic examples" ON)
option(KESTREL_BUILD_PYTHON_BINDINGS "Build pybind11 Python bindings" OFF)
option(KESTREL_USE_MOCK_PHAL "Use the bundled compile/smoke PHAL mock" OFF)

set(ISML_DIAG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isml_diag)
set(ISML_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isml_lib)
set(ISML_IMPLEMENTER_DIR ${ISML_LIB_DIR}/implementer)

# Keep the same PHAL target contract in real and local-mock builds. Tester
# builds use the in-tree ProtonHal submodule; source-only workspaces fall back
# to a small API-compatible mock outside isml_diag and isml_lib.
set(PHAL_REAL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/isml_ipc/external/ProtonHal)
set(PHAL_MOCK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/mock/ProtonHal)

if(KESTREL_USE_MOCK_PHAL OR
   NOT EXISTS "${PHAL_REAL_ROOT}/src/phal/init.c" OR
   NOT EXISTS "${PHAL_REAL_ROOT}/include/phal/components/pcie/pcie.h")
    set(PHAL_ROOT ${PHAL_MOCK_ROOT})
    set(KESTREL_USING_MOCK_PHAL ON)
    set(PHAL_CORE_SOURCES
        ${PHAL_ROOT}/src/phal/init.c
        ${PHAL_ROOT}/src/components/pcie/pcie.c
    )
    set(PHAL_PROJECT_IMPL_SOURCES)
    message(STATUS "PHAL backend: bundled mock (${PHAL_ROOT})")
else()
    set(PHAL_ROOT ${PHAL_REAL_ROOT})
    set(KESTREL_USING_MOCK_PHAL OFF)

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

if(NOT KESTREL_USING_MOCK_PHAL)
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
    isml_lib/implementer/src/atlas/ddp/ddp_impl.cpp
    isml_lib/implementer/src/atlas/dmc/dmc_impl.cpp
    isml_lib/implementer/src/atlas/isi/isi_impl.cpp
    isml_lib/implementer/src/atlas/pcie/pcie_impl.cpp
    isml_lib/implementer/src/atlas/pmu/pmu_impl.cpp
    isml_lib/implementer/src/atlas/tpu/tpu_impl.cpp
    isml_lib/implementer/src/atlas_m/ddp/ddp_impl.cpp
    isml_lib/implementer/src/atlas_m/dmc/dmc_impl.cpp
    isml_lib/implementer/src/atlas_m/isi/isi_impl.cpp
    isml_lib/implementer/src/atlas_m/pcie/pcie_impl.cpp
    isml_lib/implementer/src/atlas_m/pmu/pmu_impl.cpp
    isml_lib/implementer/src/atlas_m/tpu/tpu_impl.cpp
)

target_include_directories(isml_lib
    PUBLIC
        ${ISML_IMPLEMENTER_DIR}/include
        ${ISML_DIAG_DIR}/include
    PRIVATE
        ${ISML_IMPLEMENTER_DIR}/src
)

add_library(isml_diag
    isml_diag/src/core/CmdQueueMgm.cpp
    isml_diag/src/core/DevMem.cpp
    isml_diag/src/core/PolicyParser.cpp
    isml_diag/src/core/PhalBridge.cpp
    isml_diag/src/device/DeviceManager.cpp
    isml_diag/src/device/TPUDevice.cpp
    isml_diag/src/core/HalContext.cpp
    isml_diag/src/modules/ddp/DDPModule.cpp
    isml_diag/src/modules/ddp/dmem_cases.cpp
    isml_diag/src/modules/dmc/DMCModule.cpp
    isml_diag/src/modules/dmc/status_cases.cpp
    isml_diag/src/modules/dmc/reg_cases.cpp
    isml_diag/src/modules/isi/ISIModule.cpp
    isml_diag/src/modules/isi/link_cases.cpp
    isml_diag/src/modules/isi/pcie_aperture_cases.cpp
    isml_diag/src/modules/isi/setup_cases.cpp
    isml_diag/src/modules/pcie/PCIeModule.cpp
    isml_diag/src/modules/pcie/dma_cases.cpp
    isml_diag/src/modules/pcie/aperture_mapping_cases.cpp
    isml_diag/src/modules/pmu/PMUModule.cpp
    isml_diag/src/modules/pmu/ipc_cases.cpp
    isml_diag/src/modules/pmu/reg_cases.cpp
)

target_include_directories(isml_diag
    PUBLIC
        ${ISML_DIAG_DIR}/include
        ${ISML_IMPLEMENTER_DIR}/include
)

target_link_libraries(isml_diag
    PUBLIC
        isml_lib
        phal
)

set_target_properties(isml_diag PROPERTIES
    OUTPUT_NAME kestrel_diag
)

add_library(kestrel_diag ALIAS isml_diag)

if(KESTREL_BUILD_EXAMPLES)
    add_executable(kestrel_diag_example
        isml_diag/examples/main.cpp
    )

    target_link_libraries(kestrel_diag_example
        PRIVATE
            isml_diag
    )
endif()

if(KESTREL_BUILD_PYTHON_BINDINGS)
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(tpu_hal
        isml_diag/bindings/PyBinding.cpp
    )

    target_link_libraries(tpu_hal
        PRIVATE
            isml_diag
    )
endif()
