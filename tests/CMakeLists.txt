cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(IsmlIoTests LANGUAGES C CXX)
    enable_testing()
endif()

# Portable software tests only. Production still builds exclusively on Linux.
# Compile the production testcase/dispatch code with a test-only mapping owner;
# no sysfs access, DMA allocation, or hardware behavior is simulated here.
set(REPO ${CMAKE_CURRENT_LIST_DIR}/..)
file(GLOB_RECURSE IO_TEST_SOURCES CONFIGURE_DEPENDS
    ${REPO}/isml_diag/src/*.cpp
    ${REPO}/isml_diag/testcases/*.cpp
    ${REPO}/isml_lib/implementer/src/*.cpp)
list(REMOVE_ITEM IO_TEST_SOURCES ${REPO}/isml_diag/src/core/HalContext.cpp)
add_library(io_test_code STATIC ${IO_TEST_SOURCES}
    hal_fixture.cpp
    ${REPO}/mock/ProtonHal/src/phal/init.c
    ${REPO}/mock/ProtonHal/src/components/pcie/pcie.c
    ${REPO}/mock/ProtonHal/src/components/isi/isi.c)
target_compile_features(io_test_code PUBLIC cxx_std_17)
target_include_directories(io_test_code PUBLIC
    ${REPO}/isml_diag/include
    ${REPO}/isml_lib/implementer/include
    ${REPO}/mock/ProtonHal/include
    PRIVATE ${REPO}/isml_lib/implementer/src)
target_compile_definitions(io_test_code PUBLIC HAL_DV_MODE=1 PHAL_TARGET_ENV_DV=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(io_test_code PRIVATE -Wall -Wextra)
endif()
find_package(Threads REQUIRED)
target_link_libraries(io_test_code PUBLIC Threads::Threads)
add_executable(io_tests io_tests.cpp)
target_link_libraries(io_tests PRIVATE io_test_code)
add_test(NAME io_contract COMMAND io_tests)
set_tests_properties(io_contract PROPERTIES WORKING_DIRECTORY ${REPO})
add_executable(io_test_cli ${REPO}/isml_diag/cli/main.cpp)
target_link_libraries(io_test_cli PRIVATE io_test_code)
add_test(NAME cli_help COMMAND io_test_cli --help)
add_test(NAME cli_discover COMMAND io_test_cli discover --tree)
set_tests_properties(cli_discover PROPERTIES WORKING_DIRECTORY ${REPO})
