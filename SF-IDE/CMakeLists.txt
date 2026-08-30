cmake_minimum_required(VERSION 3.20)
project(SF-IDE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

# ImGui (dx11 + win32 + docking) is provided by the vcpkg manifest.
find_package(ImGui CONFIG REQUIRED)

# Locate the vcpkg ImGui sources so the Win32 + DX11 backends can be compiled
# into SF-IDE itself (vcpkg ships only imgui core in the static library).
get_target_property(IMGUI_INCLUDE_DIR imgui::imgui INTERFACE_INCLUDE_DIRECTORIES)
if(NOT IMGUI_INCLUDE_DIR)
    message(FATAL_ERROR "imgui::imgui target does not expose an include directory")
endif()
# ImGui backend implementations live in src/imgui_backends (kept locally so we
# do not depend on vcpkg internal buildtree paths).
set(IMGUI_BACKEND_SOURCES
    src/imgui_backends/imgui_impl_win32.cpp
    src/imgui_backends/imgui_impl_dx11.cpp)

foreach(_src ${IMGUI_BACKEND_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        message(FATAL_ERROR "Expected ImGui backend source not found: ${_src}")
    endif()
endforeach()

add_executable(SF-IDE WIN32
    src/main.cpp
    src/Editor.cpp
    ${IMGUI_BACKEND_SOURCES}
)

target_include_directories(SF-IDE PRIVATE src)

target_link_libraries(SF-IDE PRIVATE
    imgui::imgui
    d3d11
    dxgi
)

if(MSVC)
    target_link_libraries(SF-IDE PRIVATE user32 gdi32)
    # Treat the C runtime header files as the system header set so that MSVC
    # does not warn about standard library usage we cannot change.
    target_compile_options(SF-IDE PRIVATE /W4 /permissive- /utf-8)
endif()

option(SFIDE_BUILD_TESTS "Build SF-IDE unit tests" OFF)
if(SFIDE_BUILD_TESTS)
    add_executable(test_editor tests/test_editor.cpp src/Editor.cpp)
    target_include_directories(test_editor PRIVATE src)
endif()
