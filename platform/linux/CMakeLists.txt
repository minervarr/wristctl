cmake_minimum_required(VERSION 3.22)

project(wristctl_linux LANGUAGES C)

# BlueZ transport: sd-bus only, no libdbus, no shelling out to bluetoothctl.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_library(wristctl_bluez STATIC
    src/bluez_transport.c)

target_include_directories(wristctl_bluez PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wristctl_bluez PUBLIC
    wristctl_core
    PkgConfig::SYSTEMD)

if(MSVC)
    target_compile_options(wristctl_bluez PRIVATE /W3)
else()
    target_compile_options(wristctl_bluez PRIVATE -Wall -Wextra)
endif()

# Headless bring-up and automation frontend. It uses the same transport and
# session as the GUI, so a protocol path proven here is the GUI's path too.
add_executable(wristctl_cli src/cli_main.c)
target_link_libraries(wristctl_cli PRIVATE wristctl_bluez)
if(MSVC)
    target_compile_options(wristctl_cli PRIVATE /W3)
else()
    target_compile_options(wristctl_cli PRIVATE -Wall -Wextra)
endif()
