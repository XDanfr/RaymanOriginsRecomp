cmake_minimum_required(VERSION 3.20)
project(RaymanPort CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(RAYMAN_PPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/recomp/ppc CACHE PATH "Generated recompiled PPC sources")
set(PPC_DIR ${RAYMAN_PPC_DIR})

if (RAYMAN_RECOMP_SWITCH)
    file(GLOB RAYMAN_PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp)

    add_library(RaymanRecompLib STATIC
        ${RAYMAN_PPC_SOURCES}
        ${PPC_DIR}/ppc_func_mapping.cpp
    )

    target_include_directories(RaymanRecompLib PUBLIC
        ${PPC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
    )

    target_compile_options(RaymanRecompLib PRIVATE
        -O2
        -fno-strict-aliasing
        -w
        $<$<COMPILE_LANGUAGE:CXX>:-include>
        $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/runtime/cpu/ppc_overrides.h>
    )

    add_library(RaymanSwitchRuntimeCore STATIC
        runtime/memory.cpp
        runtime/loader.cpp
        runtime/xex_lzx.cpp
        runtime/kernel/stub_log.cpp
        runtime/kernel/heap.cpp
        runtime/kernel/page_heap.cpp
        runtime/kernel/memory_imports.cpp
        runtime/kernel/objects.cpp
        runtime/kernel/thread.cpp
        runtime/kernel/sync_imports.cpp
        runtime/kernel/system_imports.cpp
        runtime/kernel/guest_printf.cpp
        runtime/kernel/file_system.cpp
        runtime/kernel/xam_imports.cpp
        runtime/kernel/net_imports.cpp
        runtime/kernel/video_imports.cpp
        runtime/gpu/command_processor.cpp
        runtime/apu/audio.cpp
        runtime/switch/platform.cpp
        runtime/switch/guest_bootstrap.cpp
        runtime/switch/recomp_stubs.cpp
        tools/XenonRecomp/XenonUtils/image.cpp
        tools/XenonRecomp/XenonUtils/xex.cpp
        tools/XenonRecomp/thirdparty/libmspack/libmspack/mspack/lzxd.c
        tools/XenonRecomp/thirdparty/tiny-AES-c/aes.c
        thirdparty/o1heap/o1heap/o1heap.c
    )

    target_include_directories(RaymanSwitchRuntimeCore PRIVATE
        runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/kernel
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap/o1heap
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/XenonUtils
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/libmspack/libmspack/mspack
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/tiny-AES-c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/TinySHA1
    )

    target_compile_options(RaymanSwitchRuntimeCore PRIVATE
        -O1
        -fno-strict-aliasing
        -w
    )

    target_link_libraries(RaymanSwitchRuntimeCore PRIVATE RaymanRecompLib)

    # The generated mapping references every XEX import, including imports that
    # are intentionally unsupported during bring-up.  Keep the same weak
    # fallback translation unit as the desktop target, but archive it so it is
    # considered only after the real runtime hooks have been extracted.
    set(RAYMAN_IMPORT_STUBS ${CMAKE_CURRENT_BINARY_DIR}/generated/import_stubs.cpp)
    add_custom_command(
        OUTPUT ${RAYMAN_IMPORT_STUBS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_import_stubs.py ${PPC_DIR}/ppc_func_mapping.cpp ${RAYMAN_IMPORT_STUBS}
        DEPENDS ${PPC_DIR}/ppc_func_mapping.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_import_stubs.py
    )
    add_library(RaymanImportFallbacks STATIC ${RAYMAN_IMPORT_STUBS})
    target_include_directories(RaymanImportFallbacks PRIVATE
        runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/kernel
        ${PPC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
    )

    add_executable(rayman_switch
        runtime/switch/main.cpp
    )
    target_include_directories(rayman_switch PRIVATE
        runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/kernel
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/XenonUtils
    )

    # RaymanRecompLib needs the runtime import hooks, while the runtime's
    # memory code needs the generated function map.  Put the archives inside
    # one linker rescan group, rather than emitting an empty group as options.
    target_link_libraries(rayman_switch PRIVATE
        "-Wl,--start-group"
        RaymanSwitchRuntimeCore
        RaymanRecompLib
        "-Wl,--end-group"
        # If a regenerated image introduces an import without a real hook,
        # extract its weak fallback only after the primary runtime/recomp
        # cycle.  The second group resolves LogImportStub from the runtime.
        "-Wl,--start-group"
        RaymanImportFallbacks
        RaymanSwitchRuntimeCore
        "-Wl,--end-group"
        nx
    )

    find_program(RAYMAN_ELF2NRO elf2nro
        HINTS
            "${DEVKITPRO}/tools/bin"
            "$ENV{DEVKITPRO}/tools/bin"
        REQUIRED
    )
    find_program(RAYMAN_NACPTOOL nacptool
        HINTS
            "${DEVKITPRO}/tools/bin"
            "$ENV{DEVKITPRO}/tools/bin"
        REQUIRED
    )

    set(RAYMAN_NRO "${CMAKE_CURRENT_BINARY_DIR}/RaymanOrigins.nro")
    set(RAYMAN_NACP "${CMAKE_CURRENT_BINARY_DIR}/RaymanOrigins.nacp")
    set(RAYMAN_NRO_ICON "${DEVKITPRO}/libnx/default_icon.jpg")

    add_custom_command(
        OUTPUT ${RAYMAN_NACP}
        COMMAND ${RAYMAN_NACPTOOL} --create
            "Rayman Origins Recomp"
            "Rayman Origins Recomp"
            "0.1.0"
            ${RAYMAN_NACP}
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${RAYMAN_NRO}
        COMMAND ${CMAKE_COMMAND} -E echo "Packaging $<TARGET_FILE:rayman_switch> -> ${RAYMAN_NRO}"
        COMMAND ${RAYMAN_ELF2NRO} $<TARGET_FILE:rayman_switch> ${RAYMAN_NRO}
            --nacp=${RAYMAN_NACP}
            --icon=${RAYMAN_NRO_ICON}
        DEPENDS rayman_switch ${RAYMAN_NACP}
        VERBATIM
    )

    add_custom_target(rayman_nro ALL DEPENDS ${RAYMAN_NRO})

    return()
endif()

add_subdirectory(tools/XenonRecomp EXCLUDE_FROM_ALL)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/SDL EXCLUDE_FROM_ALL)

file(GLOB RAYMAN_PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp)
add_library(RaymanRecompLib STATIC
    ${RAYMAN_PPC_SOURCES}
    ${PPC_DIR}/ppc_func_mapping.cpp
)
target_include_directories(RaymanRecompLib PUBLIC
    ${PPC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
)
target_compile_options(RaymanRecompLib PRIVATE
    -O2 -fno-strict-aliasing -ffp-model=strict -w
    -include ${CMAKE_CURRENT_SOURCE_DIR}/runtime/cpu/ppc_overrides.h
)

set(IMPORT_STUBS ${CMAKE_CURRENT_BINARY_DIR}/generated/import_stubs.cpp)
add_custom_command(
    OUTPUT ${IMPORT_STUBS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_import_stubs.py ${PPC_DIR}/ppc_func_mapping.cpp ${IMPORT_STUBS}
    DEPENDS ${PPC_DIR}/ppc_func_mapping.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_import_stubs.py
)

add_executable(rayman
    runtime/main.cpp
    runtime/memory.cpp
    runtime/loader.cpp
    runtime/crash_handler.cpp
    runtime/kernel/stub_log.cpp
    runtime/kernel/heap.cpp
    runtime/kernel/page_heap.cpp
    runtime/kernel/memory_imports.cpp
    runtime/kernel/objects.cpp
    runtime/kernel/thread.cpp
    runtime/kernel/sync_imports.cpp
    runtime/kernel/system_imports.cpp
    runtime/kernel/guest_printf.cpp
    runtime/kernel/file_system.cpp
    runtime/kernel/xam_imports.cpp
    runtime/kernel/net_imports.cpp
    runtime/kernel/video_imports.cpp
    runtime/gpu/command_processor.cpp
    runtime/apu/audio.cpp
    runtime/host/platform.cpp
    thirdparty/o1heap/o1heap/o1heap.c
    ${IMPORT_STUBS}
)
target_include_directories(rayman PRIVATE
    runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap/o1heap
)
target_compile_options(rayman PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O1 -g -fno-strict-aliasing -ffp-model=strict>)
target_link_libraries(rayman PRIVATE RaymanRecompLib XenonUtils SDL3::SDL3-static)
if (APPLE)
    target_link_options(rayman PRIVATE -Wl,-export_dynamic)
endif()
