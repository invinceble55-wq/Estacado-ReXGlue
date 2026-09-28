// Diagnostics (d3d12_debug_gpu_spin): one thread group that runs a dependent
// integer chain for xe_spin_iterations steps at every frame start, so each
// frame's GPU work takes longer - emulates a slower GPU for bugs that depend on
// how far the GPU lags behind the CPU. The store is practically never taken;
// it keeps the loop from being removed.
// scripts/build-debug-shaders.ps1 (FXC) -> bytecode/d3d12_5_1/debug_gpu_spin_cs.h

cbuffer XeDebugGpuSpinConstants : register(b0) {
  uint xe_spin_iterations;
};

RWByteAddressBuffer xe_spin_dest : register(u0);

[numthreads(32, 1, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint value = xe_thread_id.x;
  [loop] for (uint i = 0u; i < xe_spin_iterations; ++i) {
    value = value * 1664525u + 1013904223u;
  }
  [branch] if (value == 0x7FFFFFFFu) {
    xe_spin_dest.Store(xe_thread_id.x << 2u, value);
  }
}
