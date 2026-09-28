/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_immediate_drawer.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/streamline_bridge.h>

#include <dxgi1_6.h>
#include <malloc.h>

REXCVAR_DEFINE_BOOL(d3d12_debug, false, "UI/D3D12", "Enable Direct3D 12 and DXGI debug layer")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(d3d12_debug_gpu_validation, false, "UI/D3D12",
                    "With d3d12_debug, also enable GPU-based validation (very slow)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(d3d12_break_on_error, false, "UI/D3D12",
                    "Break on Direct3D 12 validation errors");

REXCVAR_DEFINE_BOOL(d3d12_break_on_warning, false, "UI/D3D12",
                    "Break on Direct3D 12 validation warnings");

REXCVAR_DEFINE_INT32(d3d12_adapter, -1, "UI/D3D12",
                     "Index of the DXGI adapter to use (-1 for any physical, -2 for WARP)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(d3d12_queue_priority, 1, "UI/D3D12",
                     "Graphics command queue priority (0=normal, 1=high, 2=realtime)")
    .range(0, 2);

namespace rex::ui::d3d12 {

namespace {

// With d3d12_debug, debug layer messages go to stderr (REX_D3D12_DEBUG): the
// first occurrences of each message ID in full, then at powers of two.
std::mutex debug_message_mutex;
std::unordered_map<int, uint32_t> debug_message_counts;

void CALLBACK LogDebugMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
                              D3D12_MESSAGE_ID id, LPCSTR description, void* context) {
  uint32_t count;
  {
    std::lock_guard<std::mutex> lock(debug_message_mutex);
    count = ++debug_message_counts[int(id)];
  }
  if (count > 8 && (count & (count - 1))) {
    return;
  }
  std::fprintf(stderr, "REX_D3D12_DEBUG severity=%d category=%d id=%d count=%u %s\n",
               int(severity), int(category), int(id), count, description ? description : "");
  std::fflush(stderr);
}

}  // namespace

bool D3D12Provider::IsD3D12APIAvailable() {
  HMODULE library_d3d12 = LoadLibraryW(L"D3D12.dll");
  if (!library_d3d12) {
    return false;
  }
  FreeLibrary(library_d3d12);
  return true;
}

std::unique_ptr<D3D12Provider> D3D12Provider::Create() {
  std::unique_ptr<D3D12Provider> provider(new D3D12Provider);
  if (!provider->Initialize()) {
    rex::FatalError(
        "Unable to initialize Direct3D 12 graphics subsystem.\n"
        "\n"
        "Ensure that you have the latest drivers for your GPU and it supports "
        "Direct3D 12 with the feature level of at least 11_0.\n"
        "\n"
        "See https://xenia.jp/faq/ for more information and a list of "
        "supported GPUs.");
    return nullptr;
  }
  return provider;
}

D3D12Provider::~D3D12Provider() {
  if (graphics_analysis_ != nullptr) {
    graphics_analysis_->Release();
  }
  // Streamline's proxies and slShutdown go before the native objects.
  streamline::Shutdown();
  if (direct_queue_ != nullptr) {
    direct_queue_->Release();
  }
  if (device_ != nullptr) {
    device_->Release();
  }
  if (dxgi_factory_ != nullptr) {
    dxgi_factory_->Release();
  }

  if (REXCVAR_GET(d3d12_debug) && pfn_dxgi_get_debug_interface1_) {
    Microsoft::WRL::ComPtr<IDXGIDebug> dxgi_debug;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&dxgi_debug)))) {
      dxgi_debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
    }
  }

  if (library_dxcompiler_ != nullptr) {
    FreeLibrary(library_dxcompiler_);
  }
  if (library_dxilconv_ != nullptr) {
    FreeLibrary(library_dxilconv_);
  }
  if (library_d3dcompiler_ != nullptr) {
    FreeLibrary(library_d3dcompiler_);
  }
  if (library_d3d12_ != nullptr) {
    FreeLibrary(library_d3d12_);
  }
  if (library_dxgi_ != nullptr) {
    FreeLibrary(library_dxgi_);
  }
}

IDXGIFactory2* D3D12Provider::GetSwapChainFactory() const {
  IDXGIFactory2* proxy = streamline::SwapChainFactory();
  return proxy ? proxy : dxgi_factory_;
}

ID3D12CommandQueue* D3D12Provider::GetSwapChainQueue() const {
  ID3D12CommandQueue* proxy = streamline::SwapChainQueue();
  return proxy ? proxy : direct_queue_;
}

