# Exercise the real CLI/registry with the software fixture. Host DMA allocation
# intentionally fails in this fixture; reaching it proves argument dispatch,
# not a successful hardware transfer.
function(check_dma size timeout)
    execute_process(COMMAND "${CLI}" pcie_dma_data_transfer ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 2 OR
       NOT output MATCHES "DMA request: direction=h2d size_bytes=${size}" OR
       NOT output MATCHES "timeout_ms=${timeout}" OR
       NOT output MATCHES "host DMA allocation failed" OR
       NOT output MATCHES "TPU0::pcie_dma_data_transfer")
        message(FATAL_ERROR "DMA CLI dispatch failed (${result}): ${output}${error}")
    endif()
endfunction()

check_dma(4096 1000)
check_dma(4096 1000 --drv isml_drv)
check_dma(65536 1000 --drv none --size-bytes 65536)
check_dma(134217728 30000 --target TPU0 --direction h2d
    --size-bytes 0x08000000 --pattern incremental --timeout-ms 30000)
check_dma(4096 60001 --timeout-ms 60001)

function(check_host_internal direction)
    execute_process(COMMAND "${CLI}" pcie_dma_data_transfer --drv none
        --direction ${direction} --size-bytes 256
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 2 OR
       NOT output MATCHES "DMA request: direction=${direction} size_bytes=256" OR
       NOT output MATCHES "host DMA allocation failed" OR
       NOT output MATCHES "TPU0::pcie_dma_data_transfer")
        message(FATAL_ERROR
            "${direction} DMA CLI dispatch failed (${result}): ${output}${error}")
    endif()
endfunction()

check_host_internal(h2i)
check_host_internal(i2h)
check_host_internal(h2s)
check_host_internal(s2h)

function(check_invalid expected)
    execute_process(COMMAND "${CLI}" ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 8 OR NOT output MATCHES "${expected}" OR
       output MATCHES "DMA request:")
        message(FATAL_ERROR "Invalid CLI accepted (${result}): ${output}${error}")
    endif()
endfunction()

check_invalid("module prefix" dma_data_transfer)
check_invalid("argument=size" pcie_dma_data_transfer --size 4096)
check_invalid("invalid drv=other" pcie_dma_data_transfer --drv other)
check_invalid("invalid drv=isml_diag" pcie_dma_data_transfer --drv isml_diag)
check_invalid("invalid internal-memory range" pcie_dma_data_transfer
    --direction h2i --size-bytes 288)
check_invalid("invalid internal-memory range" pcie_dma_data_transfer
    --direction h2s --size-bytes 256 --intermediate-offset 32)

execute_process(COMMAND "${CLI}" pcie_dma_h2d_example --drv none
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE example_result
    OUTPUT_VARIABLE example_output ERROR_VARIABLE example_error)
if(NOT example_result EQUAL 2 OR
   NOT example_output MATCHES "DMA example allocation failed" OR
   NOT example_output MATCHES "TPU0::pcie_dma_h2d_example")
    message(FATAL_ERROR
        "DMA example drv dispatch failed (${example_result}): ${example_output}${example_error}")
endif()

execute_process(COMMAND "${CLI}" pcie_dma_data_transfer -all
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE all_result
    OUTPUT_VARIABLE all_output ERROR_VARIABLE all_error)
if(NOT all_output MATCHES "\\[RUN 1/8\\]" OR
   NOT all_output MATCHES "\\[RUN 8/8\\]" OR
   NOT all_output MATCHES "\\[SUMMARY\\] completed=8" OR
   all_result EQUAL 0)
    message(FATAL_ERROR "DMA -all dispatch failed (${all_result}): ${all_output}${all_error}")
endif()

execute_process(COMMAND "${CLI}" pcie_dma_data_transfer -all --size-bytes 4096
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE mixed_result
    OUTPUT_VARIABLE mixed_output ERROR_VARIABLE mixed_error)
if(NOT mixed_result EQUAL 1 OR
   NOT mixed_error MATCHES "-all cannot be combined with testcase arguments")
    message(FATAL_ERROR "DMA -all accepted testcase arguments: ${mixed_output}${mixed_error}")
endif()
