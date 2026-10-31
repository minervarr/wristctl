cmake_minimum_required(VERSION 3.22.1)

# The Android frontend, reached from app/build.gradle.kts.
#
# It is the same application as the desktop: gui/src/watch_view.cc and
# gui/src/ble_worker.cc are compiled unmodified into libwristctl.so, with
# AndroidHost as the third Host and BleLink.java as the BLE transport.
# WatchNotificationListener.java only forwards notification metadata to the
# shared worker. No UI is in Java and no watch protocol is in Java.
project(wristctl_android LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(WRISTCTL_ROOT ${REPO_ROOT})
set(VK_CANVAS_DIR
    ${REPO_ROOT}/libs/firstparty/framework/Vk_Canvas_Lb_LAW
    CACHE PATH "vk_canvas source root (consumed by app_shell too)" FORCE)
set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
set(WRISTCTL_ANDROID_ASSETS
    ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/assets)

# app_shell names the log file/logcat tag, the Wayland app id and the Win32
# class. Only the first matters here, but all four must exist before
# add_subdirectory(app_shell).
set(APP_SHELL_APP_NAME    "wristctl"     CACHE STRING "" FORCE)
set(APP_SHELL_WIN_CLASS   "WristctlMain" CACHE STRING "" FORCE)
set(APP_SHELL_WINDOW_TITLE "wristctl"    CACHE STRING "" FORCE)
set(APP_SHELL_APP_ID      "wristctl"     CACHE STRING "" FORCE)
set(APP_SHELL_NATIVE_APP_GLUE_DIR ${NATIVE_APP_GLUE} CACHE PATH "" FORCE)

# The core is added as a subdirectory, never copied. Tests produce host
# executables and make no sense in an ABI build.
set(WRISTCTL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(WRISTCTL_BUILD_BLUEZ OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/libs/firstparty/wristctl_core
                 ${CMAKE_BINARY_DIR}/wristctl_core)

# archive_engine: only arc_fs, the settings path vocabulary. No archive, net or
# tag modules, so configure never reaches the network or libarchive.
set(ARC_BUILD_ARCHIVE OFF CACHE BOOL "" FORCE)
set(ARC_BUILD_NET     OFF CACHE BOOL "" FORCE)
set(ARC_BUILD_TAG     OFF CACHE BOOL "" FORCE)
set(ARC_BUILD_FS      ON  CACHE BOOL "" FORCE)
set(ARC_BUILD_FS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/libs/firstparty/archive_engine/core
                 ${CMAKE_BINARY_DIR}/archive_engine)

# ui_metrics.cc needs the build-generated legibility floor. The generator
# builds and RUNS a host tool, which cannot be done under the NDK
# cross-compiler, so take the header from a desktop build tree. It is pure font
# geometry and identical on every platform.
set(UI_MIN_TEXT_SIZE_CANDIDATES
    ${REPO_ROOT}/build/linux_debug/generated/ui_min_text_size.gen.h
    ${REPO_ROOT}/build/linux/generated/ui_min_text_size.gen.h
    ${REPO_ROOT}/build-gui/generated/ui_min_text_size.gen.h
    ${REPO_ROOT}/build-gui-rea/generated/ui_min_text_size.gen.h)
set(UI_MIN_TEXT_SIZE_SRC "")
foreach(candidate ${UI_MIN_TEXT_SIZE_CANDIDATES})
    if(EXISTS ${candidate})
        set(UI_MIN_TEXT_SIZE_SRC ${candidate})
        break()
    endif()
endforeach()
if(NOT UI_MIN_TEXT_SIZE_SRC)
    message(FATAL_ERROR
        "ui_min_text_size.gen.h not found in any desktop build tree. "
        "Run ./scripts/build.sh build --debug first (it derives the header "
        "from assets/fonts/ui/ui.otf).")
endif()
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
configure_file(${UI_MIN_TEXT_SIZE_SRC}
               ${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h COPYONLY)

# vk_canvas first: app_shell needs its core headers and one backend.
add_subdirectory(${VK_CANVAS_DIR}/core ${CMAKE_BINARY_DIR}/vk_canvas_core)
add_subdirectory(${REPO_ROOT}/libs/firstparty/framework/App_shell
                 ${CMAKE_BINARY_DIR}/app_shell)

# Shaders: the full set the desktop GUI compiles. Output lands directly in the
# Gradle module's assets so AGP packages it; the asset-merge task is made to
# depend on the native build in app/build.gradle.kts.
include(${VK_CANVAS_DIR}/cmake/VceShaders.cmake)
if(NOT EXISTS "${VCE_SLANGC}")
    find_program(WRISTCTL_SLANGC slangc)
    if(WRISTCTL_SLANGC)
        set(VCE_SLANGC "${WRISTCTL_SLANGC}" CACHE FILEPATH "Slang compiler" FORCE)
    else()
        message(FATAL_ERROR "slangc not found; install the Vulkan SDK")
    endif()
endif()
vce_compile_slang(compile_font_shaders
    ${WRISTCTL_ANDROID_ASSETS}/shaders
    ${VK_CANVAS_DIR}/first_party/vulkan_font_engine/shaders_src
    composite_vert composite_frag tiling coverage msdf_vert msdf_frag
    glyph_area glyph_resolve)
vce_compile_slang(compile_canvas_shaders
    ${WRISTCTL_ANDROID_ASSETS}/shaders
    ${VK_CANVAS_DIR}/shaders_src
    overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

add_library(wristctl SHARED
    app/src/main/cpp/main.cc
    app/src/main/cpp/android_link.cc
    ${VK_CANVAS_DIR}/platform/android/android_platform.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c
    ${REPO_ROOT}/gui/src/watch_view.cc
    ${REPO_ROOT}/gui/src/watch_view_features.cc
    ${REPO_ROOT}/gui/src/ble_worker.cc
    ${REPO_ROOT}/gui/src/settings.cc
    ${REPO_ROOT}/gui/src/sntp_client.cc
    ${REPO_ROOT}/gui/src/utc_sync.cc)

target_include_directories(wristctl PRIVATE
    ${REPO_ROOT}/gui/src
    ${NATIVE_APP_GLUE}
    ${VK_CANVAS_DIR}/platform/android
    ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(wristctl PRIVATE
    wristctl_core
    app_shell
    app_shell_android
    vk_canvas_core
    arc_fs
    android
    log)

target_compile_options(wristctl PRIVATE -Wall -Wextra)
add_dependencies(wristctl compile_font_shaders compile_canvas_shaders)

# native_app_glue's ANativeActivity_onCreate is the entry point; keep it
# against linker GC.
target_link_options(wristctl PRIVATE -u ANativeActivity_onCreate)

# The one static input the APK needs beside the shaders. The font list is the
# app's own; only the UI face is used.
add_custom_command(TARGET wristctl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${WRISTCTL_ANDROID_ASSETS}/fonts/ui
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${REPO_ROOT}/assets/fonts/ui/ui.otf
            ${WRISTCTL_ANDROID_ASSETS}/fonts/ui/ui.otf
    COMMENT "Copying the UI font into APK assets")
