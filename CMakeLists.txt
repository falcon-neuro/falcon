cmake_minimum_required(VERSION 3.25)
# Falcon's version, set by hand before a release: the release's date as year, month and day. The
# release's tag is this with a v in front.
project(falcon VERSION 26.11.21 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The system's own GCC is older, and it is what CMake finds when it is run without scripts/cmake.sh.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR
        "Falcon needs GCC 16 or newer, and this is GCC ${CMAKE_CXX_COMPILER_VERSION}. "
        "Fetch it with scripts/setup/fetch_gcc.sh and configure with scripts/cmake.sh.")
endif()

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

add_executable(falcon
    src/main.cpp
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
    # A multiply and an add fuse into one instruction only inside one expression, whichever
    # compiler builds Falcon. GCC's own default also fuses across expressions, which rounds
    # differently from what a reference script computes.
    -ffp-contract=on
    # GCC warns wherever a SIMD vector type such as __m512 is a template argument, e.g. in a
    # std::array of vectors, because it drops there the attribute that lets a pointer of that type
    # read memory of any type. Falcon passes vectors by value and reads memory through intrinsics
    # on float*, so it never relies on that attribute.
    $<$<CXX_COMPILER_ID:GNU>:-Wno-ignored-attributes>
    $<$<CXX_COMPILER_ID:GNU>:-freflection>
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<CONFIG:Profile>:-O3 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -march=native>
    $<$<CONFIG:Release>:-O3 -march=native -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer>
)

target_link_options(falcon PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>
    $<$<CXX_COMPILER_ID:Clang>:-fuse-ld=lld>
)

# The fetched GCC (scripts/setup/fetch_gcc.sh) is newer than the system's, and so is its C++
# library, which sits in its own folder: the program is told where, or it would load the
# system's older one and not start.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH FALCON_GCC_BIN_DIRECTORY)
    cmake_path(SET FALCON_GCC_LIBRARY_DIRECTORY NORMALIZE
               "${FALCON_GCC_BIN_DIRECTORY}/../lib/x86_64-linux-gnu")
    target_link_options(falcon PRIVATE "-Wl,-rpath,${FALCON_GCC_LIBRARY_DIRECTORY}")
endif()

target_compile_definitions(falcon PRIVATE
    _GLIBCXX_ASSERTIONS=1
    LIBCXX_ENABLE_HARDENING=1
    FALCON_VERSION="v${PROJECT_VERSION}"
)

target_include_directories(falcon PRIVATE src)

# The list of processors for the UI and the generator, read from the processors' source by
# src/ui/pipeline_designer/list_processors.py. It is made again when a processor is added or changed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Always clang, whichever compiler builds Falcon: the script reads the syntax tree that clang prints.
find_program(FALCON_CLANG NAMES clang++ REQUIRED)
file(GLOB FALCON_PROCESSOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/processors/*.cpp)
file(GLOB FALCON_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/*.cpp)
set(FALCON_ALL_PROCESSORS_FILE "${CMAKE_BINARY_DIR}/generated/all_processors.cpp")
add_custom_command(
    OUTPUT ${FALCON_ALL_PROCESSORS_FILE}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ui/pipeline_designer/list_processors.py
            --clang ${FALCON_CLANG}
            --source-root ${CMAKE_SOURCE_DIR}/src
            --output ${FALCON_ALL_PROCESSORS_FILE}
            ${FALCON_PROCESSOR_SOURCES}
            -- -std=c++26 -stdlib=libc++ -mavx2 -mfma -march=native -I ${CMAKE_SOURCE_DIR}/src
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/pipeline_designer/list_processors.py
            ${FALCON_PROCESSOR_SOURCES}
            ${FALCON_ENGINE_SOURCES}
    VERBATIM
    COMMENT "Listing the processors into ${FALCON_ALL_PROCESSORS_FILE}"
)
add_custom_target(falcon_all_processors DEPENDS ${FALCON_ALL_PROCESSORS_FILE})
add_dependencies(falcon falcon_all_processors)
target_include_directories(falcon PRIVATE ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(falcon PRIVATE
    imgui
    pthread
    rt
    uring
)

# Each extension may bring a CMakeLists.txt that adds its own include directories and libraries.
file(GLOB FALCON_EXTENSION_CMAKE_FILES ${CMAKE_SOURCE_DIR}/src/processors/custom/*/CMakeLists.txt)
foreach(FALCON_EXTENSION_CMAKE_FILE ${FALCON_EXTENSION_CMAKE_FILES})
    cmake_path(GET FALCON_EXTENSION_CMAKE_FILE PARENT_PATH FALCON_EXTENSION_DIRECTORY)
    add_subdirectory(${FALCON_EXTENSION_DIRECTORY})
endforeach()

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
