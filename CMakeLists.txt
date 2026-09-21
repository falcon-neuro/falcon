cmake_minimum_required(VERSION 3.25)
project(falcon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_COMPILER "clang++")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(FETCHCONTENT_BASE_DIR "${CMAKE_HOME_DIRECTORY}/.cache" CACHE PATH "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    file(WRITE "${CMAKE_BINARY_DIR}/debug/.lldbinit" "settings set target.memory-module-load-level partial\n")
endif()

include(FetchContent)

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.5.1.zip
)
set(GLFW_BUILD_DOCS OFF CACHE INTERNAL "")
set(GLFW_BUILD_TESTS OFF CACHE INTERNAL "")
set(GLFW_BUILD_EXAMPLES OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
)
FetchContent_MakeAvailable(imgui)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC
    glfw
    OpenGL::GL
)

add_executable(falcon
    src/main.cpp
)

target_compile_options(falcon PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -ftrivial-auto-var-init=pattern
    -stdlib=libc++
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<CONFIG:Profile>:-O3 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -march=native>
    $<$<CONFIG:Release>:-O3 -march=native -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer>
)

target_link_options(falcon PRIVATE
    -stdlib=libc++
    -fuse-ld=lld
)

target_compile_definitions(falcon PRIVATE
    _GLIBCXX_ASSERTIONS=1
    LIBCXX_ENABLE_HARDENING=1
)

target_include_directories(falcon PRIVATE src)

target_link_libraries(falcon PRIVATE
    imgui
    pthread
    rt
)
