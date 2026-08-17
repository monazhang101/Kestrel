cmake_minimum_required(VERSION 3.16)

project(KestrelDiag LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KESTREL_BUILD_EXAMPLES "Build Kestrel diagnostic examples" ON)
option(KESTREL_BUILD_PYTHON_BINDINGS "Build pybind11 Python bindings" OFF)

add_library(kestrel_diag
    src/core/CmdQueueMgm.cpp
    src/device/DeviceManager.cpp
    src/device/TPUDevice.cpp
    src/module/DDPModule.cpp
    src/module/DMCModule.cpp
    src/module/ISIModule.cpp
    src/module/PCIeModule.cpp
    src/module/PMUModule.cpp
)

target_include_directories(kestrel_diag
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
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
