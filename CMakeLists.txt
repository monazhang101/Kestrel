cmake_minimum_required(VERSION 3.16)

project(KestrelDiag LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KESTREL_BUILD_EXAMPLES "Build Kestrel diagnostic examples" ON)
option(KESTREL_BUILD_PYTHON_BINDINGS "Build pybind11 Python bindings" OFF)

set(ISML_DIAG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isml_diag)
set(ISML_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isml_lib)
set(ISML_IMPLEMENTER_DIR ${ISML_LIB_DIR}/implementer)

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
    isml_diag/src/core/PolicyParser.cpp
    isml_diag/src/device/DeviceManager.cpp
    isml_diag/src/device/TPUDevice.cpp
    isml_diag/src/core/HalBackend.cpp
    isml_diag/src/modules/ddp/DDPModule.cpp
    isml_diag/src/modules/ddp/ddp_dmem_cases.cpp
    isml_diag/src/modules/dmc/DMCModule.cpp
    isml_diag/src/modules/dmc/dmc_status_cases.cpp
    isml_diag/src/modules/dmc/dmc_reg_cases.cpp
    isml_diag/src/modules/isi/ISIModule.cpp
    isml_diag/src/modules/isi/isi_link_cases.cpp
    isml_diag/src/modules/isi/isi_setup_cases.cpp
    isml_diag/src/modules/pcie/PCIeModule.cpp
    isml_diag/src/modules/pcie/pcie_link_cases.cpp
    isml_diag/src/modules/pcie/pcie_dma_cases.cpp
    isml_diag/src/modules/pcie/pcie_reg_cases.cpp
    isml_diag/src/modules/pmu/PMUModule.cpp
    isml_diag/src/modules/pmu/pmu_ipc_cases.cpp
    isml_diag/src/modules/pmu/pmu_reg_cases.cpp
)

target_include_directories(isml_diag
    PUBLIC
        ${ISML_DIAG_DIR}/include
        ${ISML_IMPLEMENTER_DIR}/include
)

target_link_libraries(isml_diag
    PUBLIC
        isml_lib
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
