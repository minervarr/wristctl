cmake_minimum_required(VERSION 3.22)

project(wristctl_core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WRISTCTL_BUILD_TESTS "Build the Debug-only assert() test suites" ON)

# Portable C11: no OS headers, no JNI, no platform #ifdefs. Everything a
# transport needs is pushed in through the session seam (session.h).
add_library(wristctl_core STATIC
    src/generated.c
    src/models.c
    src/session.c)
add_library(wristctl_core::wristctl_core ALIAS wristctl_core)

target_include_directories(wristctl_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wristctl_core PUBLIC c_std_11)
set_target_properties(wristctl_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(wristctl_core PRIVATE /W3 /utf-8)
elseif(ANDROID)
    target_compile_options(wristctl_core PRIVATE -Wall -Wextra)
else()
    target_compile_options(wristctl_core PRIVATE -Wall -Wextra -Werror)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
if(BUILD_TESTING AND WRISTCTL_BUILD_TESTS)
    include(TestHelpers)

    wristctl_add_test(test_vectors
        SOURCES  tests/test_vectors.c
        INCLUDES tests
        LIBS     wristctl_core)

    wristctl_add_test(test_models
        SOURCES  tests/test_models.c
        LIBS     wristctl_core)

    wristctl_add_test(test_session
        SOURCES  tests/test_session.c
        LIBS     wristctl_core)

    wristctl_add_test(test_cpp
        SOURCES  tests/test_cpp.cc
        LIBS     wristctl_core)
    if(TARGET test_cpp)
        set_target_properties(test_cpp PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
    endif()
endif()
