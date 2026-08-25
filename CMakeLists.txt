cmake_minimum_required(VERSION 3.16)

project(KestrelDiag LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KESTREL_BUILD_EXAMPLES "Build Kestrel diagnostic examples" ON)
option(KESTREL_BUILD_PYTHON_BINDINGS "Build pybind11 Python bindings" OFF)

add_library(kestrel_diag
    src/core/CmdQueueMgm.cpp
    src/core/PolicyParser.cpp
    src/device/DeviceManager.cpp
    src/device/TPUDevice.cpp
    src/core/HalBackend.cpp
    src/implementer/implementer.cpp
    src/implementer/generic/generic_impl.cpp
    src/implementer/atlas/ddp/ddp_impl.cpp
    src/implementer/atlas/dmc/dmc_impl.cpp
    src/implementer/atlas/isi/isi_impl.cpp
    src/implementer/atlas/pcie/pcie_impl.cpp
    src/implementer/atlas/pmu/pmu_impl.cpp
    src/implementer/atlas/tpu/tpu_impl.cpp
    src/implementer/atlas_m/ddp/ddp_impl.cpp
    src/implementer/atlas_m/dmc/dmc_impl.cpp
    src/implementer/atlas_m/isi/isi_impl.cpp
    src/implementer/atlas_m/pcie/pcie_impl.cpp
    src/implementer/atlas_m/pmu/pmu_impl.cpp
    src/implementer/atlas_m/tpu/tpu_impl.cpp
    src/modules/ddp/DDPModule.cpp
    src/modules/ddp/ddp_dmem_cases.cpp
    src/modules/dmc/DMCModule.cpp
    src/modules/dmc/dmc_status_cases.cpp
    src/modules/dmc/dmc_reg_cases.cpp
    src/modules/isi/ISIModule.cpp
    src/modules/isi/isi_link_cases.cpp
    src/modules/isi/isi_setup_cases.cpp
    src/modules/pcie/PCIeModule.cpp
    src/modules/pcie/pcie_link_cases.cpp
    src/modules/pcie/pcie_dma_cases.cpp
    src/modules/pcie/pcie_reg_cases.cpp
    src/modules/pmu/PMUModule.cpp
    src/modules/pmu/pmu_ipc_cases.cpp
    src/modules/pmu/pmu_reg_cases.cpp
)

target_include_directories(kestrel_diag
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/implementer
)

if(KESTREL_BUILD_EXAMPLES)
    add_executable(kestrel_diag_example
        examples/main.cpp
    )

    target_link_libraries(kestrel_diag_example
        PRIVATE
            kestrel_diag
    )
endif()

if(KESTREL_BUILD_PYTHON_BINDINGS)
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(tpu_hal
        bindings/PyBinding.cpp
    )

    target_link_libraries(tpu_hal
        PRIVATE
            kestrel_diag
    )
endif()
