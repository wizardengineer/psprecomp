cmake_minimum_required(VERSION 3.20)
project(psprecomp_runtime CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find SDL2 via pkg-config (Homebrew installs pkg-config file)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# Find OpenGL
find_package(OpenGL REQUIRED)

# Find zlib (system zlib on macOS; pkg-config-discoverable on Linux)
find_package(ZLIB REQUIRED)

# Build the recompiler output as a subdirectory
# output/ contains its own CMakeLists.txt producing libboot_recomp.a
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../output ${CMAKE_CURRENT_BINARY_DIR}/recomp_build)

# Runtime sources
set(RUNTIME_SOURCES
    src/main.cpp
    src/asset_bnd.cpp
    src/psp_memory.cpp
    src/psp_dispatch.cpp
    src/psp_scheduler.cpp
    src/psp_render_queue.cpp
    src/psp_event_loop.cpp
    src/psp_ge.cpp
    src/psp_ge_vertex.cpp
    src/psp_ge_texture.cpp
    src/psp_ge_shader.cpp
    src/psp_ge_draw.cpp
    src/psp_ge_test.cpp
    src/hle/psp_hle_dispatch.cpp
    src/hle/psp_hle_kernel_thread.cpp
    src/hle/psp_hle_kernel_memory.cpp
    src/hle/psp_hle_kernel_sema.cpp
    src/hle/psp_hle_kernel_mutex.cpp
    src/hle/psp_hle_kernel_eventflag.cpp
    src/hle/psp_hle_io.cpp
    src/hle/psp_hle_display.cpp
    src/hle/psp_hle_ge.cpp
    src/hle/psp_hle_power.cpp
    src/hle/psp_hle_ctrl.cpp
    src/hle/psp_hle_utility.cpp
    src/hle/psp_hle_sas.cpp
    src/psp_vfpu_core.cpp
    src/psp_vfpu_arith.cpp
    src/psp_vfpu_trig.cpp
    src/psp_vfpu_matrix.cpp
    src/psp_vfpu_mem.cpp
    src/psp_vfpu_convert.cpp
    src/psp_vfpu_misc.cpp
    src/glad.c
    src/psp_debug_socket.cpp
)

add_executable(psprecomp_runtime ${RUNTIME_SOURCES})

target_include_directories(psprecomp_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../output/include  # recomp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../output           # funcs.h
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(psprecomp_runtime PRIVATE
    boot_recomp
    PkgConfig::SDL2
    OpenGL::GL
    ZLIB::ZLIB
    pthread
)

# Suppress OpenGL deprecation warnings on macOS
target_compile_definitions(psprecomp_runtime PRIVATE
    GL_SILENCE_DEPRECATION
)

target_compile_options(psprecomp_runtime PRIVATE
    $<IF:$<CONFIG:Debug>,-O0,-O2>
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# VFPU unit test target (links only VFPU sources, no SDL2/GL dependency)
set(VFPU_TEST_SOURCES
    tests/test_vfpu.cpp
    src/psp_vfpu_core.cpp
    src/psp_vfpu_arith.cpp
    src/psp_vfpu_trig.cpp
    src/psp_vfpu_matrix.cpp
    src/psp_vfpu_mem.cpp
    src/psp_vfpu_convert.cpp
    src/psp_vfpu_misc.cpp
)

add_executable(test_vfpu ${VFPU_TEST_SOURCES})

target_include_directories(test_vfpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../output/include  # recomp.h
)

target_compile_options(test_vfpu PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_compile_definitions(test_vfpu PRIVATE
    GL_SILENCE_DEPRECATION
)

enable_testing()
add_test(NAME vfpu_tests COMMAND test_vfpu)

# Debug socket v2 unit test target (issue #35). Links ONLY the socket
# layer -- the I/S commands reach game state through the PspDebugHooks
# seams, so no SDL2/GL/recomp dependency is needed.
set(DEBUG_SOCKET_TEST_SOURCES
    tests/test_debug_socket.cpp
    src/psp_debug_socket.cpp
)

add_executable(test_debug_socket ${DEBUG_SOCKET_TEST_SOURCES})

target_include_directories(test_debug_socket PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(test_debug_socket PRIVATE
    pthread
)

target_compile_options(test_debug_socket PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME debug_socket_tests COMMAND test_debug_socket)

# BND parser unit test target (mirrors test_vfpu convention: no SDL2/GL,
# links the parser plus psp_memory + system zlib only). The test source
# `tests/test_asset_bnd.cpp` is created in Plan 11-06; the EXISTS guard
# leaves the target inactive until then so this plan's build stays green.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_asset_bnd.cpp)
    set(BND_TEST_SOURCES
        tests/test_asset_bnd.cpp
        src/asset_bnd.cpp
        src/psp_memory.cpp
    )

    add_executable(test_asset_bnd ${BND_TEST_SOURCES})

    target_include_directories(test_asset_bnd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../output/include  # recomp.h
    )

    target_link_libraries(test_asset_bnd PRIVATE
        ZLIB::ZLIB
        pthread
    )

    target_compile_options(test_asset_bnd PRIVATE
        -O2
        -Wall
        -Wextra
        -Wno-unused-parameter
    )

    target_compile_definitions(test_asset_bnd PRIVATE
        GL_SILENCE_DEPRECATION
    )

    add_test(NAME bnd_tests COMMAND test_asset_bnd)
endif()
