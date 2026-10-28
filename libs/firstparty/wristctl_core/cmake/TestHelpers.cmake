# ---------------------------------------------------------------------------
# wristctl_add_test(<name> SOURCES <src>... [INCLUDES <dir>...] [LIBS <lib>...])
#
# One assert()-based test executable registered with CTest. The convention is
# the workspace's: plain assert(), no framework, Debug-only, run directly.
# Release configurations simply produce no test targets.
# ---------------------------------------------------------------------------
function(wristctl_add_test name)
    if(NOT WRISTCTL_BUILD_TESTS)
        return()
    endif()
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()

    cmake_parse_arguments(T "" "" "SOURCES;INCLUDES;LIBS" ${ARGN})
    if(NOT T_SOURCES)
        message(FATAL_ERROR "wristctl_add_test(${name}) requires SOURCES")
    endif()

    add_executable(${name} ${T_SOURCES})
    if(T_INCLUDES)
        target_include_directories(${name} PRIVATE ${T_INCLUDES})
    endif()
    if(T_LIBS)
        target_link_libraries(${name} PRIVATE ${T_LIBS})
    endif()
    if(MSVC)
        target_compile_options(${name} PRIVATE /W3 /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME ${name} COMMAND ${name})
endfunction()
