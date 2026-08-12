# AnisPaper vendored build of Almamu/linux-wallpaperengine.
# Differences vs upstream (see CMakeLists.txt.orig): library-only target,
# X11/offscreen-only drivers (no layer-shell Wayland windows), no CEF/web
# support (AnisPaper renders web wallpapers with its own Qt WebEngine child),
# no tests and no standalone binary.
cmake_minimum_required(VERSION 3.20)
project(anispaper-wallpaperengine LANGUAGES C CXX)

# Embed scene-script builtins (builtins.js) into a generated header, mirroring upstream.
set(WPENGINE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WPENGINE_BUILTINS_JS "${CMAKE_CURRENT_SOURCE_DIR}/src/WallpaperEngine/Scripting/resources/builtins.js")
file(READ "${WPENGINE_BUILTINS_JS}" WPENGINE_SCRIPT_BUILTINS)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WallpaperEngine/Scripting/resources/Builtins.generated.h.in"
    "${WPENGINE_GENERATED_DIR}/WallpaperEngine/Scripting/Builtins.generated.h"
    @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WPENGINE_BUILTINS_JS}")
include_directories(${WPENGINE_GENERATED_DIR})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules;${CMAKE_MODULE_PATH}")
set(OpenGL_GL_PREFERENCE "LEGACY")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
set(KISSFFT_OPENMP OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_FORCE_PIC ON CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)

find_package(PkgConfig REQUIRED)
# FindOpenGL/FindGLEW/FindGLUT of this CMake version interact badly with the
# X11 cache seeded by the vendored GLFW above, so use pkg-config everywhere.
pkg_check_modules(GL REQUIRED IMPORTED_TARGET gl)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)
pkg_check_modules(GLUT REQUIRED IMPORTED_TARGET glut)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
pkg_check_modules(SDL2_PC REQUIRED IMPORTED_TARGET sdl2)
find_package(MPV REQUIRED)
pkg_check_modules(LZ4_PC REQUIRED IMPORTED_TARGET liblz4)
set(LZ4_LIBRARY ${LZ4_PC_LINK_LIBRARIES})
set(LZ4_INCLUDE_DIR ${LZ4_PC_INCLUDE_DIRS})
find_package(FFMPEG REQUIRED)
find_package(PulseAudio REQUIRED)
find_package(Freetype REQUIRED)
find_package(DBus REQUIRED)
find_package(X11 REQUIRED)

add_subdirectory(src/External/glslang-WallpaperEngine glslang)
add_subdirectory(src/External/SPIRV-Cross-WallpaperEngine spirv-cross)
add_subdirectory(src/External/kissfft kissfft)
add_subdirectory(src/External/argparse argparse)
add_subdirectory(src/External/quickjs quickjs)

file(GLOB_RECURSE WE_SOURCES CONFIGURE_DEPENDS
    "src/Steam/*.cpp"
    "src/WallpaperEngine/*.cpp"
    "src/External/MimeTypes/MimeTypes.cpp"
)

# Excluded from the AnisPaper build:
#  - Testing/            (needs Catch2, unused at runtime)
#  - Wayland drivers     (offscreen child renders through X11/GLFW hidden window)
#  - main.cpp            (replaced by src/scene_engine/main.cpp)
list(FILTER WE_SOURCES EXCLUDE REGEX "/Testing/")
list(FILTER WE_SOURCES EXCLUDE REGEX "/Drivers/Wayland")
list(FILTER WE_SOURCES EXCLUDE REGEX "/Drivers/Output/Wayland")
list(FILTER WE_SOURCES EXCLUDE REGEX "/Drivers/Detectors/Wayland")
list(FILTER WE_SOURCES EXCLUDE REGEX "/Input/Drivers/Wayland")
list(FILTER WE_SOURCES EXCLUDE REGEX "/src/main.cpp")
# CEF-free build: CWeb needs the full CEF headers, which are not vendored.
list(FILTER WE_SOURCES EXCLUDE REGEX "/Render/Wallpapers/CWeb")

add_library(wallpaperengine-core STATIC ${WE_SOURCES})

target_compile_definitions(wallpaperengine-core
    PUBLIC ERRORONLY=0 DEMOMODE=0 ENABLE_X11)
target_compile_options(wallpaperengine-core PRIVATE -Wno-narrowing)

target_include_directories(wallpaperengine-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/External/glslang-WallpaperEngine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/External/SPIRV-Cross-WallpaperEngine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/External/MimeTypes
    ${CMAKE_CURRENT_SOURCE_DIR}/src/External/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/External/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/src/External/kissfft
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MPV_INCLUDE_DIR}
    ${GLEW_INCLUDE_DIRS}
    ${LZ4_INCLUDE_DIR}
    ${FFMPEG_INCLUDE_DIR}
    ${PULSEAUDIO_INCLUDE_DIR}
    ${DBUS_INCLUDE_DIRS}
    ${X11_INCLUDES})

target_link_libraries(wallpaperengine-core PUBLIC
    PkgConfig::GL
    PkgConfig::GLEW
    PkgConfig::GLUT
    PkgConfig::ZLIB
    PkgConfig::SDL2_PC
    ${LZ4_LIBRARY}
    ${FFMPEG_LIBRARIES}
    ${MPV_LIBRARY}
    ${PULSEAUDIO_LIBRARY}
    Freetype::Freetype
    ${X11_LIBRARIES}
    ${X11_Xrandr_LIB}
    ${X11_xf86vmode_LIB}
    ${X11_Xinerama_LIB}
    ${X11_Xcursor_LIB}
    ${X11_Xi_LIB}
    ${DBUS_LIBRARIES}
    kissfft
    glslang
    spirv-cross-core
    spirv-cross-glsl
    glfw
    argparse
    qjs)
