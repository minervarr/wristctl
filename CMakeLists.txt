cmake_minimum_required(VERSION 3.22)

project(wristctl VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# C11 for the portable core; C++17 only for the consumer test and, later, the
# App_shell GUI. No compiler extensions, matching the sibling projects.
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WRISTCTL_BUILD_TESTS "Build the Debug-only assert() test suites" ON)
option(WRISTCTL_BUILD_GUI "Build the App_shell/Vulkan desktop frontend" OFF)

# A parent project that only wants the codecs must not need sd-bus. The
# standalone tree builds the transport by default.
if(PROJECT_IS_TOP_LEVEL)
    set(WRISTCTL_BLUEZ_DEFAULT ON)
else()
    set(WRISTCTL_BLUEZ_DEFAULT OFF)
endif()
option(WRISTCTL_BUILD_BLUEZ "Build the Linux BlueZ transport (sd-bus)"
       ${WRISTCTL_BLUEZ_DEFAULT})

include(CTest)

# The protocol library. It owns the spec, the generators and the committed
# generated sources; the Python and Android trees are consumers.
add_subdirectory(libs/firstparty/wristctl_core)

if(UNIX AND NOT ANDROID AND WRISTCTL_BUILD_BLUEZ)
    add_subdirectory(platform/linux)
endif()

if(WRISTCTL_BUILD_GUI)
    add_subdirectory(gui)
endif()
