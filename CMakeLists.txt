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

add_custom_command(
    OUTPUT ${PROBE_PROTOCOL_HEADER} ${PROBE_PROTOCOL_CODE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROBE_GENERATED_DIR}
    COMMAND ${WAYLAND_SCANNER} client-header ${PROBE_PROTOCOL} ${PROBE_PROTOCOL_HEADER}
    COMMAND ${WAYLAND_SCANNER} private-code ${PROBE_PROTOCOL} ${PROBE_PROTOCOL_CODE}
    DEPENDS ${PROBE_PROTOCOL}
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

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network Gui OpenGL Widgets WebEngineCore WebEngineWidgets Quick Qml)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
set(CMAKE_AUTOMOC ON)

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

install(TARGETS anis-paperd RUNTIME DESTINATION bin)
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
