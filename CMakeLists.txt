cmake_minimum_required(VERSION 3.25)
project(falcon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_COMPILER "clang++")

# Generate compile_commands.json for IDEs like VS Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Store FetchContent dependencies in a central cache outside the build folder
set(FETCHCONTENT_BASE_DIR "${CMAKE_HOME_DIRECTORY}/.cache" CACHE PATH "")
# Prevent compiler-specific flags from being automatically appended to shared library link commands
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

include(FetchContent)

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.5.1.zip
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
)

FetchContent_Declare(
    glad
    URL https://github.com/Dav1dde/glad/archive/refs/tags/v2.0.8.zip
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

FetchContent_MakeAvailable(glad)

add_subdirectory("${glad_SOURCE_DIR}/cmake" glad_cmake)

glad_add_library(glad_gl_core_46 STATIC API gl:core=4.6)

target_link_libraries(imgui PUBLIC
    glfw
    OpenGL::GL
    glad_gl_core_46
)

# add_executable(falcon
#     src/main.cpp
# )

add_executable(falcon
    src/pipelines/filter_comparison_pipeline.cpp
)

target_compile_options(falcon PRIVATE
    -mavx2
    -mfma
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
    -Werror
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
    uring
)

# Profile builds also write the disassembly of falcon's own code to <build dir>/output/falcon.S.
# GNU objdump, not llvm-objdump: Disassembly Explorer can't read llvm-objdump's "; file:line" markers.
if(CMAKE_BUILD_TYPE STREQUAL "Profile")
    find_program(GNU_OBJDUMP NAMES objdump REQUIRED)
    set(FALCON_DISASSEMBLY_FILE "${CMAKE_BINARY_DIR}/output/falcon.S")
    cmake_path(GET FALCON_DISASSEMBLY_FILE PARENT_PATH FALCON_DISASSEMBLY_DIRECTORY)
    add_custom_command(
        OUTPUT ${FALCON_DISASSEMBLY_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${FALCON_DISASSEMBLY_DIRECTORY}
        COMMAND ${GNU_OBJDUMP} -d -l -C -M intel --insn-width=15 $<TARGET_OBJECTS:falcon> > ${FALCON_DISASSEMBLY_FILE}
        DEPENDS falcon
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Disassembling falcon into ${FALCON_DISASSEMBLY_FILE}"
    )
    add_custom_target(falcon_disassembly ALL DEPENDS ${FALCON_DISASSEMBLY_FILE})
endif()
