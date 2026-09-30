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
        runtime/gpu/native_hooks.cpp
        runtime/apu/audio.cpp
        runtime/switch/platform.cpp
        runtime/switch/guest_bootstrap.cpp
        runtime/switch/native_renderer.cpp
        runtime/switch/presenter.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/xxHash
    )

    target_compile_options(RaymanSwitchRuntimeCore PRIVATE
        -O1
        -fno-strict-aliasing
        -w
    )

    target_link_libraries(RaymanSwitchRuntimeCore PRIVATE RaymanRecompLib)

    # The Switch Vulkan driver is deliberately an external build input.  This
    # repository must remain buildable for bootstrap/diagnostic work without
    # NVK, while a complete nvk-switch package enables the real renderer.
    set(RAYMAN_SWITCH_NVK_ROOT "" CACHE PATH
        "Switch NVK SDK root (include/vulkan and lib/libvulkan.a)")
    set(RAYMAN_SWITCH_NVK_LIBRARY "" CACHE FILEPATH
        "Optional explicit path to the Switch NVK libvulkan.a")
    if (RAYMAN_SWITCH_NVK_ROOT OR RAYMAN_SWITCH_NVK_LIBRARY)
        if (NOT RAYMAN_SWITCH_NVK_ROOT)
            get_filename_component(_RAYMAN_NVK_LIB_DIR "${RAYMAN_SWITCH_NVK_LIBRARY}" DIRECTORY)
            get_filename_component(RAYMAN_SWITCH_NVK_ROOT "${_RAYMAN_NVK_LIB_DIR}/.." ABSOLUTE)
        endif()
        if (NOT RAYMAN_SWITCH_NVK_LIBRARY)
            set(RAYMAN_SWITCH_NVK_LIBRARY
                "${RAYMAN_SWITCH_NVK_ROOT}/lib/libvulkan.a")
        endif()

        set(_RAYMAN_NVK_HEADER
            "${RAYMAN_SWITCH_NVK_ROOT}/include/vulkan/vulkan.h")
        set(_RAYMAN_NVK_VI_HEADER
            "${RAYMAN_SWITCH_NVK_ROOT}/include/vulkan/vulkan_vi.h")
        foreach(_RAYMAN_NVK_INPUT IN ITEMS
            "${_RAYMAN_NVK_HEADER}"
            "${_RAYMAN_NVK_VI_HEADER}"
            "${RAYMAN_SWITCH_NVK_LIBRARY}")
            if (NOT EXISTS "${_RAYMAN_NVK_INPUT}")
                message(FATAL_ERROR
                    "Incomplete Switch NVK package: missing ${_RAYMAN_NVK_INPUT}")
            endif()
        endforeach()

        # Prefer a self-contained NVK package.  This is useful when the driver
        # was built in its official container but the host devkitPro install
        # does not include the Switch portlibs repository.  Fall back to the
        # conventional devkitPro location for packages assembled that way.
        set(_RAYMAN_SWITCH_NVK_PORTLIBS "")
        foreach(_RAYMAN_PORTLIB IN ITEMS z zstd expat dl)
            set(_RAYMAN_PACKAGED_PORTLIB
                "${RAYMAN_SWITCH_NVK_ROOT}/lib/lib${_RAYMAN_PORTLIB}.a")
            set(_RAYMAN_SYSTEM_PORTLIB
                "${DEVKITPRO}/portlibs/switch/lib/lib${_RAYMAN_PORTLIB}.a")
            if (EXISTS "${_RAYMAN_PACKAGED_PORTLIB}")
                list(APPEND _RAYMAN_SWITCH_NVK_PORTLIBS
                    "${_RAYMAN_PACKAGED_PORTLIB}")
            elseif (EXISTS "${_RAYMAN_SYSTEM_PORTLIB}")
                list(APPEND _RAYMAN_SWITCH_NVK_PORTLIBS
                    "${_RAYMAN_SYSTEM_PORTLIB}")
            else()
                message(FATAL_ERROR
                    "Switch NVK needs lib${_RAYMAN_PORTLIB}.a in "
                    "${RAYMAN_SWITCH_NVK_ROOT}/lib or "
                    "${DEVKITPRO}/portlibs/switch/lib")
            endif()
        endforeach()

        target_sources(RaymanSwitchRuntimeCore PRIVATE
            tools/native_renderer/thirdparty/volk/volk.c
        )
        target_include_directories(RaymanSwitchRuntimeCore PRIVATE
            ${RAYMAN_SWITCH_NVK_ROOT}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/native_renderer
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/native_renderer/thirdparty/volk
        )
        target_compile_definitions(RaymanSwitchRuntimeCore PRIVATE
            RAYMAN_SWITCH_NATIVE_VULKAN=1
            VK_USE_PLATFORM_VI_NN=1
        )
    endif()

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
        runtime/switch/exception_handler.cpp
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
    )

    if (RAYMAN_SWITCH_NVK_ROOT OR RAYMAN_SWITCH_NVK_LIBRARY)
        target_link_options(rayman_switch PRIVATE
            "LINKER:--wrap=vk_icdGetInstanceProcAddr"
            "LINKER:--wrap=open"
            "LINKER:--wrap=close"
            "LINKER:--wrap=stat"
            "LINKER:--wrap=lstat"
            "LINKER:--allow-multiple-definition"
        )
        target_link_libraries(rayman_switch PRIVATE
            "-Wl,--start-group"
            # The merged NVK archive contains Mesa entrypoint tables whose
            # driver/common implementations are referenced weakly.  Ordinary
            # archive extraction drops those members and leaves null WSI
            # callbacks (for example vkGetPhysicalDeviceProperties2).
            "-Wl,--whole-archive"
            ${RAYMAN_SWITCH_NVK_LIBRARY}
            "-Wl,--no-whole-archive"
            ${_RAYMAN_SWITCH_NVK_PORTLIBS}
            nx
            m
            pthread
            "-Wl,--end-group"
        )
    else()
        target_link_libraries(rayman_switch PRIVATE nx)
    endif()

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
