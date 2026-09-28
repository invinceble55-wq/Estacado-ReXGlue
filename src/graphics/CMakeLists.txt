# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    pipeline/shader/replacement_pack.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    trace_writer.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/texture/texture_pack.cpp
    pipeline/texture/prompt_icons.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/command_processor_temporal_aa.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

add_library(rexgpu-xenos SHARED ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)

if(WIN32)
    # Match the runtime DLL's deterministic Release PE-link policy.
    target_link_options(rexgpu-xenos PRIVATE
        $<$<CONFIG:Release>:LINKER:/Brepro>)
endif()

set_target_properties(rexgpu-xenos PROPERTIES
    EXPORT_NAME gpu-xenos
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(rexgpu-xenos PRIVATE
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-variable)

# Opt-in cross-module inlining for the command-processor path. Only this
# plugin's own objects are compiled as ThinLTO bitcode; lld-link completes the
# optimization at link time. Off by default so existing builds are unchanged.
option(REXGLUE_GPU_THINLTO "Build rexgpu-xenos Release objects with ThinLTO" OFF)
if(REXGLUE_GPU_THINLTO AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(rexgpu-xenos PRIVATE $<$<CONFIG:Release>:-flto=thin>)
endif()

# Opt-in profile-guided optimization of the plugin (clang, IR PGO). GENERATE
# builds an instrumented training DLL (never a player build) that writes its
# counters to LLVM_PROFILE_FILE; USE optimizes with a merged .profdata. OFF by
# default so existing builds are unchanged.
set(REXGLUE_GPU_PGO "OFF" CACHE STRING
    "rexgpu-xenos profile-guided optimization: OFF, GENERATE or USE")
set_property(CACHE REXGLUE_GPU_PGO PROPERTY STRINGS OFF GENERATE USE)
set(REXGLUE_GPU_PGO_PROFILE "" CACHE FILEPATH
    "Merged .profdata used when REXGLUE_GPU_PGO is USE")
if(NOT REXGLUE_GPU_PGO STREQUAL "OFF")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "REXGLUE_GPU_PGO requires clang")
    endif()
    if(REXGLUE_GPU_PGO STREQUAL "GENERATE")
        target_compile_options(rexgpu-xenos PRIVATE $<$<CONFIG:Release>:-fprofile-generate>)
        target_compile_definitions(rexgpu-xenos PRIVATE REXGLUE_PGO_GENERATE=1)
        # lld-link is invoked directly, so the profile runtime isn't added
        # implicitly as it would be by the clang driver.
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
                        OUTPUT_VARIABLE REXGLUE_CLANG_RESOURCE_DIR
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(REXGLUE_CLANG_PROFILE_RUNTIME
            "${REXGLUE_CLANG_RESOURCE_DIR}/lib/windows/clang_rt.profile-x86_64.lib")
        if(NOT EXISTS "${REXGLUE_CLANG_PROFILE_RUNTIME}")
            message(FATAL_ERROR "clang profile runtime not found: ${REXGLUE_CLANG_PROFILE_RUNTIME}")
        endif()
        target_link_libraries(rexgpu-xenos PRIVATE "${REXGLUE_CLANG_PROFILE_RUNTIME}")
    elseif(REXGLUE_GPU_PGO STREQUAL "USE")
        if(NOT EXISTS "${REXGLUE_GPU_PGO_PROFILE}")
            message(FATAL_ERROR "REXGLUE_GPU_PGO_PROFILE not found: ${REXGLUE_GPU_PGO_PROFILE}")
        endif()
        target_compile_options(rexgpu-xenos PRIVATE
            $<$<CONFIG:Release>:-fprofile-use=${REXGLUE_GPU_PGO_PROFILE}>
            -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
            -Wno-backend-plugin)
        # lld's call-graph-profile function ordering is not reproducible
        # (three clean builds gave three layouts); keep the deterministic
        # PE-link policy. Branch weights, inlining and block layout still use
        # the profile.
        target_link_options(rexgpu-xenos PRIVATE
            $<$<CONFIG:Release>:LINKER:/call-graph-profile-sort:no>)
        # Rebuild when the profile changes.
        set_property(SOURCE ${REXGPU_XENOS_SOURCES} APPEND PROPERTY
            OBJECT_DEPENDS "${REXGLUE_GPU_PGO_PROFILE}")
    else()
        message(FATAL_ERROR "REXGLUE_GPU_PGO must be OFF, GENERATE or USE")
    endif()
endif()

# Diagnostics and work counters (GPU timing categories and pass split,
# transfer logs, upload/texture/resolve counters, ...) are compiled only into
# diagnostic builds (a separate tree); player builds contain none of their
# checks, so a disabled diagnostic can't cost frames or break PGO profiles.
option(REXGLUE_GPU_DIAGNOSTICS
    "Compile GPU diagnostics and work counters into rexgpu-xenos (measurement builds only)" OFF)
target_compile_definitions(rexgpu-xenos PRIVATE
    REX_GPU_DIAGNOSTICS=$<IF:$<BOOL:${REXGLUE_GPU_DIAGNOSTICS}>,1,0>)

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
target_link_libraries(rexgpu-xenos PRIVATE
    rexruntime
    xxHash::xxhash
    snappy
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-xenos PRIVATE
        volk::volk
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgpu-xenos PRIVATE
        d3d12
        dxgi
        dxguid
    )
    # Optional NVIDIA DLSS (NGX) for the temporal AA path. The SDK is fetched
    # into the parent project's build/deps/DLSS (never committed: its license
    # allows redistribution only as object code inside the application).
    set(REXGLUE_DLSS_SDK_DIR "${REXGLUE_ROOT}/../../build/deps/DLSS" CACHE PATH
        "NVIDIA DLSS SDK root (include/, lib/Windows_x86_64/x64/)")
    if(WIN32 AND EXISTS "${REXGLUE_DLSS_SDK_DIR}/include/nvsdk_ngx_helpers.h" AND
       EXISTS "${REXGLUE_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
        target_include_directories(rexgpu-xenos SYSTEM PRIVATE "${REXGLUE_DLSS_SDK_DIR}/include")
        target_link_libraries(rexgpu-xenos PRIVATE
            "${REXGLUE_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
        target_compile_definitions(rexgpu-xenos PRIVATE REX_HAVE_DLSS=1)
        message(STATUS "  DLSS SDK: ${REXGLUE_DLSS_SDK_DIR}")
    else()
        target_compile_definitions(rexgpu-xenos PRIVATE REX_HAVE_DLSS=0)
        message(STATUS "  DLSS SDK: not found (${REXGLUE_DLSS_SDK_DIR})")
    endif()
    # AMD FSR 3.1 (FidelityFX API headers; amd_fidelityfx_dx12.dll is loaded at
    # run time from the game folder) and Intel XeSS (headers; libxess.dll
    # loaded at run time). Header-only at build time, no import libraries.
    set(REXGLUE_FFX_API_DIR "${REXGLUE_ROOT}/../../build/deps/FidelityFX-SDK/ffx-api/include"
        CACHE PATH "AMD FidelityFX API include directory")
    if(WIN32 AND EXISTS "${REXGLUE_FFX_API_DIR}/ffx_api/ffx_upscale.h")
        target_include_directories(rexgpu-xenos SYSTEM PRIVATE "${REXGLUE_FFX_API_DIR}")
        target_compile_definitions(rexgpu-xenos PRIVATE REX_HAVE_FSR=1)
        message(STATUS "  FSR 3.1 API: ${REXGLUE_FFX_API_DIR}")
    else()
        target_compile_definitions(rexgpu-xenos PRIVATE REX_HAVE_FSR=0)
    endif()
    set(REXGLUE_XESS_SDK_DIR "${REXGLUE_ROOT}/../../build/deps/XeSS" CACHE PATH
        "Intel XeSS SDK root (inc/)")
    if(WIN32 AND EXISTS "${REXGLUE_XESS_SDK_DIR}/inc/xess/xess_d3d12.h")
        target_include_directories(rexgpu-xenos SYSTEM PRIVATE "${REXGLUE_XESS_SDK_DIR}/inc")
        target_compile_definitions(rexgpu-xenos PRIVATE REX_HAVE_XESS=1)
        message(STATUS "  XeSS SDK: ${REXGLUE_XESS_SDK_DIR}")
    else()
        target_compile_definitions(rexgpu-xenos PRIVATE REX_HAVE_XESS=0)
    endif()
endif()
