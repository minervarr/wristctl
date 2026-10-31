# ---------------------------------------------------------------------------
# gui/ — the App_shell + Vk_Canvas desktop frontend.
#
# Everything here is a consumer of the first-party framework libraries and of
# wristctl_core/wristctl_bluez. The window is Wayland-first; there is no X11
# path in vk_canvas today.
# ---------------------------------------------------------------------------

set(VK_CANVAS_DIR ${CMAKE_SOURCE_DIR}/libs/firstparty/framework/Vk_Canvas_Lb_LAW
    CACHE PATH "vk_canvas source root (consumed by app_shell too)" FORCE)

set(APP_SHELL_APP_NAME    "wristctl"       CACHE STRING "" FORCE)
set(APP_SHELL_WIN_CLASS   "WristctlMain"   CACHE STRING "" FORCE)
set(APP_SHELL_WINDOW_TITLE "wristctl"      CACHE STRING "" FORCE)
set(APP_SHELL_APP_ID      "wristctl"       CACHE STRING "" FORCE)

find_package(Threads REQUIRED)

add_subdirectory(${VK_CANVAS_DIR}/core ${CMAKE_BINARY_DIR}/vk_canvas_build)
add_subdirectory(${CMAKE_SOURCE_DIR}/libs/firstparty/framework/App_shell
                 ${CMAKE_BINARY_DIR}/app_shell_build)

if(WIN32 OR ANDROID)
    message(FATAL_ERROR
        "wristctl_gui is Linux/Wayland first; the Windows/Android hosts need "
        "their own wiring before they can build this target.")
endif()

add_subdirectory(${VK_CANVAS_DIR}/platform/linux
                 ${CMAKE_BINARY_DIR}/vk_canvas_linux_build)

# archive_engine: only the filesystem/layout vocabulary and its Result/logging
# utility. The archive, net and tag modules are OFF so configure never reaches
# the network or libarchive.
set(ARC_BUILD_ARCHIVE OFF CACHE BOOL "" FORCE)
set(ARC_BUILD_NET     OFF CACHE BOOL "" FORCE)
set(ARC_BUILD_TAG     OFF CACHE BOOL "" FORCE)
set(ARC_BUILD_FS      ON  CACHE BOOL "" FORCE)
set(ARC_BUILD_FS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/libs/firstparty/archive_engine/core
                 ${CMAKE_BINARY_DIR}/archive_engine_build)

# The UI text-size floor, derived from the face this app ships.
include(AppShellMinTextSize)
app_shell_generate_min_text_size(${CMAKE_SOURCE_DIR}/assets/fonts/ui/ui.otf)
add_dependencies(app_shell generate_ui_min_text_size)

include(${VK_CANVAS_DIR}/cmake/VceShaders.cmake)
if(NOT WIN32 AND NOT EXISTS "${VCE_SLANGC}")
    find_program(WRISTCTL_SLANGC slangc)
    if(WRISTCTL_SLANGC)
        set(VCE_SLANGC "${WRISTCTL_SLANGC}" CACHE FILEPATH "Slang compiler (slangc)" FORCE)
    else()
        message(FATAL_ERROR "slangc not found; install the Vulkan SDK")
    endif()
endif()

vce_compile_slang(compile_font_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${VK_CANVAS_DIR}/first_party/vulkan_font_engine/shaders_src
    composite_vert composite_frag tiling coverage msdf_vert msdf_frag
    glyph_area glyph_resolve)
vce_compile_slang(compile_canvas_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${VK_CANVAS_DIR}/shaders_src
    overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

add_executable(wristctl_gui
    src/gui_main.cc
    src/watch_view.cc
    src/watch_view_features.cc
    src/ble_worker.cc
    src/bluez_link.cc
    src/settings.cc
    src/sntp_client.cc
    src/utc_sync.cc)

target_link_libraries(wristctl_gui PRIVATE
    wristctl_core
    wristctl_bluez
    app_shell
    app_shell_wayland
    vk_canvas_core
    vk_canvas_wayland
    arc_fs
    Threads::Threads)

target_compile_options(wristctl_gui PRIVATE -Wall -Wextra)
add_dependencies(wristctl_gui compile_font_shaders compile_canvas_shaders)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(settings_test tests/settings_test.cc src/settings.cc)
    target_link_libraries(settings_test PRIVATE arc_fs)
    target_include_directories(settings_test PRIVATE src)
    target_compile_options(settings_test PRIVATE -Wall -Wextra)
    add_test(NAME settings_test COMMAND settings_test)

    add_executable(watch_model_test tests/watch_model_test.cc)
    target_include_directories(watch_model_test PRIVATE src)
    target_link_libraries(watch_model_test PRIVATE wristctl_core)
    target_compile_options(watch_model_test PRIVATE -Wall -Wextra)
    add_test(NAME watch_model_test COMMAND watch_model_test)

    add_executable(reconnect_test tests/reconnect_test.cc)
    target_include_directories(reconnect_test PRIVATE src)
    target_compile_options(reconnect_test PRIVATE -Wall -Wextra)
    add_test(NAME reconnect_test COMMAND reconnect_test)

    add_executable(sntp_client_test tests/sntp_client_test.cc src/sntp_client.cc)
    target_include_directories(sntp_client_test PRIVATE src)
    target_link_libraries(sntp_client_test PRIVATE Threads::Threads)
    target_compile_options(sntp_client_test PRIVATE -Wall -Wextra)
    add_test(NAME sntp_client_test COMMAND sntp_client_test)

    add_executable(utc_sync_test tests/utc_sync_test.cc src/utc_sync.cc)
    target_include_directories(utc_sync_test PRIVATE src)
    target_link_libraries(utc_sync_test PRIVATE wristctl_core)
    target_compile_options(utc_sync_test PRIVATE -Wall -Wextra)
    add_test(NAME utc_sync_test COMMAND utc_sync_test)
endif()

# Shaders are looked up beside the executable, which is where
# FileAssetReader expects them.
add_custom_target(wristctl_gui_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:wristctl_gui>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/assets $<TARGET_FILE_DIR:wristctl_gui>/assets
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:wristctl_gui>/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets/fonts/ui
            $<TARGET_FILE_DIR:wristctl_gui>/fonts/ui
    COMMENT "Copying shaders and UI font next to wristctl_gui")
add_dependencies(wristctl_gui_assets compile_font_shaders compile_canvas_shaders)
add_dependencies(wristctl_gui wristctl_gui_assets)
