# Exercise the real CLI/registry with the software fixture. Host DMA allocation
# intentionally fails in this fixture; reaching it proves argument dispatch,
# not a successful hardware transfer.
function(check_dma size timeout)
    execute_process(COMMAND "${CLI}" pcie_dma_data_transfer ${ARGN}
        WORKING_DIRECTORY "${REPO}" RESULT_VARIABLE result
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
check_dma(134217728 30000 --target TPU0 --direction h2d
    --size-bytes 0x08000000 --pattern incremental --timeout-ms 30000)

function(check_invalid expected)
    execute_process(COMMAND "${CLI}" ${ARGN}
        WORKING_DIRECTORY "${REPO}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 8 OR NOT output MATCHES "${expected}" OR
       output MATCHES "DMA request:")
        message(FATAL_ERROR "Invalid CLI accepted (${result}): ${output}${error}")
    endif()
endfunction()

check_invalid("module prefix" dma_data_transfer)
check_invalid("argument=size" pcie_dma_data_transfer --size 4096)
check_invalid("outside policy range" pcie_dma_data_transfer --size-bytes 0x10000000)