bool D3D12Provider::EnableIncreaseBasePriorityPrivilege() {
  TOKEN_PRIVILEGES privileges;
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValue(nullptr, SE_INC_BASE_PRIORITY_NAME, &privileges.Privileges[0].Luid)) {
    return false;
  }
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
    return false;
  }
  bool enabled =
      AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) &&
      GetLastError() != ERROR_NOT_ALL_ASSIGNED;
  CloseHandle(token);
  return enabled;
}

bool D3D12Provider::Initialize() {
  // Load the core libraries.
  library_dxgi_ = LoadLibraryW(L"dxgi.dll");
  library_d3d12_ = LoadLibraryW(L"D3D12.dll");
  if (!library_dxgi_ || !library_d3d12_) {
    REXLOG_ERROR("Failed to load dxgi.dll or D3D12.dll");
    return false;
  }
  bool libraries_loaded = true;
  libraries_loaded &= (pfn_create_dxgi_factory2_ = PFNCreateDXGIFactory2(
                           GetProcAddress(library_dxgi_, "CreateDXGIFactory2"))) != nullptr;
  libraries_loaded &= (pfn_dxgi_get_debug_interface1_ = PFNDXGIGetDebugInterface1(
                           GetProcAddress(library_dxgi_, "DXGIGetDebugInterface1"))) != nullptr;
  libraries_loaded &= (pfn_d3d12_get_debug_interface_ = PFN_D3D12_GET_DEBUG_INTERFACE(
                           GetProcAddress(library_d3d12_, "D3D12GetDebugInterface"))) != nullptr;
  libraries_loaded &= (pfn_d3d12_create_device_ = PFN_D3D12_CREATE_DEVICE(
                           GetProcAddress(library_d3d12_, "D3D12CreateDevice"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_serialize_root_signature_ = PFN_D3D12_SERIALIZE_ROOT_SIGNATURE(
           GetProcAddress(library_d3d12_, "D3D12SerializeRootSignature"))) != nullptr;
  if (!libraries_loaded) {
    REXLOG_ERROR("Failed to get DXGI or Direct3D 12 functions");
    return false;
  }

  // Load optional D3DCompiler_47.dll.
  pfn_d3d_disassemble_ = nullptr;
  library_d3dcompiler_ = LoadLibraryW(L"D3DCompiler_47.dll");
  if (library_d3dcompiler_) {
    pfn_d3d_disassemble_ = pD3DDisassemble(GetProcAddress(library_d3dcompiler_, "D3DDisassemble"));
    if (pfn_d3d_disassemble_ == nullptr) {
      REXLOG_DEBUG(
          "Failed to get D3DDisassemble from D3DCompiler_47.dll, DXBC "
          "disassembly for debugging will be unavailable");
    }
  } else {
    REXLOG_DEBUG(
        "Failed to load D3DCompiler_47.dll, DXBC disassembly for debugging "
        "will be unavailable");
  }

  // Load optional dxilconv.dll.
  pfn_dxilconv_dxc_create_instance_ = nullptr;
  library_dxilconv_ = LoadLibraryW(L"dxilconv.dll");
  if (library_dxilconv_) {
    pfn_dxilconv_dxc_create_instance_ =
        DxcCreateInstanceProc(GetProcAddress(library_dxilconv_, "DxcCreateInstance"));
    if (pfn_dxilconv_dxc_create_instance_ == nullptr) {
      REXLOG_DEBUG(
          "Failed to get DxcCreateInstance from dxilconv.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    REXLOG_DEBUG(
        "Failed to load dxilconv.dll, converted DXIL disassembly for debugging "
        "will be unavailable - DXIL may be unsupported by your OS version");
  }

  // Load optional dxcompiler.dll.
  pfn_dxcompiler_dxc_create_instance_ = nullptr;
  library_dxcompiler_ = LoadLibraryW(L"dxcompiler.dll");
  if (library_dxcompiler_) {
    pfn_dxcompiler_dxc_create_instance_ =
        DxcCreateInstanceProc(GetProcAddress(library_dxcompiler_, "DxcCreateInstance"));
    if (pfn_dxcompiler_dxc_create_instance_ == nullptr) {
      REXLOG_DEBUG(
          "Failed to get DxcCreateInstance from dxcompiler.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    REXLOG_DEBUG(
        "Failed to load dxcompiler.dll, converted DXIL disassembly for "
        "debugging will be unavailable - if needed, download the DirectX "
        "Shader Compiler from "
        "https://github.com/microsoft/DirectXShaderCompiler/releases and place "
        "the DLL in the Xenia directory");
  }

  // Configure the DXGI debug info queue.
  if (REXCVAR_GET(d3d12_break_on_error) || REXCVAR_GET(d3d12_break_on_warning)) {
    IDXGIInfoQueue* dxgi_info_queue;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&dxgi_info_queue)))) {
      if (REXCVAR_GET(d3d12_break_on_error)) {
        dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL,
                                            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR,
                                            TRUE);
      }
      if (REXCVAR_GET(d3d12_break_on_warning)) {
        dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL,
                                            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, TRUE);
      }
      dxgi_info_queue->Release();
    }
  }

  // Enable the debug layer.
  bool debug = REXCVAR_GET(d3d12_debug);
  if (debug) {
    ID3D12Debug* debug_interface;
    if (SUCCEEDED(pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&debug_interface)))) {
      debug_interface->EnableDebugLayer();
      if (REXCVAR_GET(d3d12_debug_gpu_validation)) {
        ID3D12Debug1* debug_interface1;
        if (SUCCEEDED(debug_interface->QueryInterface(IID_PPV_ARGS(&debug_interface1)))) {
          debug_interface1->SetEnableGPUBasedValidation(TRUE);
          debug_interface1->Release();
          std::fprintf(stderr, "REX_D3D12_DEBUG_GPU_VALIDATION enabled=1\n");
        }
      }
      debug_interface->Release();
    } else {
      REXLOG_WARN("Failed to enable the Direct3D 12 debug layer");
      debug = false;
    }

    // Enable DRED (Device Removed Extended Data) for diagnosing GPU crashes.
    {
      Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred_settings;
      if (SUCCEEDED(pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&dred_settings)))) {
        dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        REXLOG_INFO("DRED (Device Removed Extended Data) enabled");
      } else {
        REXLOG_WARN(
            "Failed to enable DRED - device removal diagnostics will be "
            "limited");
      }
    }
  }
  // Optional NVIDIA Streamline (Reflex), initialized before any DXGI or
  // Direct3D 12 object exists as it requires.
  streamline::InitializeBeforeDevice();

  // Create the DXGI factory.
  IDXGIFactory2* dxgi_factory;
  if (FAILED(pfn_create_dxgi_factory2_(debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                       IID_PPV_ARGS(&dxgi_factory)))) {
    REXLOG_ERROR("Failed to create a DXGI factory");
    return false;
  }

  // Choose the adapter. By default the high-performance one first (the
  // discrete GPU on hybrid laptops, where the first listed adapter is usually
  // the integrated one), then the first hardware adapter.
  uint32_t adapter_index = 0;
  IDXGIAdapter1* adapter = nullptr;
  if (REXCVAR_GET(d3d12_adapter) == -1) {
    IDXGIFactory6* dxgi_factory6 = nullptr;
    if (SUCCEEDED(dxgi_factory->QueryInterface(IID_PPV_ARGS(&dxgi_factory6)))) {
      for (UINT preference_index = 0;
           !adapter && SUCCEEDED(dxgi_factory6->EnumAdapterByGpuPreference(
                           preference_index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                           IID_PPV_ARGS(&adapter)));
           ++preference_index) {
        DXGI_ADAPTER_DESC1 preferred_desc;
        if (FAILED(adapter->GetDesc1(&preferred_desc)) ||
            (preferred_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            FAILED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0,
                                            _uuidof(ID3D12Device), nullptr))) {
          adapter->Release();
          adapter = nullptr;
        }
      }
      dxgi_factory6->Release();
    }
  }
  while (!adapter && dxgi_factory->EnumAdapters1(adapter_index, &adapter) == S_OK) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
      if (SUCCEEDED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device),
                                             nullptr))) {
        if (REXCVAR_GET(d3d12_adapter) >= 0) {
          if (adapter_index == REXCVAR_GET(d3d12_adapter)) {
            break;
          }
        } else if (REXCVAR_GET(d3d12_adapter) == -2) {
          if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            break;
          }
        } else {
          if (!(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            break;
          }
        }
      }
    }
    adapter->Release();
    adapter = nullptr;
    ++adapter_index;
  }
  if (adapter == nullptr) {
    REXLOG_ERROR(
        "Failed to get an adapter supporting Direct3D 12 with the feature "
        "level of at least 11_0");
    dxgi_factory->Release();
    return false;
  }
  DXGI_ADAPTER_DESC adapter_desc;
  if (FAILED(adapter->GetDesc(&adapter_desc))) {
    REXLOG_ERROR("Failed to get the DXGI adapter description");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter_vendor_id_ = GpuVendorID(adapter_desc.VendorId);
  int adapter_name_mb_size =
      WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, nullptr, 0, nullptr, nullptr);
  if (adapter_name_mb_size != 0) {
    char* adapter_name_mb = reinterpret_cast<char*>(alloca(adapter_name_mb_size));
    if (WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, adapter_name_mb,
                            adapter_name_mb_size, nullptr, nullptr) != 0) {
      REXGPU_INFO("DXGI adapter: {} (vendor 0x{:04X}, device 0x{:04X})", adapter_name_mb,
                  adapter_desc.VendorId, adapter_desc.DeviceId);
      // Also on stderr, so session logs and test runs name the GPU and its
      // driver (user-mode driver version) for reports.
      LARGE_INTEGER driver_version = {};
      const bool driver_known =
          SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version));
      std::fprintf(stderr,
                   "REX_GPU_ADAPTER name=\"%s\" vendor=0x%04X device=0x%04X driver=%u.%u.%u.%u "
                   "dedicated_mb=%llu\n",
                   adapter_name_mb, adapter_desc.VendorId, adapter_desc.DeviceId,
                   driver_known ? uint32_t(HIWORD(driver_version.HighPart)) : 0u,
                   driver_known ? uint32_t(LOWORD(driver_version.HighPart)) : 0u,
                   driver_known ? uint32_t(HIWORD(driver_version.LowPart)) : 0u,
                   driver_known ? uint32_t(LOWORD(driver_version.LowPart)) : 0u,
                   static_cast<unsigned long long>(adapter_desc.DedicatedVideoMemory >> 20));
      std::fflush(stderr);
    }
  }

  // Create the Direct3D 12 device.
  ID3D12Device* device;
  if (FAILED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    REXLOG_ERROR("Failed to create a Direct3D 12 feature level 11_0 device");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter->Release();
  streamline::OnDeviceCreated(device,
                              (uint64_t(uint32_t(adapter_desc.AdapterLuid.HighPart)) << 32) |
                                  adapter_desc.AdapterLuid.LowPart);

  // Configure the Direct3D 12 debug info queue.
  ID3D12InfoQueue* d3d12_info_queue;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d12_info_queue)))) {
    D3D12_MESSAGE_SEVERITY d3d12_info_queue_denied_severities[] = {
        D3D12_MESSAGE_SEVERITY_INFO,
    };
    D3D12_MESSAGE_ID d3d12_info_queue_denied_messages[] = {
        // Xbox 360 vertex fetch is explicit in shaders.
        D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT,
        // Bug in the debug layer (fixed in some version of Windows) - gaps in
        // render target bindings must be represented with a fully typed RTV
        // descriptor and DXGI_FORMAT_UNKNOWN in the pipeline state, but older
        // debug layer versions give a format mismatch error in this case.
        D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE,
        // Render targets and shader exports don't have to match on the Xbox
        // 360.
        D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
        // Arbitrary scissor can be specified by the guest, also it can be
        // explicitly used to disable drawing.
        D3D12_MESSAGE_ID_DRAW_EMPTY_SCISSOR_RECTANGLE,
        // Arbitrary clear values can be specified by the guest.
        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
    };
    D3D12_INFO_QUEUE_FILTER d3d12_info_queue_filter = {};
    d3d12_info_queue_filter.DenyList.NumSeverities =
        UINT(rex::countof(d3d12_info_queue_denied_severities));
    d3d12_info_queue_filter.DenyList.pSeverityList = d3d12_info_queue_denied_severities;
    d3d12_info_queue_filter.DenyList.NumIDs = UINT(rex::countof(d3d12_info_queue_denied_messages));
    d3d12_info_queue_filter.DenyList.pIDList = d3d12_info_queue_denied_messages;
    d3d12_info_queue->PushStorageFilter(&d3d12_info_queue_filter);
    if (REXCVAR_GET(d3d12_break_on_error)) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
    if (REXCVAR_GET(d3d12_break_on_warning)) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
    }
    ID3D12InfoQueue1* d3d12_info_queue1;
    if (SUCCEEDED(d3d12_info_queue->QueryInterface(IID_PPV_ARGS(&d3d12_info_queue1)))) {
      DWORD cookie = 0;
      const bool registered = SUCCEEDED(d3d12_info_queue1->RegisterMessageCallback(
          LogDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie));
      std::fprintf(stderr, "REX_D3D12_DEBUG_LOG registered=%u\n", registered ? 1u : 0u);
      d3d12_info_queue1->Release();
    }
    d3d12_info_queue->Release();
  }

  // Create the command queue for graphics.
  D3D12_COMMAND_QUEUE_DESC queue_desc;
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (REXCVAR_GET(d3d12_queue_priority) >= 2) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    if (!EnableIncreaseBasePriorityPrivilege()) {
      REXLOG_WARN(
          "Failed to enable SeIncreaseBasePriorityPrivilege for global "
          "realtime Direct3D 12 command queue priority, falling back to high "
          "priority, try launching Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    }
  } else if (REXCVAR_GET(d3d12_queue_priority) >= 1) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  } else {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  }
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.NodeMask = 0;
  ID3D12CommandQueue* direct_queue;
  // With Streamline the queue is created through its device proxy (the swap
  // chain needs that proxy); every submission uses the native queue.
  auto create_direct_queue = [&]() {
    return streamline::CreateDirectQueue(device, queue_desc, &direct_queue) ||
           SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue)));
  };
  if (!create_direct_queue()) {
    bool queue_created = false;
    if (queue_desc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
      REXLOG_WARN(
          "Failed to create a Direct3D 12 direct command queue with global "
          "realtime priority, falling back to high priority, try launching "
          "Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
      queue_created = create_direct_queue();
    }
    if (!queue_created) {
      REXLOG_ERROR("Failed to create a Direct3D 12 direct command queue");
      device->Release();
      dxgi_factory->Release();
      return false;
    }
  }

  dxgi_factory_ = dxgi_factory;
  device_ = device;
  direct_queue_ = direct_queue;
  streamline::UpgradeFactory(dxgi_factory);

  // Get descriptor sizes for each type.
  for (uint32_t i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i) {
    descriptor_sizes_[i] = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE(i));
  }

  // Check if optional features are supported.
  // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED requires Windows 10 2004 (indicated by
  // the availability of ID3D12Device8 or D3D12_FEATURE_D3D12_OPTIONS7).
  heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_NONE;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
    heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
  }
  ps_specified_stencil_reference_supported_ = false;
  rasterizer_ordered_views_supported_ = false;
  resource_binding_tier_ = D3D12_RESOURCE_BINDING_TIER_1;
  tiled_resources_tier_ = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
  unaligned_block_textures_supported_ = false;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
    ps_specified_stencil_reference_supported_ = bool(options.PSSpecifiedStencilRefSupported);
    rasterizer_ordered_views_supported_ = bool(options.ROVsSupported);
    resource_binding_tier_ = options.ResourceBindingTier;
    tiled_resources_tier_ = options.TiledResourcesTier;
  }
  programmable_sample_positions_tier_ = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2)))) {
    programmable_sample_positions_tier_ = options2.ProgrammableSamplePositionsTier;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options8, sizeof(options8)))) {
    unaligned_block_textures_supported_ = bool(options8.UnalignedBlockTexturesSupported);
  }
  virtual_address_bits_per_resource_ = 0;
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT virtual_address_support;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT,
                                            &virtual_address_support,
                                            sizeof(virtual_address_support)))) {
    virtual_address_bits_per_resource_ =
        virtual_address_support.MaxGPUVirtualAddressBitsPerResource;
  }
  REXGPU_INFO(
      "Direct3D 12 device and OS features:\n"
      "* Max GPU virtual address bits per resource: {}\n"
      "* Non-zeroed heap creation: {}\n"
      "* Pixel-shader-specified stencil reference: {}\n"
      "* Programmable sample positions: tier {}\n"
      "* Rasterizer-ordered views: {}\n"
      "* Resource binding: tier {}\n"
      "* Tiled resources: tier {}\n"
      "* Unaligned block-compressed textures: {}",
      virtual_address_bits_per_resource_,
      (heap_flag_create_not_zeroed_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) ? "yes" : "no",
      ps_specified_stencil_reference_supported_ ? "yes" : "no",
      uint32_t(programmable_sample_positions_tier_),
      rasterizer_ordered_views_supported_ ? "yes" : "no", uint32_t(resource_binding_tier_),
      uint32_t(tiled_resources_tier_), unaligned_block_textures_supported_ ? "yes" : "no");

  // Get the graphics analysis interface, will silently fail if PIX is not
  // attached.
  pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&graphics_analysis_));

  return true;
}

std::unique_ptr<Presenter> D3D12Provider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return D3D12Presenter::Create(host_gpu_loss_callback, *this);
}

std::unique_ptr<ImmediateDrawer> D3D12Provider::CreateImmediateDrawer() {
  return D3D12ImmediateDrawer::Create(*this);
}

}  // namespace rex::ui::d3d12
