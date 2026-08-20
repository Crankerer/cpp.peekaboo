cmake_minimum_required(VERSION 3.24)

# One number, counted up per build. CI passes its run number; a local build that
# says nothing is version 0, which is how you tell the two apart.
set(PEEKABOO_VERSION_MAJOR "0" CACHE STRING "Build number stamped into the executable")
set(PEEKABOO_VERSION "${PEEKABOO_VERSION_MAJOR}.0.0")

project(PeekaBoo VERSION ${PEEKABOO_VERSION} LANGUAGES CXX)

if(WIN32)
    enable_language(RC)  # the icon and the version stamp
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)

FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d
    SOURCE_SUBDIR do-not-build)  # header only, skip its own targets

FetchContent_MakeAvailable(glfw imgui stb miniaudio)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_executable(peekaboo
    src/main.cpp
    src/media.cpp
    src/overlay.cpp
    src/pdf.cpp
    src/preview.cpp
    src/shell.cpp
    src/startup.cpp)

if(WIN32)
    set(PEEKABOO_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/peekaboo.ico")
    configure_file(src/peekaboo.rc.in "${CMAKE_CURRENT_BINARY_DIR}/peekaboo.rc" @ONLY)

    # The resource lives in its own target on purpose: windres hands the
    # compiling target's include paths to its preprocessor unquoted, so a space
    # in any of them fails the build. Here there are none to hand over.
    add_library(peekaboo_icon OBJECT "${CMAKE_CURRENT_BINARY_DIR}/peekaboo.rc")
    target_link_libraries(peekaboo PRIVATE peekaboo_icon)
endif()

target_compile_definitions(peekaboo PRIVATE PEEKABOO_VERSION="${PEEKABOO_VERSION}")
set_target_properties(peekaboo PROPERTIES WIN32_EXECUTABLE TRUE)
target_include_directories(peekaboo PRIVATE ${stb_SOURCE_DIR} ${miniaudio_SOURCE_DIR})
target_link_libraries(peekaboo PRIVATE imgui glfw OpenGL::GL)

if(WIN32)
    # NOMINMAX for every translation unit, not just the ones that remember to
    # ask: windows.h defines min and max as macros, and the first std::min it
    # reaches turns into a syntax error.
    target_compile_definitions(peekaboo PRIVATE UNICODE _UNICODE NOMINMAX)
    # runtimeobject and shcore are what Windows.Data.Pdf needs: the WinRT
    # activation core, and the IStream to IRandomAccessStream bridge.
    target_link_libraries(peekaboo PRIVATE advapi32 dwmapi mfplat mfreadwrite mfuuid ole32 oleaut32 runtimeobject
                                       shcore shell32 shlwapi uuid windowscodecs winmm)
endif()

if(MSVC)  # keep int main() with the Windows subsystem
    target_link_options(peekaboo PRIVATE /ENTRY:mainCRTStartup)
endif()

if(MINGW)
    # Without this the executable wants libstdc++-6.dll, libgcc_s_seh-1.dll and
    # libwinpthread-1.dll beside it, which is no way to hand someone a tray tool.
    target_link_options(peekaboo PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(MSVC)
    target_compile_options(peekaboo PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(peekaboo PRIVATE -Wall -Wextra)
endif()
