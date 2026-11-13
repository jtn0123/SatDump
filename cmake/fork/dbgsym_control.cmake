# CPack calls this before copying its completed archives to the output folder.
if(CPACK_GENERATOR STREQUAL "DEB")
    foreach(_package IN LISTS CPACK_PACKAGE_FILES)
        if(_package MATCHES "-dbgsym_.*\\.ddeb$")
            execute_process(COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/../../scripts/dbgsym-control.sh" "${_package}"
                RESULT_VARIABLE _result)
            if(NOT _result STREQUAL "0")
                message(FATAL_ERROR "Cannot normalize debug package checksums: ${_package}")
            endif()
        endif()
    endforeach()
endif()
