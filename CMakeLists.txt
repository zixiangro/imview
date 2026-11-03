cmake_minimum_required(VERSION 3.14)
project(imview VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(IMVIEW_BUILD_TESTS "Build the logic tests" ON)

# Keep the vendored EUI-NEO lean: library only, no demos/modules/tray.
set(EUI_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_USER_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_TEST_FIXTURES OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_TRAY OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_FFMPEG_VIDEO_EXAMPLE OFF CACHE BOOL "" FORCE)

add_subdirectory(3rd/EUI-NEO)

# ImView ships as a single executable. Nothing is read from disk at startup:
# text falls back to the system UI font (Segoe UI, with Microsoft YaHei for CJK),
# so EUI's asset folder is not copied next to the binary. EUI would also embed
# its own application icon; the project has none yet, so that is cleared too.
set_property(GLOBAL PROPERTY EUI_NEO_APP_ASSETS_DIR "")
set_property(GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE "")

configure_file(res/imview.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/imview.rc" @ONLY)

add_executable(imview
    src/app.cpp
    src/stb_wrap.cpp
    src/exif.cpp
    src/window_frame.cpp
    src/shell_actions.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/imview.rc"
)
target_compile_definitions(imview PRIVATE
    IMVIEW_VERSION="${PROJECT_VERSION}"
    IMVIEW_HOMEPAGE="https://github.com/zixiangro/imview"
)
eui_neo_configure_app(imview)

# Logic tests: no window, no EUI — just the helpers that carry format logic.
if(IMVIEW_BUILD_TESTS)
    enable_testing()
    add_executable(imview_tests
        tests/tests.cpp
        src/stb_wrap.cpp
        src/exif.cpp
    )
    target_include_directories(imview_tests PRIVATE src 3rd/EUI-NEO)
    target_compile_definitions(imview_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
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
