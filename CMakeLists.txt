cmake_minimum_required(VERSION 3.20)

project(anispaper LANGUAGES C CXX)

include(CTest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROBE_PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/src/probe/protocols/wlr-layer-shell-unstable-v1.xml)
set(PROBE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PROBE_PROTOCOL_HEADER ${PROBE_GENERATED_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h)
set(PROBE_PROTOCOL_CODE ${PROBE_GENERATED_DIR}/wlr-layer-shell-unstable-v1-protocol.c)
set(OUTPUT_ORDER_PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon/protocols/kde-output-order-v1.xml)
set(OUTPUT_ORDER_HEADER ${PROBE_GENERATED_DIR}/kde-output-order-v1-client-protocol.h)
set(OUTPUT_ORDER_CODE ${PROBE_GENERATED_DIR}/kde-output-order-v1-protocol.c)

add_custom_command(
    OUTPUT ${PROBE_PROTOCOL_HEADER} ${PROBE_PROTOCOL_CODE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROBE_GENERATED_DIR}
    COMMAND ${WAYLAND_SCANNER} client-header ${PROBE_PROTOCOL} ${PROBE_PROTOCOL_HEADER}
    COMMAND ${WAYLAND_SCANNER} private-code ${PROBE_PROTOCOL} ${PROBE_PROTOCOL_CODE}
    DEPENDS ${PROBE_PROTOCOL}
    VERBATIM
)
add_custom_command(
    OUTPUT ${OUTPUT_ORDER_HEADER} ${OUTPUT_ORDER_CODE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROBE_GENERATED_DIR}
    COMMAND ${WAYLAND_SCANNER} client-header ${OUTPUT_ORDER_PROTOCOL} ${OUTPUT_ORDER_HEADER}
    COMMAND ${WAYLAND_SCANNER} private-code ${OUTPUT_ORDER_PROTOCOL} ${OUTPUT_ORDER_CODE}
    DEPENDS ${OUTPUT_ORDER_PROTOCOL}
    VERBATIM
)

add_executable(anispaper-layer-shell-probe
    src/probe/main.cpp
    src/probe/protocols/xdg-popup-stub.c
    ${PROBE_PROTOCOL_CODE}
    ${PROBE_PROTOCOL_HEADER}
)
set_source_files_properties(${PROBE_PROTOCOL_CODE} PROPERTIES GENERATED TRUE)
target_compile_features(anispaper-layer-shell-probe PRIVATE cxx_std_20)
target_include_directories(anispaper-layer-shell-probe PRIVATE ${PROBE_GENERATED_DIR})
target_link_libraries(anispaper-layer-shell-probe PRIVATE PkgConfig::WAYLAND_CLIENT)
target_compile_options(anispaper-layer-shell-probe PRIVATE -Wall -Wextra -Wpedantic)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network Gui OpenGL Widgets WebEngineCore WebEngineWidgets Quick Qml DBus)
find_package(KF6Auth 6.0 REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
set(CMAKE_AUTOMOC ON)

# Offscreen Wallpaper Engine scene renderer child (vendored engine).  The
# child runs in its own process so scene parsing/shaders/GL never touch the
# daemon or plasmashell.
option(ANISPAPER_ENABLE_SCENE_ENGINE "Build the offscreen scene wallpaper engine child" ON)
if(ANISPAPER_ENABLE_SCENE_ENGINE)
  set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  # kissfft re-enables BUILD_SHARED_LIBS internally unless KISSFFT_STATIC is
  # set; keep the scene engine child fully self-contained.
  set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
  set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
  set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
  set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
  add_subdirectory(third_party/linux-wallpaperengine EXCLUDE_FROM_ALL)
  add_executable(anis-paper-scene-engine src/scene_engine/main.cpp)
  target_compile_features(anis-paper-scene-engine PRIVATE cxx_std_20)
  # The engine registers its GLFW driver through __attribute__((constructor)),
  # which a static library would drop when no symbol is referenced.
  target_link_libraries(anis-paper-scene-engine PRIVATE
      -Wl,--whole-archive wallpaperengine-core -Wl,--no-whole-archive
      PkgConfig::JPEG)
endif()

add_library(anispaper-bridge STATIC
    src/bridge/frame_protocol.h
    src/bridge/frame_bridge.h
    src/bridge/frame_bridge.cpp
)
target_compile_features(anispaper-bridge PUBLIC cxx_std_20)
target_link_libraries(anispaper-bridge PUBLIC Qt6::Core Qt6::Gui)
target_compile_options(anispaper-bridge PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(anispaper-bridge PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(anis-paperd
    src/daemon/main.cpp
    src/daemon/plasma_login_manager.h
    src/daemon/plasma_login_manager.cpp
    src/daemon/plasma_login_wallpaper.h
    src/daemon/plasma_login_wallpaper.cpp
    src/daemon/plasma_wallpaper_activator.h
    src/daemon/plasma_wallpaper_activator.cpp
    src/common/wayland_monitor.cpp
    src/renderers/renderer.h
    src/renderers/static_image_renderer.h
    src/renderers/static_image_renderer.cpp
    src/renderers/scene_renderer.h
    src/renderers/scene_renderer.cpp
    src/renderers/isolated_renderer.h
    src/renderers/isolated_renderer.cpp
    src/renderers/video_renderer.h
    src/renderers/video_renderer.cpp
    src/renderers/web_renderer.h
    src/renderers/web_renderer.cpp
    src/renderers/renderer_manager.h
    src/renderers/renderer_manager.cpp
    src/renderers/renderer_child.h
    src/renderers/renderer_child.cpp
)
target_compile_features(anis-paperd PRIVATE cxx_std_20)
target_link_libraries(anis-paperd PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::DBus
    KF6::AuthCore
    Qt6::Gui
    Qt6::OpenGL
    Qt6::Widgets
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    anispaper-bridge
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::MPV
)
target_compile_options(anis-paperd PRIVATE -Wall -Wextra -Wpedantic)

add_executable(anispaper-plasma-output-map
    src/daemon/plasma_output_map_helper.cpp
    src/daemon/plasma_wallpaper_activator.h
    src/daemon/plasma_wallpaper_activator.cpp
    ${OUTPUT_ORDER_CODE}
    ${OUTPUT_ORDER_HEADER})
set_source_files_properties(${OUTPUT_ORDER_CODE} PROPERTIES GENERATED TRUE)
target_compile_features(anispaper-plasma-output-map PRIVATE cxx_std_20)
target_include_directories(anispaper-plasma-output-map PRIVATE ${PROBE_GENERATED_DIR})
target_link_libraries(anispaper-plasma-output-map PRIVATE Qt6::Core Qt6::Gui Qt6::DBus PkgConfig::WAYLAND_CLIENT)
target_compile_options(anispaper-plasma-output-map PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(anis-paperd anispaper-plasma-output-map)

# This QML extension registers image://anispaper on the Plasma wallpaper
# engine. Keep its build-tree layout identical to the installed local module
# so the dummy QQuickView test exercises the real dynamic plugin.
set(ANISPAPER_FRAME_QML_BUILD_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/qml/org/anispaper/frame)
file(MAKE_DIRECTORY ${ANISPAPER_FRAME_QML_BUILD_DIR})
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/plasma/org.anispaper.frame/contents/ui/org/anispaper/frame/qmldir
    ${ANISPAPER_FRAME_QML_BUILD_DIR}/qmldir
    COPYONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/plasma/org.anispaper.frame/contents/ui/org/anispaper/frame/FrameBridgeSupport.qml
    ${ANISPAPER_FRAME_QML_BUILD_DIR}/FrameBridgeSupport.qml
    COPYONLY)
add_library(anispaperframeprovider MODULE
    src/plasma/frame_image_provider.h
    src/plasma/frame_image_provider.cpp
    src/plasma/anispaper_frame_plugin.cpp
)
target_compile_features(anispaperframeprovider PRIVATE cxx_std_20)
target_link_libraries(anispaperframeprovider PRIVATE anispaper-bridge Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
target_compile_options(anispaperframeprovider PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(anispaperframeprovider PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${ANISPAPER_FRAME_QML_BUILD_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${ANISPAPER_FRAME_QML_BUILD_DIR}
)

install(TARGETS anis-paperd anispaper-plasma-output-map RUNTIME DESTINATION bin)
if(ANISPAPER_ENABLE_SCENE_ENGINE)
  install(TARGETS anis-paper-scene-engine RUNTIME DESTINATION bin)
endif()
install(PROGRAMS tools/sddm/anispaper-sddm-install
        DESTINATION libexec/anispaper)
install(FILES assets/anispaper/fallback.jpg DESTINATION share/anispaper)
install(TARGETS anispaperframeprovider
    LIBRARY DESTINATION share/plasma/wallpapers/org.anispaper.frame/contents/ui/org/anispaper/frame
    RUNTIME DESTINATION share/plasma/wallpapers/org.anispaper.frame/contents/ui/org/anispaper/frame)
install(FILES
    packaging/plasma/org.anispaper.frame/metadata.desktop
    packaging/plasma/org.anispaper.frame/metadata.json
    DESTINATION share/plasma/wallpapers/org.anispaper.frame)
install(FILES
    packaging/plasma/org.anispaper.frame/contents/config/main.xml
    DESTINATION share/plasma/wallpapers/org.anispaper.frame/contents/config)
install(FILES
    packaging/plasma/org.anispaper.frame/contents/ui/main.qml
    DESTINATION share/plasma/wallpapers/org.anispaper.frame/contents/ui)
install(FILES
    packaging/plasma/org.anispaper.frame/contents/ui/org/anispaper/frame/qmldir
    packaging/plasma/org.anispaper.frame/contents/ui/org/anispaper/frame/FrameBridgeSupport.qml
    DESTINATION share/plasma/wallpapers/org.anispaper.frame/contents/ui/org/anispaper/frame)
configure_file(packaging/systemd/anispaper.service.in anispaper.service @ONLY)
# User units are discovered under $XDG_DATA_HOME/systemd/user.  With
# --prefix ~/.local this expands to ~/.local/share/systemd/user, not lib/.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/anispaper.service DESTINATION share/systemd/user)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME sddm_helper
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sddm_helper_test.py)
  add_executable(anispaper-plasma-login-manager-test
      tests/plasma_login_manager_test.cpp
      src/daemon/plasma_login_manager.h
      src/daemon/plasma_login_manager.cpp)
  target_compile_features(anispaper-plasma-login-manager-test PRIVATE cxx_std_20)
  target_link_libraries(anispaper-plasma-login-manager-test PRIVATE Qt6::Core)
  target_compile_options(anispaper-plasma-login-manager-test PRIVATE -Wall -Wextra -Wpedantic)
  # The repository's FUSE-backed workspace is mounted noexec for direct test
  # binaries.  Running through the native ELF loader preserves the same test
  # process and also works on ordinary executable filesystems.
  find_program(ANISPAPER_ELF_LOADER NAMES ld-linux-x86-64.so.2
      PATHS /lib64 /lib NO_DEFAULT_PATH)
  if(ANISPAPER_ELF_LOADER)
    add_test(NAME plasma_login_manager
        COMMAND ${ANISPAPER_ELF_LOADER} $<TARGET_FILE:anispaper-plasma-login-manager-test>)
  else()
    add_test(NAME plasma_login_manager
        COMMAND $<TARGET_FILE:anispaper-plasma-login-manager-test>)
  endif()
  add_executable(anispaper-static-image-test
      tests/static_image_renderer_test.cpp
      src/renderers/renderer.h
      src/renderers/static_image_renderer.h
      src/renderers/static_image_renderer.cpp)
  target_compile_features(anispaper-static-image-test PRIVATE cxx_std_20)
  target_link_libraries(anispaper-static-image-test PRIVATE Qt6::Core Qt6::Gui)
  target_compile_options(anispaper-static-image-test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME static_image_renderer
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/static_image_renderer_test.py
              $<TARGET_FILE:anispaper-static-image-test>)
  add_executable(anispaper-scene-transport-bridge-test
      tests/scene_transport_bridge_test.cpp)
  # Keep the deterministic mid-copy hook out of the bridge linked by the
  # daemon and Plasma provider.  This test-only library has the identical
  # public ABI, with instrumentation compiled solely for this executable.
  add_library(anispaper-bridge-test STATIC
      src/bridge/frame_protocol.h
      src/bridge/frame_bridge.h
      src/bridge/frame_bridge.cpp)
  target_compile_features(anispaper-bridge-test PUBLIC cxx_std_20)
  target_link_libraries(anispaper-bridge-test PUBLIC Qt6::Core Qt6::Gui)
  target_compile_definitions(anispaper-bridge-test PRIVATE ANISPAPER_BRIDGE_TESTING=1)
  target_compile_options(anispaper-bridge-test PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(anispaper-bridge-test PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_features(anispaper-scene-transport-bridge-test PRIVATE cxx_std_20)
  target_link_libraries(anispaper-scene-transport-bridge-test PRIVATE anispaper-bridge-test Qt6::Core Qt6::Gui)
  target_compile_definitions(anispaper-scene-transport-bridge-test PRIVATE ANISPAPER_BRIDGE_TESTING=1)
  target_compile_options(anispaper-scene-transport-bridge-test PRIVATE -Wall -Wextra -Wpedantic)
  if(ANISPAPER_ELF_LOADER)
    add_test(NAME scene_transport_bridge
        COMMAND ${ANISPAPER_ELF_LOADER} $<TARGET_FILE:anispaper-scene-transport-bridge-test>)
  else()
    add_test(NAME scene_transport_bridge
        COMMAND $<TARGET_FILE:anispaper-scene-transport-bridge-test>)
  endif()
  add_executable(anispaper-isolated-renderer-cleanup-test
      tests/isolated_renderer_cleanup_test.cpp
      src/renderers/renderer.h
      src/renderers/isolated_renderer.h
      src/renderers/isolated_renderer.cpp)
  target_compile_features(anispaper-isolated-renderer-cleanup-test PRIVATE cxx_std_20)
  target_link_libraries(anispaper-isolated-renderer-cleanup-test PRIVATE
      anispaper-bridge Qt6::Core Qt6::Gui)
  target_compile_definitions(anispaper-isolated-renderer-cleanup-test PRIVATE
      ANISPAPER_ISOLATED_RENDERER_TESTING=1)
  target_compile_options(anispaper-isolated-renderer-cleanup-test PRIVATE -Wall -Wextra -Wpedantic)
  if(ANISPAPER_ELF_LOADER)
    add_test(NAME isolated_renderer_cleanup
        COMMAND ${ANISPAPER_ELF_LOADER} $<TARGET_FILE:anispaper-isolated-renderer-cleanup-test>)
  else()
    add_test(NAME isolated_renderer_cleanup
        COMMAND $<TARGET_FILE:anispaper-isolated-renderer-cleanup-test>)
  endif()
  add_executable(anispaper-plasma-wallpaper-activator-test
      tests/plasma_wallpaper_activator_test.cpp
      src/daemon/plasma_wallpaper_activator.h
      src/daemon/plasma_wallpaper_activator.cpp)
  target_compile_features(anispaper-plasma-wallpaper-activator-test PRIVATE cxx_std_20)
  target_link_libraries(anispaper-plasma-wallpaper-activator-test PRIVATE Qt6::Core Qt6::DBus)
  target_compile_options(anispaper-plasma-wallpaper-activator-test PRIVATE -Wall -Wextra -Wpedantic)
  if(ANISPAPER_ELF_LOADER)
    add_test(NAME plasma_wallpaper_activator
        COMMAND ${ANISPAPER_ELF_LOADER} $<TARGET_FILE:anispaper-plasma-wallpaper-activator-test>)
  else()
    add_test(NAME plasma_wallpaper_activator
        COMMAND $<TARGET_FILE:anispaper-plasma-wallpaper-activator-test>)
  endif()
  add_test(NAME f1_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/f1_integration.py $<TARGET_FILE:anis-paperd>)
  set_tests_properties(f1_integration PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME f2_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/f2_integration.py $<TARGET_FILE:anis-paperd>)
  set_tests_properties(f2_integration PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME f2_renderer_child COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/f2_renderer_child.py $<TARGET_FILE:anis-paperd>)
  set_tests_properties(f2_renderer_child PROPERTIES SKIP_RETURN_CODE 77)
  add_executable(anispaper-f3-plugin-smoke tests/f3_plugin_smoke.cpp)
  target_compile_features(anispaper-f3-plugin-smoke PRIVATE cxx_std_20)
  target_link_libraries(anispaper-f3-plugin-smoke PRIVATE anispaper-bridge Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
  target_compile_definitions(anispaper-f3-plugin-smoke PRIVATE
      ANISPAPER_FRAME_QML_IMPORT_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
      ANISPAPER_FRAME_QML_SOURCE_DIR="${ANISPAPER_FRAME_QML_BUILD_DIR}")
  target_compile_options(anispaper-f3-plugin-smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_dependencies(anispaper-f3-plugin-smoke anispaperframeprovider)
  add_test(NAME f3_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/f3_integration.py $<TARGET_FILE:anis-paperd> $<TARGET_FILE:anispaper-f3-plugin-smoke>)
  set_tests_properties(f3_integration PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME f4_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/f4_integration.py $<TARGET_FILE:anis-paperd>)
  set_tests_properties(f4_integration PROPERTIES SKIP_RETURN_CODE 77)
endif()
