cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(SF-IDE VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SFIDE_BUILD_CLIL "Build the bundled clil agent from external/clil" ON)
option(SFIDE_BUILD_TESTS "Build the unit tests" ON)

# --- GLFW: system package if present, otherwise fetched -----------------------
# A universal (arm64 + x86_64) build needs a universal GLFW, so fetch it then.
list(LENGTH CMAKE_OSX_ARCHITECTURES SFIDE_ARCH_COUNT)
if(SFIDE_ARCH_COUNT LESS 2)
    find_package(glfw3 3.3 QUIET)
endif()
if(NOT glfw3_FOUND)
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
endif()
find_package(OpenGL REQUIRED)

# --- Dear ImGui (vendored in third_party/imgui) --------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS GL_SILENCE_DEPRECATION)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- IDE core (no GUI dependencies beyond ImGui; shared with the tests) --------
add_library(sfide_core STATIC
    src/Document.cpp
    src/Syntax.cpp
    src/Vt.cpp
    src/Pty.cpp
    src/BuildRunner.cpp
    src/Util.cpp)
target_include_directories(sfide_core PUBLIC src)
if(APPLE)
    target_link_libraries(sfide_core PUBLIC)
else()
    target_link_libraries(sfide_core PUBLIC util pthread)
endif()

set(SFIDE_SOURCES
    src/main.cpp
    src/App.cpp
    src/EditorView.cpp
    src/TerminalView.cpp
    src/FileTree.cpp
    src/Theme.cpp)

if(APPLE)
    set(MACOSX_BUNDLE_BUNDLE_NAME "SF-IDE")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "nl.silfopma.sf-ide")
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_ICON_FILE "SF-IDE.icns")
    set(ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/SF-IDE.icns)
    if(EXISTS ${ICON})
        set_source_files_properties(${ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        list(APPEND SFIDE_SOURCES ${ICON})
    endif()
    add_executable(SF-IDE MACOSX_BUNDLE ${SFIDE_SOURCES})
    set_target_properties(SF-IDE PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
        XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)
else()
    add_executable(SF-IDE ${SFIDE_SOURCES})
endif()
target_link_libraries(SF-IDE PRIVATE sfide_core imgui)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sfide_core PRIVATE -Wall -Wextra)
    target_compile_options(SF-IDE PRIVATE -Wall -Wextra)
endif()

# --- clil (bundled; see external/clil) --------------------------------------------
if(SFIDE_BUILD_CLIL)
    set(CLIL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/clil)
    if(EXISTS ${CLIL_DIR}/src/main.cpp)
        file(GLOB CLIL_CXX ${CLIL_DIR}/src/*.cpp)
        file(GLOB CLIL_C ${CLIL_DIR}/src/*.c)
        add_executable(clil ${CLIL_CXX} ${CLIL_C})
        # Same flags as clil's own Makefile: C++17 without exceptions / RTTI.
        set_target_properties(clil PROPERTIES CXX_STANDARD 17 C_STANDARD 11)
        target_compile_options(clil PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
        target_compile_definitions(clil PRIVATE $<$<COMPILE_LANGUAGE:C>:_GNU_SOURCE>)
        add_dependencies(SF-IDE clil)
        # clil sits next to the IDE binary (inside SF-IDE.app/Contents/MacOS
        # on macOS), so the app is self-contained.
        add_custom_command(TARGET SF-IDE POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:clil> $<TARGET_FILE_DIR:SF-IDE>/clil
            COMMENT "Bundling clil")
    else()
        message(WARNING "external/clil is empty: run `git submodule update --init` to bundle clil. "
                        "SF-IDE will look for clil on PATH instead.")
    endif()
endif()

# --- tests -------------------------------------------------------------------------
if(SFIDE_BUILD_TESTS)
    enable_testing()
    add_executable(sfide_tests tests/tests.cpp)
    target_link_libraries(sfide_tests PRIVATE sfide_core)
    add_test(NAME sfide_tests COMMAND sfide_tests)
endif()

install(TARGETS SF-IDE BUNDLE DESTINATION . RUNTIME DESTINATION bin)
if(TARGET clil AND NOT APPLE)
    install(TARGETS clil RUNTIME DESTINATION bin)
endif()
