cmake_minimum_required(VERSION 3.22.1)
project(wristctl C)

# android/app/src/main/cpp -> wristctl/
set(WRISTCTL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")
set(WRISTCTL_CORE "${WRISTCTL_ROOT}/libs/firstparty/wristctl_core")

if(NOT EXISTS "${WRISTCTL_CORE}/include/wristctl/generated.h")
    message(FATAL_ERROR "C core not found at ${WRISTCTL_CORE}; run gen/gen_c.py first")
endif()

# The app links the same target the desktop tests build. Tests stay off: they
# produce host executables, which make no sense inside an ABI build.
set(WRISTCTL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${WRISTCTL_CORE} ${CMAKE_BINARY_DIR}/wristctl_core)

add_library(wristctl SHARED wristctl_jni.c)
target_link_libraries(wristctl PRIVATE wristctl_core)
target_compile_options(wristctl PRIVATE -Wall -Wextra)
