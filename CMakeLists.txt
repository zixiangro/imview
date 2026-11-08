cmake_minimum_required(VERSION 3.14)
project(imview VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(IMVIEW_BUILD_TESTS "Build the logic tests" ON)
option(IMVIEW_ENABLE_VIDEO "Enable video playback and video-related integration" ON)
if(IMVIEW_ENABLE_VIDEO)
    set(IMVIEW_ENABLE_VIDEO_VALUE 1)
else()
    set(IMVIEW_ENABLE_VIDEO_VALUE 0)
endif()

# Minimal static FFmpeg build used for video playback. Build it from the
# 3rd/ffmpeg submodule first (see doc/build.md); the default location is inside
# the ignored build tree so a clean rebuild can regenerate it.
set(IMVIEW_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg-build" CACHE PATH
    "Static FFmpeg build directory (libavcodec, libavformat, libavutil, libswscale, libswresample)")
if(IMVIEW_ENABLE_VIDEO AND
   (NOT EXISTS "${IMVIEW_FFMPEG_ROOT}/libavcodec/avcodec.lib" OR
    NOT EXISTS "${IMVIEW_FFMPEG_ROOT}/libavformat/avformat.lib" OR
    NOT EXISTS "${IMVIEW_FFMPEG_ROOT}/libavutil/avutil.lib" OR
    NOT EXISTS "${IMVIEW_FFMPEG_ROOT}/libswscale/swscale.lib" OR
    NOT EXISTS "${IMVIEW_FFMPEG_ROOT}/libswresample/swresample.lib"))
    message(FATAL_ERROR
        "FFmpeg static libraries were not found in ${IMVIEW_FFMPEG_ROOT}.\n"
        "Run tools\\build_ffmpeg.cmd first (it reconfigures when the audio "
        "libraries are missing), set IMVIEW_FFMPEG_ROOT to an existing build, "
        "or configure with -DIMVIEW_ENABLE_VIDEO=OFF (see doc/build.md).")
endif()

# Keep the vendored EUI-NEO lean: library only, no demos/modules/tray.
set(EUI_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_USER_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_TEST_FIXTURES OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_TRAY OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_FFMPEG_VIDEO_EXAMPLE OFF CACHE BOOL "" FORCE)

add_subdirectory(3rd/EUI-NEO)

# ImView ships as the main executable; the small Explorer context-menu DLL is
# embedded as a resource. Nothing else is read at startup: text falls back to the system UI font (Segoe
# UI, with Microsoft YaHei for CJK), so EUI's asset folder is not copied next to
# the binary. EUI would also embed its own application icon; the project has none
# yet, so that is cleared too.
set_property(GLOBAL PROPERTY EUI_NEO_APP_ASSETS_DIR "")
set_property(GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE "")

configure_file(res/imview.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/imview.rc" @ONLY)

add_executable(imview
    src/app.cpp
    src/stb_wrap.cpp
    src/exif.cpp
    src/media_paths.cpp
    src/window_frame.cpp
    src/shell_actions.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/imview.rc"
)
if(IMVIEW_ENABLE_VIDEO)
    target_sources(imview PRIVATE src/video_player.cpp)
else()
    target_sources(imview PRIVATE src/video_player_stub.cpp)
endif()
target_compile_definitions(imview PRIVATE
    IMVIEW_VERSION="${PROJECT_VERSION}"
    IMVIEW_HOMEPAGE="https://github.com/zixiangro/imview"
    IMVIEW_ENABLE_VIDEO=${IMVIEW_ENABLE_VIDEO_VALUE}
)
target_include_directories(imview PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/3rd/EUI-NEO/3rd"
)
if(IMVIEW_ENABLE_VIDEO)
    target_include_directories(imview PRIVATE
        "${IMVIEW_FFMPEG_ROOT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rd/ffmpeg"
    )
    target_link_directories(imview PRIVATE
        "${IMVIEW_FFMPEG_ROOT}/libavformat"
        "${IMVIEW_FFMPEG_ROOT}/libavcodec"
        "${IMVIEW_FFMPEG_ROOT}/libavutil"
        "${IMVIEW_FFMPEG_ROOT}/libswscale"
        "${IMVIEW_FFMPEG_ROOT}/libswresample"
    )
    target_link_libraries(imview PRIVATE avformat avcodec swscale swresample avutil)
endif()
target_link_libraries(imview PRIVATE ole32 user32 bcrypt advapi32)
eui_neo_configure_app(imview)

# Explorer sees the complete multi-selection only inside a COM context-menu
# handler, so this small DLL implements IShellExtInit/IContextMenu. The DLL is
# embedded into imview.exe as RCDATA; the About switch extracts it to
# %LOCALAPPDATA%\ImView and removes it again when switched off. It only launches
# imview.exe with --new-window.
add_library(imview_shell SHARED
    src/shell_menu.cpp
    src/media_paths.cpp
)
target_include_directories(imview_shell PRIVATE src)
target_compile_definitions(imview_shell PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
    IMVIEW_ENABLE_VIDEO=${IMVIEW_ENABLE_VIDEO_VALUE}
)
target_link_libraries(imview_shell PRIVATE ole32 shell32 shlwapi advapi32)
set_target_properties(imview_shell PROPERTIES
    PREFIX ""
    OUTPUT_NAME "imview_shell"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
)

# Keep the main executable in the same place for both single- and multi-config
# generators; the resource below refers to the built DLL by path.
set_target_properties(imview PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
)
set(IMVIEW_SHELL_DLL_PATH "${CMAKE_BINARY_DIR}/imview_shell.dll")
configure_file(res/imview_shell.rc.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/imview_shell.rc" @ONLY)
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/generated/imview_shell.rc"
    PROPERTIES OBJECT_DEPENDS "${IMVIEW_SHELL_DLL_PATH}"
)
target_sources(imview PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/imview_shell.rc")
add_dependencies(imview imview_shell)

# Logic tests: no window, no EUI — just the helpers that carry format logic.
if(IMVIEW_BUILD_TESTS)
    enable_testing()
    add_executable(imview_tests
        tests/tests.cpp
        src/stb_wrap.cpp
        src/exif.cpp
        src/media_paths.cpp
    )
    target_include_directories(imview_tests PRIVATE src 3rd/EUI-NEO)
    target_compile_definitions(imview_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN
        IMVIEW_ENABLE_VIDEO=${IMVIEW_ENABLE_VIDEO_VALUE}
    )
    add_test(NAME logic COMMAND imview_tests)
endif()

# `cmake --install` produces a release-ready folder: the binary plus the licence
# texts that must travel with it (see doc/release.md).
install(TARGETS imview RUNTIME DESTINATION .)
install(FILES LICENSE 3rd/THIRD-PARTY-NOTICES.md DESTINATION .)
install(FILES 3rd/EUI-NEO/LICENSE
        DESTINATION licenses RENAME EUI-NEO-Apache-2.0.txt)
install(FILES 3rd/EUI-NEO/3rd/glfw/LICENSE.md
        DESTINATION licenses RENAME GLFW-zlib.txt)
install(FILES 3rd/EUI-NEO/3rd/freetype/LICENSE.TXT
        DESTINATION licenses RENAME FreeType-FTL.txt)
install(FILES 3rd/EUI-NEO/3rd/libpng-1.6.43/LICENSE
        DESTINATION licenses RENAME libpng.txt)
install(FILES 3rd/EUI-NEO/3rd/zlib-1.3.1/LICENSE
        DESTINATION licenses RENAME zlib.txt)
install(FILES 3rd/EUI-NEO/3rd/md4c/LICENSE.md
        DESTINATION licenses RENAME md4c-MIT.txt)
install(FILES 3rd/ffmpeg/COPYING.LGPLv2.1
        DESTINATION licenses RENAME FFmpeg-LGPL-2.1.txt)
