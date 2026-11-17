cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(SpevIoTests LANGUAGES C CXX)
    enable_testing()
endif()

# Portable software tests only. Production still builds exclusively on Linux.
# Compile the production testcase/dispatch code with a test-only mapping owner;
# no sysfs access, DMA allocation, or hardware behavior is simulated here.
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include(${REPO}/Uart.cmake)
set(TEST_POLICY_SOURCE_DIR ${REPO}/spev_diag/policies)
set(TEST_POLICY_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/policies)
file(REMOVE_RECURSE "${TEST_POLICY_BINARY_DIR}")
file(COPY "${TEST_POLICY_SOURCE_DIR}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
add_custom_target(io_test_policies ALL
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${TEST_POLICY_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${TEST_POLICY_SOURCE_DIR}" "${TEST_POLICY_BINARY_DIR}")
file(GLOB_RECURSE IO_TEST_SOURCES CONFIGURE_DEPENDS
    ${REPO}/spev_diag/src/*.cpp
    ${REPO}/spev_diag/testcases/*.cpp
    ${REPO}/spev_lib/implementer/src/*.cpp)
list(REMOVE_ITEM IO_TEST_SOURCES ${REPO}/spev_diag/src/core/HalContext.cpp)
list(REMOVE_ITEM IO_TEST_SOURCES ${REPO}/spev_diag/src/core/HalContextDma.cpp)
list(REMOVE_ITEM IO_TEST_SOURCES
    ${REPO}/spev_diag/src/core/UartBridge.cpp
    ${REPO}/spev_diag/src/uart/UartProtocol.cpp
    ${REPO}/spev_diag/src/uart/UartConsole.cpp)
add_library(io_test_code STATIC ${IO_TEST_SOURCES}
    hal_fixture.cpp
    ${REPO}/spev_ipc/external/ProtonHal/src/phal/init.c
    ${REPO}/spev_ipc/external/ProtonHal/src/phal/ops.c
    ${REPO}/spev_ipc/external/ProtonHal/src/components/pcie/pcie.c
    ${REPO}/spev_ipc/external/ProtonHal/src/components/isi/isi.c)
target_compile_features(io_test_code PUBLIC cxx_std_17)
target_include_directories(io_test_code PUBLIC
    ${REPO}/spev_diag/include
    ${REPO}/spev_lib/implementer/include
    ${REPO}/spev_ipc/external/ProtonHal/include
    ${REPO}/spev_ipc/external
    PRIVATE ${REPO}/spev_lib/implementer/src)
target_compile_definitions(io_test_code PUBLIC HAL_DV_MODE=1 PHAL_TARGET_ENV_DV=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(io_test_code PRIVATE -Wall -Wextra)
endif()
find_package(Threads REQUIRED)
target_link_libraries(io_test_code PUBLIC Threads::Threads spev_uart_transport)
add_executable(uart_tests uart_tests.cpp)
target_include_directories(uart_tests PRIVATE ${REPO}/spev_diag/src)
target_link_libraries(uart_tests PRIVATE io_test_code)
add_test(NAME uart_contract COMMAND uart_tests)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uart_integration_tests uart_integration_tests.cpp)
    target_include_directories(uart_integration_tests PRIVATE ${REPO}/spev_diag/src)
    target_link_libraries(uart_integration_tests PRIVATE io_test_code)
    add_test(NAME uart_integration COMMAND uart_integration_tests)
    set_tests_properties(uart_integration PROPERTIES TIMEOUT 30)
endif()
if(NOT TARGET spev_uart)
    add_executable(spev_uart ${REPO}/spev_diag/cli/uart_main.cpp)
    target_include_directories(spev_uart PRIVATE ${REPO}/spev_diag/src)
    target_link_libraries(spev_uart PRIVATE io_test_code)
endif()
add_test(NAME uart_console_help COMMAND spev_uart --help)
add_test(NAME uart_console_invalid_timeout COMMAND spev_uart console --response-timeout-ms 0)
set_tests_properties(uart_console_invalid_timeout PROPERTIES WILL_FAIL TRUE)
add_executable(io_tests io_tests.cpp)
target_link_libraries(io_tests PRIVATE io_test_code)
add_test(NAME io_contract COMMAND io_tests)
add_executable(cp_queue_tests cp_queue_tests.cpp)
target_link_libraries(cp_queue_tests PRIVATE io_test_code)
add_test(NAME cp_queue_contract COMMAND cp_queue_tests)
add_executable(dma_tests dma_tests.cpp)
target_link_libraries(dma_tests PRIVATE io_test_code)
add_test(NAME dma_contract COMMAND dma_tests)
set_tests_properties(io_contract PROPERTIES WORKING_DIRECTORY ${REPO})
add_executable(io_test_cli ${REPO}/spev_diag/cli/main.cpp)
target_link_libraries(io_test_cli PRIVATE io_test_code)
add_dependencies(io_test_cli io_test_policies)
add_test(NAME cli_help COMMAND io_test_cli --help)
set_tests_properties(cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "error\\|info\\|flow\\|regio")
add_test(NAME cli_flow_log_level COMMAND io_test_cli pcie_bar_read32 --bar-index 0 --offset 4 --log-level flow)
add_test(NAME cli_regio_log_level COMMAND io_test_cli pcie_bar_read32 --bar-index 0 --offset 4 --log-level regio)
add_test(NAME cli_legacy_debug_log_level COMMAND io_test_cli pcie_bar_read32 --bar-index 0 --offset 4 --log-level debug)
add_test(NAME cli_legacy_trace_log_level COMMAND io_test_cli pcie_bar_read32 --bar-index 0 --offset 4 --log-level trace)
add_test(NAME cli_bar_write32 COMMAND io_test_cli pcie_bar_write32 --bar-index 0 --offset 4 --value 0x1234abcd)
add_test(NAME cli_discover COMMAND io_test_cli discover)
set_tests_properties(
    cli_discover cli_bar_write32 cli_flow_log_level cli_regio_log_level
    cli_legacy_debug_log_level cli_legacy_trace_log_level
    PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME cli_reject_backend COMMAND io_test_cli discover --backend ihal)
set_tests_properties(cli_reject_backend PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    WILL_FAIL TRUE)

add_test(NAME cli_default_target COMMAND io_test_cli pcie_bar_read32 --bar-index 0 --offset 4)
add_test(NAME cli_tpu_target COMMAND io_test_cli pcie_bar_read32 --target TPU0 --bar-index 0 --offset 4)
set_tests_properties(cli_default_target cli_tpu_target PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR} PASS_REGULAR_EXPRESSION "TPU0::pcie_bar_read32")

add_test(NAME cli_dma_arguments COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:io_test_cli> -DREPO=${REPO}
    -DWORK_DIR=${CMAKE_BINARY_DIR}
    -P ${CMAKE_CURRENT_LIST_DIR}/cli_dma_arguments.cmake)
