# Shared by the Linux production build and portable software contract tests.
if(TARGET isml_uart_transport)
    return()
endif()
set(UART_SUITE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(UART_DIAG_DIR "${UART_SUITE_DIR}/isml_diag")
set(UART_ABIOS_DIR "${UART_SUITE_DIR}/abios")
add_library(isml_uart_protocol STATIC
    ${UART_ABIOS_DIR}/common/uart_ipc/uart_ipc_proto.c
    ${UART_ABIOS_DIR}/common/uart_ipc/uart_ipc_rx.c
    ${UART_ABIOS_DIR}/common/crc/crc32.c
    ${UART_ABIOS_DIR}/external/cobs/src/cobs.c)
target_include_directories(isml_uart_protocol PUBLIC
    ${UART_ABIOS_DIR}/common/uart_ipc
    ${UART_ABIOS_DIR}/common/crc
    ${UART_ABIOS_DIR}/external/cobs/src/include)
set_target_properties(isml_uart_protocol PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

add_library(isml_uart_transport STATIC
    ${UART_DIAG_DIR}/src/core/UartBridge.cpp
    ${UART_DIAG_DIR}/src/uart/UartProtocol.cpp)
target_compile_features(isml_uart_transport PUBLIC cxx_std_17)
target_include_directories(isml_uart_transport PUBLIC ${UART_DIAG_DIR}/include PRIVATE ${UART_DIAG_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(isml_uart_transport PUBLIC isml_uart_protocol Threads::Threads)
set_target_properties(isml_uart_transport PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(isml_uart_transport PRIVATE -Wall -Wextra)
endif()
