// Direct3D 8 (XDK 5849) entry points the title calls,
// implemented on the host
// renderer. Resource creation, locking and the XG helpers stay recompiled: they
// only manage memory.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/diag.h"
#include "d3d/gpu_frame.h"
#include "d3d/world_overlay.h"
#include "game/flame_plume.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice =
    SYM(D3D_Device); // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData =
    SYM(D3D_IndexData); // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState); // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);           // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams); // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what
// the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push;
constexpr uint32_t kDevThreshold = XDK_CDevice_PushThreshold;
constexpr uint32_t kDevBaseVertex = XDK_CDevice_BaseVertex;
constexpr uint32_t kDevIndexBuffer = XDK_CDevice_IndexBuffer;
constexpr uint32_t kDevTextures = XDK_CDevice_Textures;
constexpr uint32_t kDevRenderTarget = XDK_CDevice_RenderTarget;
constexpr uint32_t kDevDepth = XDK_CDevice_Depth;
constexpr uint32_t kDevSwapCount = XDK_CDevice_SwapCount;
constexpr uint32_t kDevBackBuffer = XDK_CDevice_BackBuffer;
constexpr uint32_t kDevFrontBuffer = XDK_CDevice_FrontBuffer;
constexpr uint32_t kDevZScale = XDK_CDevice_ZScale;
constexpr uint32_t kDevSuperX = XDK_CDevice_SuperSampleX;
constexpr uint32_t kDevSuperY = XDK_CDevice_SuperSampleY;
constexpr uint32_t kDevTransforms = XDK_CDevice_Transforms;
constexpr uint32_t kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage;
constexpr uint32_t kStreamSize = XDK_Stream_Size;

// D3DResource, D3DPixelContainer and D3DSurface fields
constexpr uint32_t kResCommon = 0; // reference counts and type
constexpr uint32_t kResData = 4;
constexpr uint32_t kResLock = 8;
constexpr uint32_t kContainerFormat = 12;
constexpr uint32_t kContainerSize = 16;
constexpr uint32_t kSurfaceParent = 0x14;
constexpr uint32_t kSurfaceBytes = 24;
constexpr uint32_t kHeaderBytes = 20; // a header without the surface parent

// Common: the device's references in bits 19 to 22, the type in bits 16 to 18
constexpr uint32_t kInternalRefs = 0x780000;
constexpr uint32_t kTitleRefs = 0xFFFF; // the title's references
constexpr uint32_t kInternalRef = 0x80000;
constexpr uint32_t kTypeMask = 0x70000;
constexpr uint32_t kTypeTexture = 0x40000;
constexpr uint32_t kTypeSurface = 0x50000;

constexpr uint32_t kVsConstants = 192; // hardware vertex constant registers
constexpr uint32_t kPsConstants = 16;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{}; // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0;
uint32_t g_scratch_size = 0;
uint32_t g_walk = 0; // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first
// dword. A surface's first one also holds its parent. The library's functions
// do it when the image has them, inlined ones are done here.
void AddRef(X86Context &ctx, uint32_t res) {
  if (!res)
    return;
#if HAS_D3D_InternalAddRef
  GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
  (void)ctx;
  uint32_t common = R32(res + kResCommon);
  bool surface = (common & kTypeMask) == kTypeSurface;
  if (!(common & kInternalRefs) && surface) {
    if (uint32_t parent = R32(res + kSurfaceParent))
      W32(parent, R32(parent) + kInternalRef);
  }
  W32(res, R32(res) + kInternalRef);
#endif
}
void Release(X86Context &ctx, uint32_t res) {
  if (!res)
    return;
#if HAS_D3D_InternalRelease
  GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
  (void)ctx;
  // the library frees a resource nothing holds any more; without its function
  // the memory stays
  auto drop = [](uint32_t r) {
    W32(r, R32(r) - kInternalRef);
    bool unheld = !(R32(r) & (kInternalRefs | kTitleRefs));
    if (unheld)
      LOG_FIRST(8,
                "d3d: resource %08X is not freed, the image has no "
                "D3D_InternalRelease",
                r);
  };
  uint32_t common = R32(res + kResCommon);
  bool surface = (common & kTypeMask) == kTypeSurface;
  if ((common & kInternalRefs) == kInternalRef && surface) {
    if (uint32_t parent = R32(res + kSurfaceParent))
      drop(parent);
  }
  drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at
// every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
  Walk(Dev(kDevPush));
  uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
  if (need > g_scratch_size) {
    if (g_scratch_push)
      mem::FreeContiguous(g_scratch_push);
    g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
    g_scratch_size = need;
  }
  SetDev(kDevPush, g_scratch_push);
  SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
  g_walk = g_scratch_push;
  return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
  gpu::Surface s;
  s.header = header;
  if (header) {
    s.data = R32(header + kResData);
    s.format = R32(header + kContainerFormat);
    s.size = R32(header + kContainerSize);
  }
  return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
  uint32_t pitch = (w * bpp + 63) & ~63u;
  uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
  uint32_t hdr = mem::AllocSystem(kSurfaceBytes);
  W32(hdr + kResCommon, 0x000D0001); // surface, one reference, one internal
                                     // reference held by the device
  W32(hdr + kResData, mem::PhysicalFromVirtual(data));
  W32(hdr + kResLock, 0);
  W32(hdr + kContainerFormat, 0x00010021 | (fmt << 8)); // dma A, 2D, one level
  W32(hdr + kContainerSize, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
  W32(hdr + kSurfaceParent, 0);
  return hdr;
}

void SyncDeferred() {
  // deferred states live in the title's memory, the inline SetRenderState and
  // SetTextureStageState write them
  for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++)
    g_state.rs[i] = R32(kRenderState + i * 4);
  for (int s = 0; s < 4; s++) {
    uint32_t stage = kTextureState + s * kStageStates * 4;
    for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++)
      g_state.tss[s][i] = R32(stage + i * 4);
  }
}

float ZScale(const gpu::Surface &zs) {
  uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
  return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

float g_view_scale = 1.0f;

// c-38 and c-37 hold the viewport scale and offset, as
// D3D_GetViewportOffsetAndScale computes them
constexpr uint32_t kViewportScale = 58;
constexpr uint32_t kViewportOffset = 59;

void UpdateViewportConstants() {
  auto &v = g_state.viewport;
  g_state.zscale = ZScale(g_state.zs);
  float hw = v.w * 0.5f;
  float hh = v.h * 0.5f;
  g_state.vsc[kViewportScale] = {hw * g_view_scale, -hh * g_view_scale,
                                 (v.max_z - v.min_z) * g_state.zscale, 0};
  float x = v.x + g_state.ss_offset_x + hw;
  float y = v.y + g_state.ss_offset_y + hh;
  g_state.vsc[kViewportOffset] = {x, y, v.min_z * g_state.zscale, 0};
}

void SetComplex(uint32_t rs, uint32_t v) {
  g_state.rs[rs] = v;
  W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the
// scratch buffer
void ApplyDefaultStates(X86Context &ctx) {
#if HAS_D3D_InitDefaultState
  GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
  (void)ctx;
  LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states "
               "were not applied");
#endif
}

} // namespace

gpu::State &State() { return g_state; }
std::recursive_mutex &Lock() { return g_lock; }

// draws grow about the viewport's centre, depth stays
void ViewScale(float xy) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  g_view_scale = xy;
  UpdateViewportConstants();
}

} // namespace d3d

using namespace d3d;

// device

namespace {
// Resets guest fixed-function TSS/RS fields that leak from game alpha passes
// into custom host overlays (preventing solid untextured squares or matrixed UVs),
// and restores the exact original guest values when the overlay draw finishes.
struct ScopedOverlayExtraState {
  uint32_t saved_specular;
  uint32_t saved_vertex_blend;
  decltype(g_state.tss[0]) saved_tss0;

  ScopedOverlayExtraState()
      : saved_specular(g_state.rs[gpu::kRsSpecularEnable]),
        saved_vertex_blend(g_state.rs[gpu::kRsVertexBlend]),
        saved_tss0(g_state.tss[0]) {
    g_state.rs[gpu::kRsSpecularEnable] = 0u;
    g_state.rs[gpu::kRsVertexBlend] = 0u;
    g_state.tss[0][gpu::kTssAddressW] = 3u;
    g_state.tss[0][gpu::kTssMipFilter] = 0u;
    g_state.tss[0][gpu::kTssMipLodBias] = 0u;
    g_state.tss[0][gpu::kTssMaxMipLevel] = 0u;
    g_state.tss[0][gpu::kTssMaxAnisotropy] = 1u;
    g_state.tss[0][gpu::kTssColorKeyOp] = 0u;
    g_state.tss[0][gpu::kTssColorSign] = 0u;
    g_state.tss[0][gpu::kTssColorArg0] = 1u; // D3DTA_CURRENT
    g_state.tss[0][gpu::kTssAlphaArg0] = 1u;
    g_state.tss[0][gpu::kTssResultArg] = 1u; // D3DTA_CURRENT (never TEMP=5)
    g_state.tss[0][gpu::kTssTextureTransformFlags] = 0u;
  }

  ~ScopedOverlayExtraState() {
    g_state.tss[0] = saved_tss0;
    g_state.rs[gpu::kRsSpecularEnable] = saved_specular;
    g_state.rs[gpu::kRsVertexBlend] = saved_vertex_blend;
  }
};

// D3DPRESENT_PARAMETERS fields
constexpr uint32_t kPpWidth = 0;
constexpr uint32_t kPpHeight = 4;
constexpr uint32_t kPpFormat = 8;
constexpr uint32_t kPpCount = 12;
constexpr uint32_t kPpAutoDepth = 0x20;
constexpr uint32_t kPpDepthFormat = 0x24;
constexpr uint32_t kPpFlags = 0x28;
constexpr uint32_t kPpInterval = 0x30;

struct PresentParams {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t count;
  uint32_t auto_depth;
  uint32_t depth_format;
  uint32_t interval;
};

PresentParams ReadPresentParams(uint32_t pp) {
  PresentParams p;
  p.width = R32(pp + kPpWidth);
  p.height = R32(pp + kPpHeight);
  p.format = R32(pp + kPpFormat);
  p.count = std::max<uint32_t>(R32(pp + kPpCount), 1);
  p.auto_depth = R32(pp + kPpAutoDepth);
  p.depth_format = R32(pp + kPpDepthFormat);
  p.interval = R32(pp + kPpInterval);
  if (!p.width)
    p.width = 640;
  if (!p.height)
    p.height = 480;
  return p;
}

void CreateBuffers(const PresentParams &p) {
  uint32_t color_fmt = gpu::LinearColorFormat(p.format);
  g_buffer_count = std::min<uint32_t>(p.count + 1, 3);
  for (uint32_t i = 0; i < g_buffer_count; i++)
    g_buffers[i] = NewSurface(p.width, p.height, color_fmt, 4);
  g_depth = 0;
  if (p.auto_depth) {
    uint32_t zf = gpu::LinearDepthFormat(p.depth_format);
    g_depth = NewSurface(
        p.width, p.height, zf,
        zf == 0x30 || zf == 0x31 ? 2 : 4); // 0x30 LIN_D16, 0x31 LIN_F16
  }
}

void EnsureBackend(uint32_t w, uint32_t h) {
  if (g_backend)
    return;
  g_backend = gpu::CreateBackend();
  if (!g_backend->Init(w, h))
    LOGE("d3d: renderer init failed");
  if (kRenderThread)
    g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
}

// the device fields the recompiled getters read: targets, super sampling and
// depth scale
void ApplyDeviceTargets() {
  SetDev(kDevRenderTarget, g_buffers[1]);
  SetDev(kDevDepth, g_depth);
  SetDev(kDevBackBuffer, g_buffers[1]);
  SetDev(kDevFrontBuffer, g_buffers[1]);
  float one = 1.0f;
  uint32_t depth_format =
      g_depth ? R32(g_depth + kContainerFormat) >> 8 & 0xFF : 0;
  float zs = depth_format == 0x30 ? 65535.0f : 16777215.0f;
  std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
  std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
  std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
}
} // namespace

HLE_STDCALL(Direct3D_CreateDevice, 6) {
  XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice),
              XDK_DATA(D3D_DirtyFlags), XDK_DATA(D3D_DeferredTextureState),
              XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
              XDK_DATA(D3D_SimpleRenderStateEncode));
  XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold),
              XDK_KEY(CDevice_BaseVertex), XDK_KEY(CDevice_IndexBuffer),
              XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
              XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms),
              XDK_KEY(CDevice_Textures), XDK_KEY(CDevice_RenderTarget),
              XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
              XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount),
              XDK_KEY(CDevice_Size), XDK_KEY(DirtyFlags_Texture),
              XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
              XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t pp = a[4]; // D3DPRESENT_PARAMETERS
  PresentParams p = ReadPresentParams(pp);
  LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X "
       "interval %X flags %X",
       p.width, p.height, p.format, p.count, p.auto_depth, p.depth_format,
       p.interval, R32(pp + kPpFlags));
  std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
  W32(kDevicePtr, kDevice);
  ScratchPush();

  CreateBuffers(p);
  EnsureBackend(p.width, p.height);
  g_state = gpu::State{};
  g_state.viewport = {0, 0, p.width, p.height, 0.0f, 1.0f};
  ApplyDeviceTargets();
  g_state.rt = SurfaceOf(g_buffers[1]);
  g_state.zs = SurfaceOf(g_depth);
  UpdateViewportConstants();
  ApplyDefaultStates(ctx);
  if (a[5])
    W32(a[5], kDevice);
  return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

namespace {
// perf diagnostic: who blocks on which surface, once each
void LogBlockOnce(X86Context &ctx, uint32_t data) {
  static std::unordered_set<uint32_t> seen;
  if (!diag::Perf() || !seen.insert(data).second)
    return;
  uint32_t code[4] = {};
  uint32_t n = 0;
  for (uint32_t i = 0; i < 32 && n < 4; i++) {
    uint32_t v = R32(ctx.esp + i * 4);
    if (v >= X86CodeBase && v < X86CodeEnd)
      code[n++] = v;
  }
  LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data,
       code[0], code[1], code[2], code[3]);
}
} // namespace

HLE_STDCALL(D3D_BlockOnResource, 1) {
  uint32_t res = a[0];
  if (res && g_backend) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t type = R32(res + kResCommon) & kTypeMask;
    if (type == kTypeTexture || type == kTypeSurface) {
      uint32_t data = mem::VirtualFromPhysical(R32(res + kResData));
      LogBlockOnce(ctx, data);
      g_backend->Sync(data, 0);
    } else {
      g_backend->WaitResource(res);
    }
  }
  gpu::MarkDirty(res);
  return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips
// the block above, yet a title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
  uint32_t res = a[0];
  if ((a[5] & 0x20) && res && g_backend) { // D3DLOCK_NOOVERWRITE
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + kResData)));
  }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
  if (a[0] && g_backend) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_backend->WaitResource(a[0]);
  }
  gpu::MarkDirty(a[0]);
  return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  return a[0] && g_backend && g_backend->Busy(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (g_backend)
    g_backend->WaitFence(a[0]);
  return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (g_backend)
    g_backend->Flush();
  return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
  LOG_FIRST(
      16,
      "d3d: MakeSpace from %08X, some caller still pushes hardware commands",
      R32(ctx.esp));
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
  LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
  uint32_t hz = app::DisplayRefreshRate();
  uint32_t cap = app::Get().fps_cap;
  uint32_t rate =
      (cap > 1) ? std::max(cap, 60u) : std::clamp(hz ? hz : 120u, 60u, 240u);
  std::this_thread::sleep_for(std::chrono::microseconds(1000000u / rate));
  return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance
// after a quick reboot, which may fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t front = g_buffers[0];
  uint32_t size = R32(front + kContainerSize);
  uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64;
  uint32_t height = (size >> 12 & 0xFFF) + 1;
  uint32_t bytes = pitch * height;
  uint32_t data = mem::VirtualFromPhysical(R32(front + kResData));
  if (g_backend)
    g_backend->Sync(data, bytes);
  uint32_t common = R32(front + kResCommon);
  kernel::SavePersistedDisplay(common, R32(front + kContainerFormat), size,
                               HostPtr(data), bytes);
  LOGI("d3d: PersistDisplay, %u bytes", bytes);
  return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

namespace {
constexpr uint32_t kGammaRampBytes = 3 * 256; // D3DGAMMARAMP: red, green, blue
} // namespace

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
  uint8_t ramp[kGammaRampBytes];
  std::memcpy(ramp, HostPtr(a[1]), kGammaRampBytes);
  if (g_backend)
    g_backend->SetGamma(ramp);
  return 0;
}

// targets and presentation

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget);
  uint32_t zs = a[1];
  AddRef(ctx, rt);
  uint32_t old_rt = Dev(kDevRenderTarget);
  uint32_t old_zs = Dev(kDevDepth);
  SetDev(kDevRenderTarget, rt);
  SetDev(kDevDepth, zs);
  AddRef(ctx, zs);
  Release(ctx, old_rt);
  Release(ctx, old_zs);
  g_state.rt = SurfaceOf(rt);
  g_state.zs = SurfaceOf(zs);
  // SetRenderTarget resets the viewport to the whole target
  uint32_t w;
  uint32_t h;
  gpu::SurfaceSize(g_state.rt, w, h);
  g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
  UpdateViewportConstants();
  return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
  int32_t i = int32_t(a[0]);
  // -1 the front buffer, 0 and up the back buffers
  uint32_t back = std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1);
  uint32_t s = i < 0 ? g_buffers[0] : g_buffers[back];
  W32(s, R32(s) + 1);
  return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
  uint32_t s = Dev(kDevRenderTarget);
  if (s)
    W32(s, R32(s) + 1);
  return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
  uint32_t s = Dev(kDevDepth);
  if (s)
    W32(s, R32(s) + 1);
  return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
uint64_t g_backend_presents = 0;

constexpr bool kGammaKnown = XDK_CDevice_GammaPending != xdk::kUnknown &&
                             XDK_CDevice_GammaRamps != xdk::kUnknown;

// the ramp an inlined SetGammaRamp left for the vertical blank that shows this
// swap
void ApplyPendingGamma() {
  if (!g_backend || !kGammaKnown)
    return;
  uint32_t i = Dev(kDevSwapCount) & 1;
  uint32_t pending = XDK_CDevice_GammaPending + i * 4;
  if (Dev(pending) != 1)
    return;
  SetDev(pending, 0);
  const void *ramp =
      HostPtr(kDevice + XDK_CDevice_GammaRamps + i * kGammaRampBytes);
  g_backend->SetGamma(static_cast<const uint8_t *>(ramp));
}

// like the XDK, the surfaces stay put and their memory rotates: titles keep
// back buffer pointers and copy its data
void RotateBuffers() {
  uint32_t last = g_buffers[g_buffer_count - 1];
  uint32_t data = R32(last + kResData);
  uint32_t lock = R32(last + kResLock);
  for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
    W32(g_buffers[i] + kResData, R32(g_buffers[i - 1] + kResData));
    W32(g_buffers[i] + kResLock, R32(g_buffers[i - 1] + kResLock));
  }
  W32(g_buffers[0] + kResData, data);
  W32(g_buffers[0] + kResLock, lock);
}
} // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
  game::WaitWhileUnfocused();
  app::SplashFirstFrame();
  // handed over from the previous instance: the window opens on the second
  // frame, the first picture with anything drawn over it right after
  if (app::HandingOver() && g_presents >= 2) {
    {
      std::lock_guard<std::recursive_mutex> lock(g_lock);
      if (g_backend)
        g_backend->Flush();
    }
    app::FinishHandover();
  }
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  g_presents++;
  // the kept display is shown until the title presents its own
  kernel::SetSavedDataAddress(0);
  ApplyPendingGamma();
  if (g_backend) {
    g_backend->Present(SurfaceOf(g_buffers[1]));
    // the title records one frame while the GPU draws the one before it: a GPU
    // bound frame rate then paces the game like a frame rate cap, and the frame
    // times the game measures match the frames shown
    if (g_backend_presents)
      gpu::WaitGpuFrame(g_backend_presents - 1);
    g_backend_presents++;
  }
  app::PaceFrame();
  RotateBuffers();
  g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
  uint32_t n = Dev(kDevSwapCount) + 1;
  SetDev(kDevSwapCount, n);
  return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  const uint8_t *rects =
      a[0] && a[1] ? static_cast<const uint8_t *>(HostPtr(a[1])) : nullptr;
  if (g_backend)
    g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
  return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (g_backend)
    g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
  return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t v = a[0];
  g_state.viewport = {R32(v),      R32(v + 4), R32(v + 8),
                      R32(v + 12), GF(v + 16), GF(v + 20)};
  g_state.scissor_count = 0;
  UpdateViewportConstants();
  return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  g_state.scissor_count = std::min<uint32_t>(a[0], 8);
  for (uint32_t i = 0; i < g_state.scissor_count * 4; i++)
    g_state.scissors[i] = R32(a[2] + i * 4);
  return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
  g_state.ss_offset_x = ArgF(a, 0);
  g_state.ss_offset_y = ArgF(a, 1);
  UpdateViewportConstants();
  return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
  uint32_t method = a[0] & 0x1FFC;
  uint32_t value = a[1];
  g_state.simple[method >> 2] = value;
  return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
  uint32_t rs = a[0];
  uint32_t v = a[1];
  if (rs < gpu::kRsDeferredFirst) {
    uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
    g_state.simple[method >> 2] = v;
  } else if (rs < gpu::kRsMax) {
    SetComplex(rs, v);
  }
  return 0;
}

#define COMPLEX_STATE(name, rs)                                                \
  HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) {                            \
    SetComplex(rs, a[0]);                                                      \
    return 0;                                                                  \
  }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
  g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
  W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4,
      a[1]);
  return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
  if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset)
    g_state.tss[a[0] & 3][a[1]] = a[2];
  return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
  g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
  return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
  g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
  return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
  uint32_t stage = a[0] & 3;
  uint32_t type = a[1];
  uint32_t v = a[2];
  if (type < gpu::kTssCount) {
    g_state.tss[stage][type] = v;
    W32(kTextureState + (stage * kStageStates + type) * 4, v);
  }
  return 0;
}

namespace {
// the XDK's transform types: view 0, projection 1, texture 2 to 5, world 6 to
// 9; the renderer keeps Direct3D 8's numbers (view 2, projection 3, texture 16,
// world 256)
uint32_t D3D8TransformType(uint32_t t) {
  if (t < 2)
    return t + 2;
  if (t < 6)
    return t + 14;
  return t + 250;
}
} // namespace

// the device copy is what GetTransform reads
HLE_STDCALL(D3DDevice_SetTransform, 2) {
  uint32_t t = a[0];
  if (t >= 10)
    return 0;
  std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
  uint32_t pc = D3D8TransformType(t);
  std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
  return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
  std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
  return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
  uint32_t i = a[0];
  uint32_t l = a[1];
  if (i < 8 && l) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
  }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
  uint32_t i = a[0];
  uint32_t on = a[1];
  if (i < 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.light_enable =
        on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
  }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t stage = a[0] & 3;
  uint32_t tex = a[1];
  uint32_t old = Dev(kDevTextures + stage * 4);
  // bound again: the AddRef and Release would cancel out
  bool same = tex == old;
  if (!same)
    AddRef(ctx, tex);
  SetDev(kDevTextures + stage * 4, tex);
  if (!same)
    Release(ctx, old);
  g_state.textures[stage] = tex;
  W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
  return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
  XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t stage = a[0] & 3;
  uint32_t pal = a[1];
  uint32_t old = Dev(kDevPalettes + stage * 4);
  AddRef(ctx, pal);
  SetDev(kDevPalettes + stage * 4, pal);
  Release(ctx, old);
  g_state.palettes[stage] = pal;
  return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the
// bound one's shape replaces it. It is bound through a header of the stage's
// own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  static uint32_t headers[4];
  // texture methods from 0x1B00, 0x40 per stage
  uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3;
  uint32_t cur = g_state.textures[stage];
  uint32_t &h = headers[stage];
  if (!h)
    h = mem::AllocSystem(kHeaderBytes);
  if (cur && cur != h)
    std::memcpy(HostPtr(h), HostPtr(cur), kHeaderBytes);
  W32(h + kResData, a[1]);
  W32(h + kContainerFormat, a[2]);
  g_state.textures[stage] = h;
  return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t n = a[0] & 15;
  uint32_t vb = a[1];
  uint32_t stride = a[2];
  uint32_t stream = kStreams + n * kStreamSize;
  uint32_t old = R32(stream + XDK_Stream_Buffer);
  bool same = vb == old;
  if (!same)
    AddRef(ctx, vb);
  W32(stream + XDK_Stream_Stride, stride);
  W32(stream + XDK_Stream_Buffer, vb);
  if (!same)
    Release(ctx, old);
  g_state.streams[n] = {vb, stride};
  return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
  XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t ib = a[0];
  uint32_t old = Dev(kDevIndexBuffer);
  bool same = ib == old;
  if (!same)
    AddRef(ctx, ib);
  W32(kIndexData, ib ? R32(ib + kResData) : 0);
  SetDev(kDevIndexBuffer, ib);
  SetDev(kDevBaseVertex, a[1]);
  if (!same)
    Release(ctx, old);
  g_state.index_buffer = ib;
  g_state.index_data = ib ? R32(ib + kResData) : 0;
  g_state.base_vertex = a[1];
  return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t decl = a[0];
  uint32_t func = a[1];
  uint32_t out = a[2];
  auto vs = std::make_shared<gpu::VertexShader>();
  if (decl) {
    for (uint32_t i = 0; i < 1024; i++) {
      uint32_t t = R32(decl + i * 4);
      vs->decl.push_back(t);
      if (t == 0xFFFFFFFF)
        break;
    }
  }
  if (func) {
    uint32_t hdr = R32(func);
    vs->program_type = hdr & 0xFFFF;
    uint32_t n = hdr >> 16;
    for (uint32_t i = 0; i < n * 4; i++)
      vs->program.push_back(R32(func + 4 + i * 4));
  }
  gpu::ParseDeclaration(*vs);
  uint32_t rec = mem::AllocSystem(16, 16);
  vs->handle = rec | 1;
  g_vs[vs->handle] = vs;
  W32(out, vs->handle);
  LOG_FIRST(
      64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X",
      vs->handle, vs->decl.size(), vs->program.size() / 4, vs->program_type);
  return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  auto it = g_vs.find(a[0]);
  if (it != g_vs.end()) {
    mem::FreeSystem(a[0] & ~1u);
    g_vs.erase(it);
  }
  return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t h = a[0];
  g_state.vs_handle = h;
  g_state.vs = nullptr;
  if (h & 1) {
    auto it = g_vs.find(h);
    if (it != g_vs.end()) {
      g_state.vs = it->second;
      // constants declared with the shader load when it is selected
      for (auto &[reg, v] : it->second->constants) {
        if (reg < kVsConstants)
          g_state.vsc[reg] = v;
      }
    } else {
      LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
    }
  }
  return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
  uint32_t reg = a[0];
  if (reg < kVsConstants)
    std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
  return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
  uint32_t reg = a[0];
  if (reg < kVsConstants)
    std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
  return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
  uint32_t reg = a[0];
  uint32_t src = a[1];
  for (uint32_t i = 0; i < 4 && reg + i < kVsConstants; i++) {
    std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
  }
  return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
  uint32_t reg = a[0];
  uint32_t src = a[1];
  uint32_t dwords = a[2];
  for (uint32_t i = 0; i < dwords / 4 && reg + i < kVsConstants; i++) {
    std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
  }
  return 0;
}

HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
  LOG_FIRST(4, "d3d: LoadVertexShader %08X at %u is not supported", a[0], a[1]);
  return 0;
}
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
  LOG_FIRST(4, "d3d: SelectVertexShader %08X at %u is not supported", a[0],
            a[1]);
  return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) { return 0; }

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  auto ps = std::make_shared<gpu::PixelShader>();
  std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
  uint32_t rec = mem::AllocSystem(16, 16);
  ps->handle = rec;
  g_ps[rec] = ps;
  W32(a[1], rec);
  return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (g_ps.erase(a[0]))
    mem::FreeSystem(a[0]);
  return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  g_state.ps = nullptr;
  if (!a[0])
    return 0;
  auto it = g_ps.find(a[0]);
  if (it != g_ps.end()) {
    g_state.ps = it->second;
  } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
    // made by library code the title inlined: the object points at its
    // definition, handles are reused
    static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
    const void *def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
    auto &ps = made[a[0]];
    if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
      ps = std::make_shared<gpu::PixelShader>();
      ps->handle = a[0];
      std::memcpy(ps->def, def, sizeof(ps->def));
    }
    g_state.ps = ps;
  } else {
    LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
    return 0;
  }
  // the definition loads the combiner state like the XDK pushes it
  const uint32_t *d = g_state.ps->def;
  auto put = [&](uint32_t method, uint32_t v) {
    g_state.simple[method >> 2] = v;
  };
  for (int i = 0; i < 8; i++) {
    put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
    put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
    put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
    put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
    put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
    put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
  }
  put(gpu::kMPsFinalAbcd, d[8]);
  put(gpu::kMPsFinalEfgh, d[9]);
  put(gpu::kMPsCompareMode, d[42]);
  put(gpu::kMPsFinalConstant0, d[43]);
  put(gpu::kMPsFinalConstant1, d[44]);
  put(gpu::kMPsCombinerCount, d[53]);
  put(gpu::kMPsDotMapping, d[55]);
  put(gpu::kMPsInputTexture, d[56]);
  SetComplex(gpu::kRsPsTextureModes, d[54]);
  return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  uint32_t reg = a[0];
  uint32_t src = a[1];
  uint32_t n = a[2];
  for (uint32_t i = 0; i < n && reg + i < kPsConstants; i++) {
    std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
  }
  gpu::ApplyPixelConstants(g_state, reg, n);
  return 0;
}

// drawing

// First-person gun-list probe: logs what the title draws for its own viewmodel (gun, arms, flash)
// so the arm geometry can be told apart by texture / vertex layout.
namespace d3d {
namespace {
std::atomic<int> g_gun_mode{0};
uint32_t g_probe_seq = 0;
} // namespace

bool IsBlackArmsDraw(uint32_t prim, uint32_t count) {
  if (prim != 6u || g_state.streams[0].stride != 36u)
    return false;
  if (count < 2850u || count > 2980u)
    return false;
  uint32_t th = g_state.textures[0];
  if (!th || !mem::IsCommitted(th) || !mem::IsCommitted(th + kHeaderBytes - 1))
    return false;
  uint32_t fmt = R32(th + kContainerFormat);
  uint32_t tw = 1u << ((fmt >> 24) & 0xFu);
  uint32_t th_px = 1u << ((fmt >> 20) & 0xFu);
  return tw == 512u && th_px == 512u;
}

void ProbeGunDraw(const char *kind, uint32_t prim, uint32_t count, const uint16_t *indices) {
  (void)indices;
  if (g_gun_mode.load(std::memory_order_relaxed) != 1)
    return;
  uint32_t th = g_state.textures[0];
  uint32_t data = 0, fmt = 0, hash = 0;
  uint32_t tex_va = 0;
  if (th && mem::IsCommitted(th) && mem::IsCommitted(th + kHeaderBytes - 1)) {
    data = R32(th + kResData);
    fmt = R32(th + kContainerFormat);
    tex_va = mem::VirtualFromPhysical(data);
    hash = 2166136261u;
    if (tex_va && mem::IsCommitted(tex_va) && mem::IsCommitted(tex_va + 255)) {
      for (uint32_t i = 0; i < 256; i += 4) {
        hash = (hash ^ R32(tex_va + i)) * 16777619u;
      }
    }
  }
  static uint64_t s_seen[32] = {};
  static int s_seen_n = 0;
  uint64_t key = (uint64_t(hash) << 32) | count;
  for (int i = 0; i < s_seen_n; ++i) {
    if (s_seen[i] == key) return;
  }
  if (s_seen_n < 32) s_seen[s_seen_n++] = key;
  uint32_t tw = 1u << ((fmt >> 24) & 0xFu);
  uint32_t th_px = 1u << ((fmt >> 20) & 0xFu);
  uint32_t tfmt = (fmt >> 8) & 0xFFu;
  LOGR(
      "[FPPROBE] #%u %s prim=%u n=%u tex=%ux%u fmt=%02X hash=%08X "
      "stride=%u vs=%08X p00=%.3f p11=%.3f arms=%d",
      g_probe_seq++, kind, prim, count, tw, th_px, tfmt, hash,
      g_state.streams[0].stride, g_state.vs_handle,
      g_state.transforms[3][0], g_state.transforms[3][5],
      IsBlackArmsDraw(prim, count) ? 1 : 0);
}
void SetGunListMode(int mode) { g_gun_mode.store(mode, std::memory_order_relaxed); }
int GunListMode() { return g_gun_mode.load(std::memory_order_relaxed); }
} // namespace d3d

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  SyncDeferred();
  if (d3d::GunListMode() != 0)
    return 0;
  d3d::ProbeGunDraw("VTX", a[0], a[2], nullptr);
  if (g_backend)
    g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
  return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  SyncDeferred();
  if (d3d::GunListMode() == 1 ||
      (d3d::GunListMode() == 2 && !d3d::IsBlackArmsDraw(a[0], a[1])))
    return 0;
  const uint16_t *idx16 =
      a[2] ? reinterpret_cast<const uint16_t *>(HostPtr(a[2])) : nullptr;
  d3d::ProbeGunDraw("IDX", a[0], a[1], idx16);
  const uint8_t *indices = reinterpret_cast<const uint8_t *>(idx16);
  if (g_backend)
    g_backend->Draw(g_state, a[0], 0, a[1], indices);
  return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a
// ring the render thread reads, stream 0 points there through a vertex buffer
// header for the draw. A half of the ring is written again once the draws that
// read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20;
constexpr uint32_t kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0;
uint32_t g_up_pos = 0;
uint32_t g_up_header = 0;
uint32_t g_up_fence[2] = {};

uint32_t UpVertices(const void *src, uint32_t bytes) {
  if (!g_up_ring) {
    g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
    g_up_header = mem::AllocSystem(kHeaderBytes);
    std::memset(HostPtr(g_up_header), 0, kHeaderBytes);
    W32(g_up_header + kResCommon, 1); // a vertex buffer, one reference
  }
  bytes = std::min(bytes, kUpHalf);
  uint32_t room = (bytes + 63) & ~63u;
  uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
  // the halves the last copy ended in and this one ends in
  uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1;
  uint32_t to = (pos + room - 1) / kUpHalf;
  if (to != from) {
    g_up_fence[from] = g_backend->Fence();
    g_backend->WaitFence(g_up_fence[to]);
  }
  std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
  g_up_pos = pos + room;
  W32(g_up_header + kResData, mem::PhysicalFromVirtual(g_up_ring + pos));
  return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void *vertices,
            uint32_t stride) {
  if (!g_backend || !count || !stride)
    return;
  SyncDeferred();
  auto stream = g_state.streams[0];
  g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
  g_backend->Draw(g_state, prim, 0, count, nullptr);
  g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's
// declaration
uint32_t VertexBytes() {
  if (g_state.vs_handle & 1)
    return g_state.vs ? g_state.vs->stream_stride[0] : 0;
  gpu::VertexShader fvf;
  gpu::FvfInputs(g_state.vs_handle, fvf);
  return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
} // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so
// their commands are read from the scratch buffer.
namespace d3d {
namespace {
// push buffer methods: begin/end, inline vertices, vertex data of 2 floats, 4
// bytes and 4 floats per slot
constexpr uint32_t kMBeginEnd = 0x17FC;
constexpr uint32_t kMInlineArray = 0x1818;
constexpr uint32_t kMVertexData2F = 0x1880;
constexpr uint32_t kMVertexData2FEnd = 0x1900;
constexpr uint32_t kMVertexData4UB = 0x1940;
constexpr uint32_t kMVertexData4UBEnd = 0x1980;
constexpr uint32_t kMVertexData4F = 0x1A00;
constexpr uint32_t kMVertexData4FEnd = 0x1B00;

constexpr uint32_t kImSlots = 16;
constexpr uint32_t kImFloats = kImSlots * 4;

uint32_t g_im_prim = 0;           // open primitive
uint32_t g_im_mask = 0;           // slots it wrote
float g_im_attr[kImFloats];       // 16 slots of x y z w
std::vector<float> g_im_vertices; // every slot per vertex

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound
// program, or of the fixed pipeline
void EndPrimitive() {
  struct Made {
    std::shared_ptr<gpu::VertexShader> from;
    std::shared_ptr<gpu::VertexShader> vs;
  };
  // the render thread tells shaders apart by pointer: one object per program
  // and slot set, the program kept alive
  static std::map<std::pair<gpu::VertexShader *, uint32_t>, Made> made;
  static std::vector<float> packed;
  uint32_t prim = g_im_prim;
  uint32_t count = uint32_t(g_im_vertices.size() / kImFloats);
  uint32_t handle = g_state.vs_handle;
  g_im_prim = 0;
  if (!prim || !count)
    return;
  bool pretransformed = !(handle & 1) && (handle & 0xE) == 4; // D3DFVF_XYZRHW
  if (pretransformed)
    LOG_FIRST(1,
              "d3d: pretransformed immediate mode vertices are not supported");
  auto bound = g_state.vs;
  auto from = handle & 1 ? bound : nullptr;
  Made &m = made[{from.get(), g_im_mask}];
  if (!m.vs) {
    m.from = from;
    m.vs = std::make_shared<gpu::VertexShader>();
    if (from) {
      m.vs->program = from->program;
      m.vs->program_type = from->program_type;
    }
    for (uint32_t s = 0; s < kImSlots; s++) {
      if (!(g_im_mask >> s & 1))
        continue;
      m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42}; // FLOAT4
      m.vs->stream_stride[0] += 16;
    }
  }
  packed.clear();
  for (uint32_t v = 0; v < count; v++) {
    for (uint32_t s = 0; s < kImSlots; s++) {
      const float *slot = &g_im_vertices[v * kImFloats + s * 4];
      if (g_im_mask >> s & 1)
        packed.insert(packed.end(), slot, slot + 4);
    }
  }
  g_state.vs = m.vs;
  g_state.vs_handle = handle | 1;
  DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
  g_state.vs = bound;
  g_state.vs_handle = handle;
}

// one value of a method: begin/end opens or draws the primitive, vertex data
// fills a slot
void ApplyImmediateMethod(uint32_t m, uint32_t v) {
  float f;
  float *a = g_im_attr;
  bool last = true; // the slot's last component
  std::memcpy(&f, &v, 4);
  if (m == kMBeginEnd) { // 0 ends
    if (v) {
      g_im_prim = v;
      g_im_mask = 0;
      g_im_vertices.clear();
    } else {
      EndPrimitive();
    }
    return;
  }
  if (m >= kMVertexData2F &&
      m < kMVertexData2FEnd) { // 2 floats, z = 0 and w = 1
    a += (m - kMVertexData2F) / 8 * 4;
    last = (m - kMVertexData2F) / 4 & 1;
    a[last] = f;
    a[2] = 0;
    a[3] = 1;
  } else if (m >= kMVertexData4UB && m < kMVertexData4UBEnd) { // 4 bytes
    a += (m - kMVertexData4UB) / 4 * 4;
    for (int c = 0; c < 4; c++)
      a[c] = (v >> c * 8 & 0xFF) / 255.0f;
  } else if (m >= kMVertexData4F && m < kMVertexData4FEnd) { // 4 floats
    uint32_t component = (m - kMVertexData4F) / 4 & 3;
    a += (m - kMVertexData4F) / 16 * 4;
    last = component == 3;
    a[component] = f;
  } else {
    return;
  }
  if (!g_im_prim)
    return;
  g_im_mask |= 1u << (a - g_im_attr) / 4;
  // slot 0, the position, completes a vertex
  if (a == g_im_attr && last)
    g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + kImFloats);
}

// reads the commands pushed since the last call: vertex data between a begin
// and an end is drawn, the rest is skipped
void Walk(uint32_t end) {
  if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size)
    return;
  for (uint32_t p = g_walk; p + 4 <= end;) {
    // count in bits 18 to 28, bit 30 keeps the method for every value
    uint32_t head = R32(p);
    uint32_t method = head & 0x1FFC;
    uint32_t n = (head >> 18) & 0x7FF;
    p += 4;
    if ((head & 0xA0030003) || n * 4 > end - p)
      break; // anything but a method header
    for (uint32_t i = 0; i < n; i++, p += 4) {
      uint32_t m = head & 0x40000000 ? method : method + i * 4;
      ApplyImmediateMethod(m, R32(p));
    }
  }
  g_walk = end;
}
} // namespace
} // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
  return 0;
}

static void DrawPushedPrimitive(uint32_t prim,
                                const std::vector<uint32_t> &vertices) {
  uint32_t stride = VertexBytes();
  uint32_t bytes = uint32_t(vertices.size()) * 4;
  if (stride && bytes % stride == 0) {
    DrawUp(prim, bytes / stride, vertices.data(), stride);
  } else {
    LOG_FIRST(8,
              "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u",
              prim, bytes, stride);
  }
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs
// what the title wrote there: a primitive between two begin/end writes with its
// vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  g_push_begin = ScratchPush(a[0] * 4);
  return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  static std::vector<uint32_t> vertices;
  uint32_t prim = 0;
  uint32_t end = a[0];
  for (uint32_t p = g_push_begin; g_push_begin && p + 4 <= end;) {
    // count in bits 18 to 28, bit 30 keeps the method for every value
    uint32_t head = R32(p);
    uint32_t method = head & 0x1FFC;
    uint32_t n = (head >> 18) & 0x7FF;
    p += 4;
    if ((head & 0xA0030003) ||
        n * 4 > end - p) { // anything but a method header
      LOG_FIRST(8,
                "d3d: EndPush stops at command %08X, %u bytes before the end",
                head, end - p);
      break;
    }
    for (uint32_t i = 0; i < n; i++, p += 4) {
      uint32_t m = head & 0x40000000 ? method : method + i * 4;
      uint32_t v = R32(p);
      if (m == kMInlineArray) {
        if (prim)
          vertices.push_back(v);
      } else if (m != kMBeginEnd) {
        if (m >> 2 < g_state.simple.size())
          g_state.simple[m >> 2] = v;
      } else if (v) {
        prim = v;
        vertices.clear();
      } else if (prim) {
        DrawPushedPrimitive(prim, vertices);
        prim = 0;
      }
    }
  }
  g_push_begin = 0;
  ScratchPush();
  return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

HLE_STDCALL(D3DDevice_Begin, 1) {
  LOG_FIRST(4, "d3d: Begin/End immediate mode is not supported");
  return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
  XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  Walk(Dev(kDevPush));
  EndPrimitive();
  SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
  return 0;
}

// [[hook]] behind an End the title inlined
void EndImmediate(X86Context &, uint8_t *) {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  Walk(Dev(kDevPush));
}

namespace d3d {

void DrawWorldOverlay(const WorldVertex *verts, uint32_t vertex_count,
                      const float view[16], const float proj[16],
                      bool additive) {
  if (!g_backend || !verts || vertex_count < 3 || !view || !proj)
    return;
  // Align down to full triangles
  uint32_t tri_verts = vertex_count - (vertex_count % 3u);
  if (tri_verts < 3)
    return;

  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (!g_state.rt.header || !g_state.zs.header)
    return;
  // Sync pending guest deferred state before snapshotting so we restore exact
  // live state
  SyncDeferred();

  const auto saved_stream = g_state.streams[0];
  const auto saved_textures = g_state.textures;
  const uint32_t saved_vs_handle = g_state.vs_handle;
  const auto saved_vs = g_state.vs;
  const auto saved_ps = g_state.ps;
  const uint32_t saved_base_vertex = g_state.base_vertex;
  const uint32_t saved_scissor_count = g_state.scissor_count;
  const auto saved_view = g_state.transforms[2];
  const auto saved_proj = g_state.transforms[3];
  const auto saved_world = g_state.transforms[256];

  const uint32_t saved_zen = g_state.rs[gpu::kRsZEnable];
  const uint32_t saved_cull = g_state.rs[gpu::kRsCullMode];
  const uint32_t saved_sten = g_state.rs[gpu::kRsStencilEnable];
  const uint32_t saved_fill = g_state.rs[gpu::kRsFillMode];
  const uint32_t saved_light = g_state.rs[gpu::kRsLighting];
  const uint32_t saved_fog = g_state.rs[gpu::kRsFogEnable];
  const uint32_t saved_two = g_state.rs[gpu::kRsTwoSidedLighting];

  const uint32_t saved_zwrite = g_state.simple[gpu::kMZWriteEnable >> 2];
  const uint32_t saved_zfunc = g_state.simple[gpu::kMZFunc >> 2];
  const uint32_t saved_blend = g_state.simple[gpu::kMBlendEnable >> 2];
  const uint32_t saved_src = g_state.simple[gpu::kMSrcBlend >> 2];
  const uint32_t saved_dst = g_state.simple[gpu::kMDestBlend >> 2];
  const uint32_t saved_op = g_state.simple[gpu::kMBlendOp >> 2];
  const uint32_t saved_atest = g_state.simple[gpu::kMAlphaTestEnable >> 2];
  const uint32_t saved_afunc = g_state.simple[gpu::kMAlphaFunc >> 2];
  const uint32_t saved_cwrite = g_state.simple[gpu::kMColorWriteEnable >> 2];
  const uint32_t saved_shade = g_state.simple[gpu::kMShadeMode >> 2];
  const uint32_t saved_solid_off =
      g_state.simple[gpu::kMSolidOffsetEnable >> 2];

  const ScopedOverlayExtraState extra_state_guard;
  const uint32_t saved_tss0_cop = g_state.tss[0][gpu::kTssColorOp];
  const uint32_t saved_tss0_aop = g_state.tss[0][gpu::kTssAlphaOp];
  const uint32_t saved_tss0_akill = g_state.tss[0][gpu::kTssAlphaKill];

  // Configure untextured fixed-function FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE =
  // 0x42)
  std::memcpy(g_state.transforms[2].data(), view, 64);
  std::memcpy(g_state.transforms[3].data(), proj, 64);
  g_state.transforms[256] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  g_state.textures.fill(0);
  g_state.vs_handle = 0x42u;
  g_state.vs = nullptr;
  g_state.ps = nullptr;
  g_state.base_vertex = 0;
  g_state.scissor_count = 0;

  g_state.rs[gpu::kRsZEnable] = 1;
  g_state.rs[gpu::kRsCullMode] =
      0; // CULL_NONE: camera-aligned quads are double-sided
  g_state.rs[gpu::kRsStencilEnable] = 0;
  g_state.rs[gpu::kRsFillMode] = 0x1B02u; // D3DFILL_SOLID
  g_state.rs[gpu::kRsLighting] = 0;
  g_state.rs[gpu::kRsFogEnable] = 0;
  g_state.rs[gpu::kRsTwoSidedLighting] = 0;

  g_state.simple[gpu::kMZWriteEnable >> 2] = 0;
  g_state.simple[gpu::kMZFunc >> 2] = 0x203u; // D3DCMP_LESSEQUAL
  g_state.simple[gpu::kMBlendEnable >> 2] = 1;
  g_state.simple[gpu::kMSrcBlend >> 2] = 0x302u; // D3DBLEND_SRCALPHA
  g_state.simple[gpu::kMDestBlend >> 2] =
      additive ? 1u : 0x303u;                    // ONE vs INVSRCALPHA
  g_state.simple[gpu::kMBlendOp >> 2] = 0x8006u; // D3DBLENDOP_ADD
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = 0;
  g_state.simple[gpu::kMAlphaFunc >> 2] = 0x207u; // D3DCMP_ALWAYS
  g_state.simple[gpu::kMColorWriteEnable >> 2] =
      0x01010101u;                                 // RGBA all enabled
  g_state.simple[gpu::kMShadeMode >> 2] = 0x1D01u; // D3DSHADE_GOURAUD
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = 0;

  g_state.tss[0][gpu::kTssColorOp] =
      1; // D3DTOP_DISABLE -> output v0 diffuse directly
  g_state.tss[0][gpu::kTssAlphaOp] = 1;
  g_state.tss[0][gpu::kTssAlphaKill] = 0;

  constexpr uint32_t kChunkVerts = 3072u; // Multiple of 3 (~48 KB < kUpHalf)
  for (uint32_t off = 0; off < tri_verts; off += kChunkVerts) {
    uint32_t chunk = std::min(kChunkVerts, tri_verts - off);
    uint32_t hdr =
        UpVertices(verts + off, chunk * uint32_t(sizeof(WorldVertex)));
    g_state.streams[0] = {hdr, uint32_t(sizeof(WorldVertex))};
    g_backend->Draw(g_state, 5 /* D3DPT_TRIANGLELIST */, 0, chunk, nullptr);
  }

  // Restore exact original D3D8 state
  g_state.streams[0] = saved_stream;
  g_state.textures = saved_textures;
  g_state.vs_handle = saved_vs_handle;
  g_state.vs = saved_vs;
  g_state.ps = saved_ps;
  g_state.base_vertex = saved_base_vertex;
  g_state.scissor_count = saved_scissor_count;
  g_state.transforms[2] = saved_view;
  g_state.transforms[3] = saved_proj;
  g_state.transforms[256] = saved_world;

  g_state.rs[gpu::kRsZEnable] = saved_zen;
  g_state.rs[gpu::kRsCullMode] = saved_cull;
  g_state.rs[gpu::kRsStencilEnable] = saved_sten;
  g_state.rs[gpu::kRsFillMode] = saved_fill;
  g_state.rs[gpu::kRsLighting] = saved_light;
  g_state.rs[gpu::kRsFogEnable] = saved_fog;
  g_state.rs[gpu::kRsTwoSidedLighting] = saved_two;

  g_state.simple[gpu::kMZWriteEnable >> 2] = saved_zwrite;
  g_state.simple[gpu::kMZFunc >> 2] = saved_zfunc;
  g_state.simple[gpu::kMBlendEnable >> 2] = saved_blend;
  g_state.simple[gpu::kMSrcBlend >> 2] = saved_src;
  g_state.simple[gpu::kMDestBlend >> 2] = saved_dst;
  g_state.simple[gpu::kMBlendOp >> 2] = saved_op;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = saved_atest;
  g_state.simple[gpu::kMAlphaFunc >> 2] = saved_afunc;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = saved_cwrite;
  g_state.simple[gpu::kMShadeMode >> 2] = saved_shade;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = saved_solid_off;

  g_state.tss[0][gpu::kTssColorOp] = saved_tss0_cop;
  g_state.tss[0][gpu::kTssAlphaOp] = saved_tss0_aop;
  g_state.tss[0][gpu::kTssAlphaKill] = saved_tss0_akill;
}

namespace {

uint32_t g_vfx_atlas_hdr = 0;

inline float Hash2D(int ix, int iy, uint32_t seed) {
  uint32_t h = uint32_t(ix) * 374761393u + uint32_t(iy) * 668265263u +
               seed * 1442695041u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

inline float ValueNoise2D(float x, float y, uint32_t seed) {
  int ix = int(std::floor(x));
  int iy = int(std::floor(y));
  float fx = x - float(ix);
  float fy = y - float(iy);
  float ux = fx * fx * (3.0f - 2.0f * fx);
  float uy = fy * fy * (3.0f - 2.0f * fy);
  float a = Hash2D(ix, iy, seed);
  float b = Hash2D(ix + 1, iy, seed);
  float c = Hash2D(ix, iy + 1, seed);
  float d = Hash2D(ix + 1, iy + 1, seed);
  return (a * (1.0f - ux) + b * ux) * (1.0f - uy) +
         (c * (1.0f - ux) + d * ux) * uy;
}

inline float Fbm2D(float x, float y, uint32_t seed, int octaves = 4) {
  float sum = 0.0f;
  float amp = 0.5f;
  float freq = 1.0f;
  for (int i = 0; i < octaves; i++) {
    sum += amp * ValueNoise2D(x * freq, y * freq, seed + uint32_t(i) * 97u);
    freq *= 2.03f;
    amp *= 0.5f;
  }
  return sum;
}

uint32_t EnsureVfxAtlasTexture() {
  if (g_vfx_atlas_hdr)
    return g_vfx_atlas_hdr;
  constexpr uint32_t kAtlasSize = 1024u;
  constexpr uint32_t kTileSize = 256u;
  const uint32_t data_va = mem::AllocSystem(kAtlasSize * kAtlasSize * 4u, 256u);
  if (!data_va)
    return 0;

  uint32_t *px = reinterpret_cast<uint32_t *>(HostPtr(data_va));
  if (!px)
    return 0;

  for (uint32_t ty = 0; ty < 4; ty++) {
    for (uint32_t tx = 0; tx < 4; tx++) {
      const uint32_t tile_seed = (ty * 4u + tx) * 97u + 13u;
      for (uint32_t py = 0; py < kTileSize; py++) {
        const float v_tile = (float(py) + 0.5f) / float(kTileSize);
        const float uy = v_tile * 2.0f - 1.0f;
        for (uint32_t px_i = 0; px_i < kTileSize; px_i++) {
          const float u_tile = (float(px_i) + 0.5f) / float(kTileSize);
          const float ux = u_tile * 2.0f - 1.0f;
          const uint32_t gx = tx * kTileSize + px_i;
          const uint32_t gy = ty * kTileSize + py;
          uint32_t bgra = 0u;

          if (ty <= 1u) {
            bgra =
                game::ballistics::SampleVfxAtlasTexelBgra(tx, ty, u_tile, v_tile);
          } else {
            auto clamp01 = [](float v) -> float {
              return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            };
            auto hash2 = [](uint32_t hx, uint32_t hy, uint32_t seed) -> float {
              uint32_t n =
                  hx * 374761393u + hy * 668265263u + seed * 1442695041u;
              n = (n ^ (n >> 13)) * 1274126177u;
              n ^= (n >> 16);
              return float(n & 0xFFFFu) / 65535.0f;
            };
            auto vnoise = [&](float nx, float ny, uint32_t seed) -> float {
              const int ix = (nx >= 0.0f) ? int(nx) : (int(nx) - 1);
              const int iy = (ny >= 0.0f) ? int(ny) : (int(ny) - 1);
              float fx = nx - float(ix);
              float fy = ny - float(iy);
              fx = fx * fx * (3.0f - 2.0f * fx);
              fy = fy * fy * (3.0f - 2.0f * fy);
              const uint32_t ux0 = uint32_t(ix) & 255u;
              const uint32_t uy0 = uint32_t(iy) & 255u;
              const uint32_t ux1 = (ux0 + 1u) & 255u;
              const uint32_t uy1 = (uy0 + 1u) & 255u;
              const float a = hash2(ux0, uy0, seed);
              const float b = hash2(ux1, uy0, seed);
              const float c = hash2(ux0, uy1, seed);
              const float d = hash2(ux1, uy1, seed);
              const float ab = a + (b - a) * fx;
              const float cd = c + (d - c) * fx;
              return ab + (cd - ab) * fy;
            };
            auto fbm = [&](float nx, float ny, uint32_t seed) -> float {
              return vnoise(nx, ny, seed) * 0.54f +
                     vnoise(nx * 2.03f + 5.7f, ny * 2.03f - 3.1f, seed + 17u) *
                         0.30f +
                     vnoise(nx * 4.11f - 9.2f, ny * 4.11f + 7.4f, seed + 53u) *
                         0.16f;
            };
            const float r2 = ux * ux + uy * uy;
            if (ty == 2u) {
              // Row 2 (Tiles 8..11): Specialized ballistic blood atlas:
              //   Tile 8 (tx=0): Atomized arterial aerosol mist + cellular micro-droplets
              //   Tile 9 (tx=1): High-velocity directional arterial spurt & liquid ligaments
              //   Tile 10 (tx=2): Coagulating ground pool with crisp surface-tension meniscus
              //   Tile 11 (tx=3): Ballistic impact spatter with radial crown spines & satellite drops
              const float rad = std::sqrt(r2 + 1e-6f);
              const float dir_x = ux / rad;
              const float dir_y = uy / rad;
              const float border_guard = clamp01((0.95f - rad) * 9.0f);

              auto droplet_field = [&](float freq, float density,
                                       float radial_stretch,
                                       uint32_t d_seed) -> float {
                const float gx = (ux * 0.866f - uy * 0.500f) * freq;
                const float gy = (ux * 0.500f + uy * 0.866f) * freq;
                const int ix = (gx >= 0.0f) ? int(gx) : (int(gx) - 1);
                const int iy = (gy >= 0.0f) ? int(gy) : (int(gy) - 1);
                const float fx = gx - float(ix);
                const float fy = gy - float(iy);
                const float rx = dir_x * 0.866f - dir_y * 0.500f;
                const float ry = dir_x * 0.500f + dir_y * 0.866f;
                float best = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                  for (int dx = -1; dx <= 1; ++dx) {
                    const uint32_t cx = uint32_t(ix + dx) & 255u;
                    const uint32_t cy = uint32_t(iy + dy) & 255u;
                    if (hash2(cx, cy, d_seed) >= density)
                      continue;
                    const float jx =
                        float(dx) + 0.18f + 0.64f * hash2(cx, cy, d_seed + 31u) - fx;
                    const float jy =
                        float(dy) + 0.18f + 0.64f * hash2(cx, cy, d_seed + 73u) - fy;
                    const float along = (jx * rx + jy * ry) / radial_stretch;
                    const float perp = -jx * ry + jy * rx;
                    const float d2 = along * along + perp * perp;
                    const float drop_r =
                        0.14f + 0.28f * hash2(cx, cy, d_seed + 127u);
                    const float val = clamp01((drop_r * drop_r - d2) * 18.0f);
                    if (val > best)
                      best = val;
                  }
                }
                return best;
              };

              float alpha_f = 0.0f;
              float thick = 0.70f;
              float meniscus = 0.0f;

              if (tx == 0u) {
                const float wx =
                    ux + (fbm(ux * 3.0f, uy * 3.0f, tile_seed + 11u) - 0.5f) * 0.35f;
                const float wy =
                    uy + (fbm(ux * 3.0f + 4.2f, uy * 3.0f - 2.8f, tile_seed + 23u) -
                          0.5f) *
                             0.35f;
                const float cloud =
                    fbm(wx * 3.6f + 1.7f, wy * 3.6f - 2.3f, tile_seed);
                const float rays = fbm(dir_x * 4.5f + wx * 2.2f,
                                       dir_y * 4.5f + wy * 2.2f, tile_seed + 101u);
                const float core_falloff = clamp01(
                    1.0f - (wx * wx + wy * wy) * (1.05f + 0.70f * (1.0f - cloud)));
                const float aerosol = clamp01(
                    (core_falloff * (0.45f + 0.55f * rays) - 0.10f) * 1.40f);
                const float drops_med =
                    droplet_field(7.5f, 0.72f, 1.35f, tile_seed + 211u) *
                    clamp01(1.0f - r2 * 1.08f);
                const float drops_fine =
                    droplet_field(14.0f, 0.65f, 1.15f, tile_seed + 307u) *
                    clamp01(1.0f - r2 * 0.96f);
                const float droplets = clamp01(drops_med + drops_fine * 0.85f);
                alpha_f =
                    clamp01((aerosol * 0.62f + droplets * 0.96f) * border_guard);
                meniscus = clamp01(droplets * 0.55f);
                thick = clamp01(0.58f + 0.38f * droplets - 0.18f * core_falloff);
              } else if (tx == 1u) {
                const float warp_x =
                    ux + (fbm(ux * 3.8f, uy * 3.8f, tile_seed + 41u) - 0.5f) * 0.32f;
                const float warp_y =
                    uy + (fbm(ux * 3.8f + 4.1f, uy * 3.8f - 2.7f, tile_seed + 83u) -
                          0.5f) *
                             0.24f;
                const float streak_r2 =
                    warp_x * warp_x * 2.25f + warp_y * warp_y * 0.86f;
                const float ligament =
                    fbm(warp_x * 5.2f, warp_y * 2.5f + 3.4f, tile_seed + 149u);
                const float ray_spines =
                    fbm(dir_x * 6.5f + warp_x * 2.4f,
                        dir_y * 6.5f + warp_y * 1.8f, tile_seed + 251u);
                const float body_sdf =
                    1.0f - streak_r2 * (0.85f + 0.92f * (1.0f - ligament)) +
                    (ray_spines - 0.48f) * 0.46f;
                const float sharp_body = clamp01((body_sdf - 0.18f) * 8.0f);
                const float satellites =
                    droplet_field(9.0f, 0.68f, 1.85f, tile_seed + 379u) *
                    clamp01(1.0f - r2 * 1.05f) * clamp01(r2 * 2.4f) *
                    (1.0f - sharp_body * 0.80f);
                alpha_f = clamp01(
                    std::max(sharp_body, satellites * 0.95f) * border_guard);
                meniscus = clamp01(
                    sharp_body * (1.0f - clamp01((body_sdf - 0.32f) * 5.5f)) *
                        0.85f +
                    satellites * 0.55f);
                thick = clamp01(
                    0.52f + 0.36f * (1.0f - clamp01((body_sdf - 0.35f) * 2.4f)) +
                    0.16f * meniscus);
              } else if (tx == 2u) {
                const float wx =
                    ux + (fbm(ux * 2.6f + 1.9f, uy * 2.6f - 1.4f, tile_seed + 29u) -
                          0.5f) *
                             0.40f;
                const float wy =
                    uy + (fbm(ux * 2.6f - 3.3f, uy * 2.6f + 2.8f, tile_seed + 71u) -
                          0.5f) *
                             0.40f;
                const float wr2 = wx * wx + wy * wy;
                const float lobe = fbm(dir_x * 2.8f + wx * 1.9f,
                                       dir_y * 2.8f + wy * 1.9f, tile_seed + 131u);
                const float micro_edge =
                    fbm(wx * 6.4f, wy * 6.4f, tile_seed + 223u);
                const float pool_sdf = 0.74f + 0.34f * (lobe - 0.5f) +
                                       0.10f * (micro_edge - 0.5f) - wr2;
                const float sharp_pool = clamp01(pool_sdf * 12.5f);
                const float inner_depth = clamp01(pool_sdf * 2.5f);
                meniscus = clamp01(
                    sharp_pool * (1.0f - clamp01((pool_sdf - 0.075f) * 11.0f)));
                const float edge_drops =
                    droplet_field(7.0f, 0.45f, 1.25f, tile_seed + 419u) *
                    clamp01(1.0f - r2 * 1.04f) * clamp01(r2 * 2.8f) *
                    (1.0f - sharp_pool * 0.85f);
                alpha_f = clamp01(
                    std::max(sharp_pool * (0.94f + 0.06f * meniscus),
                             edge_drops * 0.92f) *
                    border_guard);
                thick = clamp01(0.46f + 0.38f * (1.0f - inner_depth) +
                                0.16f * meniscus);
              } else {
                const float wx =
                    ux + (fbm(ux * 3.5f - 1.2f, uy * 3.5f + 2.4f, tile_seed + 19u) -
                          0.5f) *
                             0.25f;
                const float wy =
                    uy + (fbm(ux * 3.5f + 3.1f, uy * 3.5f - 1.7f, tile_seed + 47u) -
                          0.5f) *
                             0.25f;
                const float crown = fbm(dir_x * 7.5f + wx * 1.8f,
                                        dir_y * 7.5f + wy * 1.8f, tile_seed + 67u);
                const float blobs =
                    fbm(wx * 4.2f - 2.1f, wy * 4.2f + 3.7f, tile_seed + 163u);
                const float spatter_sdf =
                    1.0f - (wx * wx + wy * wy) * (1.35f + 1.05f * (1.0f - blobs)) +
                    (crown - 0.45f) * 0.66f * clamp01(rad * 1.8f);
                const float main_splat = clamp01((spatter_sdf - 0.22f) * 9.5f);
                const float radial_drops =
                    (droplet_field(8.5f, 0.75f, 1.75f, tile_seed + 281u) +
                     droplet_field(14.5f, 0.65f, 1.45f, tile_seed + 409u) *
                         0.85f) *
                    clamp01(1.0f - r2 * 1.04f) * (1.0f - main_splat * 0.85f);
                alpha_f = clamp01(
                    std::max(main_splat, clamp01(radial_drops) * 0.96f) *
                    border_guard);
                meniscus = clamp01(
                    main_splat *
                        (1.0f - clamp01((spatter_sdf - 0.34f) * 6.5f)) * 0.80f +
                    clamp01(radial_drops) * 0.45f);
                thick = clamp01(
                    0.50f +
                    0.36f * (1.0f - clamp01((spatter_sdf - 0.30f) * 2.2f)) +
                    0.16f * meniscus);
              }

              // Modified Beer-Lambert exponential hemoglobin absorption law:
              // Optical depth d is highest in the thick coagulated core (1.0 - thick)
              // and lowest along the thin translucent ruby meniscus rim.
              const float opt_depth = clamp01(1.12f - 0.88f * thick);
              const float beer_r = std::exp(-1.55f * opt_depth);
              const float beer_g = std::exp(-5.60f * opt_depth);
              const float beer_b = std::exp(-6.20f * opt_depth);
              const float fresnel_glint = meniscus * meniscus * 0.085f;
              const uint32_t cr = uint32_t(
                  clamp01(0.18f + 0.58f * beer_r + fresnel_glint) * 255.0f + 0.5f);
              const uint32_t cg = uint32_t(
                  clamp01(0.006f + 0.045f * beer_g + fresnel_glint * 0.45f) * 255.0f +
                  0.5f);
              const uint32_t cb = uint32_t(
                  clamp01(0.008f + 0.050f * beer_b + fresnel_glint * 0.50f) * 255.0f +
                  0.5f);
              bgra = (uint32_t(alpha_f * 255.0f + 0.5f) << 24) | (cr << 16) |
                     (cg << 8) | cb;
            } else {
              // Row 3: Tiles 12-13 incandescent blackbody spark/ember cores with lens diffraction;
              // Tiles 14-15 supersonic Mach shockwave & Rayleigh-Taylor turbulent dust rings.
              const float rim_fade = clamp01(1.0f - r2 * 1.08f);
              if (tx < 2u) {
                const float core = std::exp(-r2 * 6.5f);
                const float halo = clamp01(1.0f - r2 * 1.12f);
                const float diag_u = (ux + uy) * 0.70710678f;
                const float diag_v = (ux - uy) * 0.70710678f;
                const float star_primary =
                    clamp01(1.0f - (ux * ux * 34.0f + uy * uy * 2.1f)) * halo +
                    clamp01(1.0f - (uy * uy * 34.0f + ux * ux * 2.1f)) * halo;
                const float star_diag =
                    (tx == 1u)
                        ? (clamp01(1.0f - (diag_u * diag_u * 48.0f + diag_v * diag_v * 3.0f)) +
                           clamp01(1.0f - (diag_v * diag_v * 48.0f + diag_u * diag_u * 3.0f))) *
                              halo * 0.55f
                        : 0.0f;
                const float ember_turb =
                    0.82f + 0.18f * vnoise(ux * 8.5f + float(tx) * 3.1f, uy * 8.5f, tile_seed);
                const float alpha_f = clamp01(
                    (halo * halo * 0.64f + core * 0.72f + (star_primary + star_diag) * 0.38f) *
                    ember_turb * rim_fade);
                const float temp = clamp01(core * 0.92f + star_primary * 0.28f + halo * 0.18f);
                const float r_f = 1.0f;
                const float g_f = clamp01(0.24f + 0.74f * std::pow(temp, 1.35f));
                const float b_f = clamp01(0.04f + 0.82f * std::pow(temp, 2.60f));
                bgra = (uint32_t(alpha_f * 255.0f + 0.5f) << 24) |
                       (uint32_t(r_f * 255.0f + 0.5f) << 16) |
                       (uint32_t(g_f * 255.0f + 0.5f) << 8) |
                       uint32_t(b_f * 255.0f + 0.5f);
              } else {
                const float rad = std::sqrt(r2 + 1e-6f);
                const float dx = ux / rad;
                const float dy = uy / rad;
                const float n0 = fbm(ux * 3.8f + float(tx) * 2.7f,
                                     uy * 3.8f - 1.9f, tile_seed);
                const float rt_fingers =
                    fbm(dx * 5.5f + ux * 2.4f, dy * 5.5f + uy * 2.4f, tile_seed + 79u);
                const float delta =
                    r2 - (0.46f + 0.14f * (n0 - 0.5f) + 0.08f * (rt_fingers - 0.5f));
                const float shock_ring = clamp01(1.0f - delta * delta * 22.0f);
                const float inner_wake =
                    clamp01(1.0f - r2 * (0.92f + 0.38f * (1.0f - n0))) * rt_fingers;
                const float alpha_f = clamp01(
                    (shock_ring * (0.66f + 0.34f * rt_fingers) + inner_wake * 0.28f) *
                    rim_fade * rim_fade);
                const float lum_f =
                    clamp01(0.52f + 0.36f * shock_ring + 0.14f * rt_fingers);
                const uint32_t ch = uint32_t(lum_f * 255.0f + 0.5f);
                bgra = (uint32_t(alpha_f * 255.0f + 0.5f) << 24) | (ch << 16) |
                       (ch << 8) | ch;
              }
            }
          }

          // Write swizzled Morton-ordered index for 1024x1024 XDK A8R8G8B8 texture.
          uint32_t morton = 0u;
          for (uint32_t bit = 0u; bit < 10u; ++bit) {
            morton |= ((gx >> bit) & 1u) << (bit * 2u);
            morton |= ((gy >> bit) & 1u) << (bit * 2u + 1u);
          }
          px[morton] = bgra;
        }
      }
    }
  }

  const uint32_t hdr = mem::AllocSystem(kHeaderBytes);
  if (!hdr)
    return 0;
  W32(hdr + 0, 0x40000u); // D3DCOMMON_TYPE_TEXTURE
  W32(hdr + kResData, mem::PhysicalFromVirtual(data_va));
  W32(hdr + 8, 0u);
  W32(hdr + kContainerFormat,
      0x0AA10621u);  // Swizzled A8R8G8B8, 2D, 1 level, 1024x1024
  W32(hdr + 16, 0u); // Size = 0 -> hardware swizzled layout (d.linear = false)
  g_vfx_atlas_hdr = hdr;
  return g_vfx_atlas_hdr;
}

} // namespace

void DrawTexturedWorldOverlay(const TexturedWorldVertex *verts,
                              uint32_t vertex_count, const float view[16],
                              const float proj[16], bool additive) {
  if (!g_backend || !verts || vertex_count < 3 || !view || !proj)
    return;
  uint32_t tri_verts = vertex_count - (vertex_count % 3u);
  if (tri_verts < 3)
    return;

  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (!g_state.rt.header || !g_state.zs.header)
    return;
  const uint32_t atlas_hdr = EnsureVfxAtlasTexture();
  if (!atlas_hdr)
    return;

  SyncDeferred();

  const auto saved_stream = g_state.streams[0];
  const auto saved_textures = g_state.textures;
  const uint32_t saved_vs_handle = g_state.vs_handle;
  const auto saved_vs = g_state.vs;
  const auto saved_ps = g_state.ps;
  const uint32_t saved_base_vertex = g_state.base_vertex;
  const uint32_t saved_scissor_count = g_state.scissor_count;
  const auto saved_view = g_state.transforms[2];
  const auto saved_proj = g_state.transforms[3];
  const auto saved_world = g_state.transforms[256];

  const uint32_t saved_zen = g_state.rs[gpu::kRsZEnable];
  const uint32_t saved_cull = g_state.rs[gpu::kRsCullMode];
  const uint32_t saved_sten = g_state.rs[gpu::kRsStencilEnable];
  const uint32_t saved_fill = g_state.rs[gpu::kRsFillMode];
  const uint32_t saved_light = g_state.rs[gpu::kRsLighting];
  const uint32_t saved_fog = g_state.rs[gpu::kRsFogEnable];
  const uint32_t saved_two = g_state.rs[gpu::kRsTwoSidedLighting];

  const uint32_t saved_zwrite = g_state.simple[gpu::kMZWriteEnable >> 2];
  const uint32_t saved_zfunc = g_state.simple[gpu::kMZFunc >> 2];
  const uint32_t saved_blend = g_state.simple[gpu::kMBlendEnable >> 2];
  const uint32_t saved_src = g_state.simple[gpu::kMSrcBlend >> 2];
  const uint32_t saved_dst = g_state.simple[gpu::kMDestBlend >> 2];
  const uint32_t saved_op = g_state.simple[gpu::kMBlendOp >> 2];
  const uint32_t saved_atest = g_state.simple[gpu::kMAlphaTestEnable >> 2];
  const uint32_t saved_afunc = g_state.simple[gpu::kMAlphaFunc >> 2];
  const uint32_t saved_cwrite = g_state.simple[gpu::kMColorWriteEnable >> 2];
  const uint32_t saved_shade = g_state.simple[gpu::kMShadeMode >> 2];
  const uint32_t saved_solid_off =
      g_state.simple[gpu::kMSolidOffsetEnable >> 2];

  const ScopedOverlayExtraState extra_state_guard;
  const auto saved_tss0 = g_state.tss[0];
  const uint32_t saved_tss1_cop = g_state.tss[1][gpu::kTssColorOp];
  const uint32_t saved_tss1_aop = g_state.tss[1][gpu::kTssAlphaOp];

  // Configure textured fixed-function FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE |
  // D3DFVF_TEX1 = 0x142)
  std::memcpy(g_state.transforms[2].data(), view, 64);
  std::memcpy(g_state.transforms[3].data(), proj, 64);
  g_state.transforms[256] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  g_state.textures.fill(0);
  g_state.textures[0] = atlas_hdr;
  g_state.vs_handle = 0x142u;
  g_state.vs = nullptr;
  g_state.ps = nullptr;
  g_state.base_vertex = 0;
  g_state.scissor_count = 0;

  g_state.rs[gpu::kRsZEnable] = 1;
  g_state.rs[gpu::kRsCullMode] = 0;
  g_state.rs[gpu::kRsStencilEnable] = 0;
  g_state.rs[gpu::kRsFillMode] = 0x1B02u;
  g_state.rs[gpu::kRsLighting] = 0;
  g_state.rs[gpu::kRsFogEnable] = 0;
  g_state.rs[gpu::kRsTwoSidedLighting] = 0;

  g_state.simple[gpu::kMZWriteEnable >> 2] = 0;
  g_state.simple[gpu::kMZFunc >> 2] = 0x203u; // D3DCMP_LESSEQUAL
  g_state.simple[gpu::kMBlendEnable >> 2] = 1;
  g_state.simple[gpu::kMSrcBlend >> 2] = 0x302u; // D3DBLEND_SRCALPHA
  g_state.simple[gpu::kMDestBlend >> 2] = additive ? 1u : 0x303u;
  g_state.simple[gpu::kMBlendOp >> 2] = 0x8006u;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = 0;
  g_state.simple[gpu::kMAlphaFunc >> 2] = 0x207u;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = 0x01010101u;
  g_state.simple[gpu::kMShadeMode >> 2] = 0x1D01u;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = 0;

  g_state.tss[0][gpu::kTssAddressU] = 3u; // D3DTADDRESS_CLAMP
  g_state.tss[0][gpu::kTssAddressV] = 3u;
  g_state.tss[0][gpu::kTssMagFilter] = 2u; // D3DTEXF_LINEAR
  g_state.tss[0][gpu::kTssMinFilter] = 2u;
  g_state.tss[0][gpu::kTssTexCoordIndex] = 0u;
  g_state.tss[0][gpu::kTssAlphaKill] = 0u;
  g_state.tss[0][gpu::kTssColorOp] = 4u;   // D3DTOP_MODULATE
  g_state.tss[0][gpu::kTssColorArg1] = 2u; // D3DTA_TEXTURE
  g_state.tss[0][gpu::kTssColorArg2] = 0u; // D3DTA_DIFFUSE
  g_state.tss[0][gpu::kTssAlphaOp] = 4u;   // D3DTOP_MODULATE
  g_state.tss[0][gpu::kTssAlphaArg1] = 2u; // D3DTA_TEXTURE
  g_state.tss[0][gpu::kTssAlphaArg2] = 0u; // D3DTA_DIFFUSE
  g_state.tss[1][gpu::kTssColorOp] = 1u;   // D3DTOP_DISABLE
  g_state.tss[1][gpu::kTssAlphaOp] = 1u;

  constexpr uint32_t kChunkVerts = 2400u; // Multiple of 3 (~57 KB < kUpHalf)
  for (uint32_t off = 0; off < tri_verts; off += kChunkVerts) {
    uint32_t chunk = std::min(kChunkVerts, tri_verts - off);
    uint32_t hdr =
        UpVertices(verts + off, chunk * uint32_t(sizeof(TexturedWorldVertex)));
    g_state.streams[0] = {hdr, uint32_t(sizeof(TexturedWorldVertex))};
    g_backend->Draw(g_state, 5 /* D3DPT_TRIANGLELIST */, 0, chunk, nullptr);
  }

  // Restore exact original D3D8 state
  g_state.streams[0] = saved_stream;
  g_state.textures = saved_textures;
  g_state.vs_handle = saved_vs_handle;
  g_state.vs = saved_vs;
  g_state.ps = saved_ps;
  g_state.base_vertex = saved_base_vertex;
  g_state.scissor_count = saved_scissor_count;
  g_state.transforms[2] = saved_view;
  g_state.transforms[3] = saved_proj;
  g_state.transforms[256] = saved_world;

  g_state.rs[gpu::kRsZEnable] = saved_zen;
  g_state.rs[gpu::kRsCullMode] = saved_cull;
  g_state.rs[gpu::kRsStencilEnable] = saved_sten;
  g_state.rs[gpu::kRsFillMode] = saved_fill;
  g_state.rs[gpu::kRsLighting] = saved_light;
  g_state.rs[gpu::kRsFogEnable] = saved_fog;
  g_state.rs[gpu::kRsTwoSidedLighting] = saved_two;

  g_state.simple[gpu::kMZWriteEnable >> 2] = saved_zwrite;
  g_state.simple[gpu::kMZFunc >> 2] = saved_zfunc;
  g_state.simple[gpu::kMBlendEnable >> 2] = saved_blend;
  g_state.simple[gpu::kMSrcBlend >> 2] = saved_src;
  g_state.simple[gpu::kMDestBlend >> 2] = saved_dst;
  g_state.simple[gpu::kMBlendOp >> 2] = saved_op;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = saved_atest;
  g_state.simple[gpu::kMAlphaFunc >> 2] = saved_afunc;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = saved_cwrite;
  g_state.simple[gpu::kMShadeMode >> 2] = saved_shade;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = saved_solid_off;

  g_state.tss[0] = saved_tss0;
  g_state.tss[1][gpu::kTssColorOp] = saved_tss1_cop;
  g_state.tss[1][gpu::kTssAlphaOp] = saved_tss1_aop;
}

namespace {

// Screen-space refraction used by the explosion shockwave: the colour target
// is copied into a scratch surface and then re-drawn back through a warped
// grid, so the world behind a fresh blast visibly bends for a moment.
uint32_t g_warp_surface = 0;
uint32_t g_warp_w = 0;
uint32_t g_warp_h = 0;
int g_warp_state = 0; // 0 = untested, 1 = usable, -1 = unsupported here
int g_warp_tries = 0;
uint64_t g_warp_frame = ~0ull;

bool WarpSnapshotHasPixels() {
  if (!g_warp_surface)
    return false;
  const uint32_t data =
      mem::VirtualFromPhysical(R32(g_warp_surface + kResData));
  if (!data)
    return false;
  const uint32_t pitch = (g_warp_w * 4u + 63u) & ~63u;
  if (!pitch)
    return false;
  const uint32_t *px = static_cast<const uint32_t *>(HostPtr(data));
  if (!px)
    return false;
  const uint32_t row = pitch >> 2;
  for (uint32_t gy = 1; gy <= 7; gy++) {
    const uint32_t y = (g_warp_h * gy) / 8u;
    for (uint32_t gx = 1; gx <= 7; gx++) {
      const uint32_t x = (g_warp_w * gx) / 8u;
      if ((px[y * row + x] & 0x00FFFFFFu) != 0)
        return true;
    }
  }
  return false;
}

bool CaptureFrameForWarp() {
  if (!g_backend || g_warp_state < 0 || !g_state.rt.header)
    return false;

  uint32_t w = 0, h = 0;
  gpu::SurfaceSize(g_state.rt, w, h);
  if (w < 8 || h < 8)
    return false;
  const uint32_t fmt =
      (R32(g_state.rt.header + kContainerFormat) >> 8) & 0xFFu;

  if (!g_warp_surface || g_warp_w != w || g_warp_h != h) {
    g_warp_surface = NewSurface(w, h, fmt, 4);
    g_warp_w = w;
    g_warp_h = h;
    g_warp_state = 0;
    g_warp_tries = 0;
    g_warp_frame = ~0ull;
    if (!g_warp_surface)
      return false;
  }

  if (g_warp_frame == g_backend_presents && g_warp_state > 0)
    return true;

  g_backend->Copy(g_state.rt, SurfaceOf(g_warp_surface), 0, 0, 0);
  g_warp_frame = g_backend_presents;

  if (g_warp_state == 0) {
    const uint32_t data =
        mem::VirtualFromPhysical(R32(g_warp_surface + kResData));
    if (data)
      g_backend->Sync(data, 0);
    if (WarpSnapshotHasPixels()) {
      g_warp_state = 1;
    } else if (++g_warp_tries >= 4) {
      g_warp_state = -1;
      LOG_FIRST(1, "d3d: screen-warp lens disabled, the backend cannot resolve "
                   "the colour target");
      return false;
    }
  }
  return g_warp_state > 0;
}

} // namespace

bool DrawScreenWarp(const TexturedWorldVertex *verts, uint32_t vertex_count) {
  if (!g_backend || !verts || vertex_count < 3)
    return false;
  const uint32_t tri_verts = vertex_count - (vertex_count % 3u);
  if (tri_verts < 3)
    return false;

  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (!g_state.rt.header || !g_state.zs.header)
    return false;

  SyncDeferred();
  if (!CaptureFrameForWarp())
    return false;

  const auto saved_stream = g_state.streams[0];
  const auto saved_textures = g_state.textures;
  const uint32_t saved_vs_handle = g_state.vs_handle;
  const auto saved_vs = g_state.vs;
  const auto saved_ps = g_state.ps;
  const uint32_t saved_base_vertex = g_state.base_vertex;
  const uint32_t saved_scissor_count = g_state.scissor_count;
  const auto saved_view = g_state.transforms[2];
  const auto saved_proj = g_state.transforms[3];
  const auto saved_world = g_state.transforms[256];

  const uint32_t saved_zen = g_state.rs[gpu::kRsZEnable];
  const uint32_t saved_cull = g_state.rs[gpu::kRsCullMode];
  const uint32_t saved_sten = g_state.rs[gpu::kRsStencilEnable];
  const uint32_t saved_fill = g_state.rs[gpu::kRsFillMode];
  const uint32_t saved_light = g_state.rs[gpu::kRsLighting];
  const uint32_t saved_fog = g_state.rs[gpu::kRsFogEnable];
  const uint32_t saved_two = g_state.rs[gpu::kRsTwoSidedLighting];

  const uint32_t saved_zwrite = g_state.simple[gpu::kMZWriteEnable >> 2];
  const uint32_t saved_zfunc = g_state.simple[gpu::kMZFunc >> 2];
  const uint32_t saved_blend = g_state.simple[gpu::kMBlendEnable >> 2];
  const uint32_t saved_src = g_state.simple[gpu::kMSrcBlend >> 2];
  const uint32_t saved_dst = g_state.simple[gpu::kMDestBlend >> 2];
  const uint32_t saved_op = g_state.simple[gpu::kMBlendOp >> 2];
  const uint32_t saved_atest = g_state.simple[gpu::kMAlphaTestEnable >> 2];
  const uint32_t saved_afunc = g_state.simple[gpu::kMAlphaFunc >> 2];
  const uint32_t saved_cwrite = g_state.simple[gpu::kMColorWriteEnable >> 2];
  const uint32_t saved_shade = g_state.simple[gpu::kMShadeMode >> 2];
  const uint32_t saved_solid_off =
      g_state.simple[gpu::kMSolidOffsetEnable >> 2];

  const ScopedOverlayExtraState extra_state_guard;
  const auto saved_tss0 = g_state.tss[0];
  const uint32_t saved_tss1_cop = g_state.tss[1][gpu::kTssColorOp];
  const uint32_t saved_tss1_aop = g_state.tss[1][gpu::kTssAlphaOp];

  // Clip-space passthrough: the caller already supplies NDC x/y in [-1, 1].
  g_state.transforms[2] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  g_state.transforms[3] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  g_state.transforms[256] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  g_state.textures.fill(0);
  g_state.textures[0] = g_warp_surface;
  g_state.vs_handle = 0x142u;
  g_state.vs = nullptr;
  g_state.ps = nullptr;
  g_state.base_vertex = 0;
  g_state.scissor_count = 0;

  g_state.rs[gpu::kRsZEnable] = 0;
  g_state.rs[gpu::kRsCullMode] = 0;
  g_state.rs[gpu::kRsStencilEnable] = 0;
  g_state.rs[gpu::kRsFillMode] = 0x1B02u;
  g_state.rs[gpu::kRsLighting] = 0;
  g_state.rs[gpu::kRsFogEnable] = 0;
  g_state.rs[gpu::kRsTwoSidedLighting] = 0;

  g_state.simple[gpu::kMZWriteEnable >> 2] = 0;
  g_state.simple[gpu::kMZFunc >> 2] = 0x203u;
  g_state.simple[gpu::kMBlendEnable >> 2] = 1;
  g_state.simple[gpu::kMSrcBlend >> 2] = 0x302u; // D3DBLEND_SRCALPHA
  g_state.simple[gpu::kMDestBlend >> 2] = 0x303u;
  g_state.simple[gpu::kMBlendOp >> 2] = 0x8006u;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = 0;
  g_state.simple[gpu::kMAlphaFunc >> 2] = 0x207u;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = 0x01010101u;
  g_state.simple[gpu::kMShadeMode >> 2] = 0x1D01u;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = 0;

  g_state.tss[0][gpu::kTssAddressU] = 3u; // D3DTADDRESS_CLAMP
  g_state.tss[0][gpu::kTssAddressV] = 3u;
  g_state.tss[0][gpu::kTssMagFilter] = 2u; // D3DTEXF_LINEAR
  g_state.tss[0][gpu::kTssMinFilter] = 2u;
  g_state.tss[0][gpu::kTssTexCoordIndex] = 0u;
  g_state.tss[0][gpu::kTssAlphaKill] = 0u;
  g_state.tss[0][gpu::kTssColorOp] = 4u;   // D3DTOP_MODULATE
  g_state.tss[0][gpu::kTssColorArg1] = 2u; // D3DTA_TEXTURE
  g_state.tss[0][gpu::kTssColorArg2] = 0u; // D3DTA_DIFFUSE
  // The captured frame's alpha channel is meaningless for the back buffer, so
  // take the lens alpha straight from the vertex colour.
  g_state.tss[0][gpu::kTssAlphaOp] = 2u;   // D3DTOP_SELECTARG1
  g_state.tss[0][gpu::kTssAlphaArg1] = 0u; // D3DTA_DIFFUSE
  g_state.tss[0][gpu::kTssAlphaArg2] = 0u;
  g_state.tss[1][gpu::kTssColorOp] = 1u; // D3DTOP_DISABLE
  g_state.tss[1][gpu::kTssAlphaOp] = 1u;

  constexpr uint32_t kChunkVerts = 2400u;
  for (uint32_t off = 0; off < tri_verts; off += kChunkVerts) {
    const uint32_t chunk = std::min(kChunkVerts, tri_verts - off);
    const uint32_t hdr =
        UpVertices(verts + off, chunk * uint32_t(sizeof(TexturedWorldVertex)));
    g_state.streams[0] = {hdr, uint32_t(sizeof(TexturedWorldVertex))};
    g_backend->Draw(g_state, 5 /* D3DPT_TRIANGLELIST */, 0, chunk, nullptr);
  }

  g_state.streams[0] = saved_stream;
  g_state.textures = saved_textures;
  g_state.vs_handle = saved_vs_handle;
  g_state.vs = saved_vs;
  g_state.ps = saved_ps;
  g_state.base_vertex = saved_base_vertex;
  g_state.scissor_count = saved_scissor_count;
  g_state.transforms[2] = saved_view;
  g_state.transforms[3] = saved_proj;
  g_state.transforms[256] = saved_world;

  g_state.rs[gpu::kRsZEnable] = saved_zen;
  g_state.rs[gpu::kRsCullMode] = saved_cull;
  g_state.rs[gpu::kRsStencilEnable] = saved_sten;
  g_state.rs[gpu::kRsFillMode] = saved_fill;
  g_state.rs[gpu::kRsLighting] = saved_light;
  g_state.rs[gpu::kRsFogEnable] = saved_fog;
  g_state.rs[gpu::kRsTwoSidedLighting] = saved_two;

  g_state.simple[gpu::kMZWriteEnable >> 2] = saved_zwrite;
  g_state.simple[gpu::kMZFunc >> 2] = saved_zfunc;
  g_state.simple[gpu::kMBlendEnable >> 2] = saved_blend;
  g_state.simple[gpu::kMSrcBlend >> 2] = saved_src;
  g_state.simple[gpu::kMDestBlend >> 2] = saved_dst;
  g_state.simple[gpu::kMBlendOp >> 2] = saved_op;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = saved_atest;
  g_state.simple[gpu::kMAlphaFunc >> 2] = saved_afunc;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = saved_cwrite;
  g_state.simple[gpu::kMShadeMode >> 2] = saved_shade;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = saved_solid_off;

  g_state.tss[0] = saved_tss0;
  g_state.tss[1][gpu::kTssColorOp] = saved_tss1_cop;
  g_state.tss[1][gpu::kTssAlphaOp] = saved_tss1_aop;
  return true;
}

void DrawViewmodelMesh(const ViewmodelVertex *verts, uint32_t vertex_count,
                       const uint16_t *indices, uint32_t index_count,
                       const float world[16], const float view[16],
                       const float proj[16], const float flame_pos_world[3],
                       float flame_intensity) {
  if (!g_backend || !verts || !indices || vertex_count == 0 ||
      index_count < 3 || !world || !view || !proj)
    return;

  std::lock_guard<std::recursive_mutex> lock(g_lock);

  static uint32_t s_vm_vb_hdr = 0;
  static uint32_t s_vm_vb_data = 0;
  static const ViewmodelVertex *s_vm_uploaded_ptr = nullptr;
  static uint32_t s_vm_uploaded_count = 0;
  const uint32_t vbytes = vertex_count * uint32_t(sizeof(ViewmodelVertex));

  if (!s_vm_vb_hdr) {
    s_vm_vb_hdr = mem::AllocSystem(kHeaderBytes);
  }
  const bool new_buffer = !s_vm_vb_data || s_vm_uploaded_count < vertex_count;
  if (new_buffer) {
    s_vm_vb_data = mem::AllocContiguous(vbytes, 0, 0xFFFFFFFFu, 0x1000u);
  }
  // Hands IK rebuilds this mesh every frame from a static buffer: re-upload and
  // dirty it each frame (the pointer alone does not change), like the textured path.
  std::memcpy(HostPtr(s_vm_vb_data), verts, vbytes);
  gpu::MarkDirty(s_vm_vb_data);
  if (new_buffer || s_vm_uploaded_ptr != verts || s_vm_uploaded_count != vertex_count) {
    std::memset(HostPtr(s_vm_vb_hdr), 0, kHeaderBytes);
    W32(s_vm_vb_hdr + kResCommon, 1u);
    W32(s_vm_vb_hdr + kResData, mem::PhysicalFromVirtual(s_vm_vb_data));
    s_vm_uploaded_ptr = verts;
    s_vm_uploaded_count = vertex_count;
  }

  // Save D3D8 pipeline state
  const auto saved_stream = g_state.streams[0];
  const auto saved_textures = g_state.textures;
  const uint32_t saved_vs_handle = g_state.vs_handle;
  const auto saved_vs = g_state.vs;
  const auto saved_ps = g_state.ps;
  const uint32_t saved_base_vertex = g_state.base_vertex;
  const uint32_t saved_scissor_count = g_state.scissor_count;
  const auto saved_viewport = g_state.viewport;
  const auto saved_view = g_state.transforms[2];
  const auto saved_proj = g_state.transforms[3];
  const auto saved_world = g_state.transforms[256];
  const auto saved_material = g_state.material;
  const auto saved_lights0 = g_state.lights[0];
  const auto saved_lights1 = g_state.lights[1];
  const auto saved_lights2 = g_state.lights[2];
  const uint32_t saved_light_enable = g_state.light_enable;

  const uint32_t saved_zen = g_state.rs[gpu::kRsZEnable];
  const uint32_t saved_cull = g_state.rs[gpu::kRsCullMode];
  const uint32_t saved_sten = g_state.rs[gpu::kRsStencilEnable];
  const uint32_t saved_fill = g_state.rs[gpu::kRsFillMode];
  const uint32_t saved_light = g_state.rs[gpu::kRsLighting];
  const uint32_t saved_spec = g_state.rs[gpu::kRsSpecularEnable];
  const uint32_t saved_locv = g_state.rs[gpu::kRsLocalViewer];
  const uint32_t saved_norm = g_state.rs[gpu::kRsNormalizeNormals];
  const uint32_t saved_colv = g_state.rs[gpu::kRsColorVertex];
  const uint32_t saved_dms = g_state.rs[gpu::kRsDiffuseMaterialSource];
  const uint32_t saved_ams = g_state.rs[gpu::kRsAmbientMaterialSource];
  const uint32_t saved_amb = g_state.rs[gpu::kRsAmbient];
  const uint32_t saved_fog = g_state.rs[gpu::kRsFogEnable];
  const uint32_t saved_two = g_state.rs[gpu::kRsTwoSidedLighting];

  const uint32_t saved_zwrite = g_state.simple[gpu::kMZWriteEnable >> 2];
  const uint32_t saved_zfunc = g_state.simple[gpu::kMZFunc >> 2];
  const uint32_t saved_blend = g_state.simple[gpu::kMBlendEnable >> 2];
  const uint32_t saved_atest = g_state.simple[gpu::kMAlphaTestEnable >> 2];
  const uint32_t saved_cwrite = g_state.simple[gpu::kMColorWriteEnable >> 2];
  const uint32_t saved_shade = g_state.simple[gpu::kMShadeMode >> 2];
  const uint32_t saved_solid_off =
      g_state.simple[gpu::kMSolidOffsetEnable >> 2];
  const uint32_t saved_tss0_cop = g_state.tss[0][gpu::kTssColorOp];
  const uint32_t saved_tss0_ca1 = g_state.tss[0][gpu::kTssColorArg1];
  const uint32_t saved_tss0_aop = g_state.tss[0][gpu::kTssAlphaOp];
  const uint32_t saved_tss0_aa1 = g_state.tss[0][gpu::kTssAlphaArg1];

  const ScopedOverlayExtraState extra_state_guard;
  struct ScopedVmPipelineFix {
    uint32_t spec_src, emis_src;
    std::array<uint32_t, 3> cop, aop;
    ScopedVmPipelineFix()
        : spec_src(g_state.rs[gpu::kRsSpecularMaterialSource]),
          emis_src(g_state.rs[gpu::kRsEmissiveMaterialSource]),
          cop{g_state.tss[1][gpu::kTssColorOp], g_state.tss[2][gpu::kTssColorOp],
              g_state.tss[3][gpu::kTssColorOp]},
          aop{g_state.tss[1][gpu::kTssAlphaOp], g_state.tss[2][gpu::kTssAlphaOp],
              g_state.tss[3][gpu::kTssAlphaOp]} {
      g_state.rs[gpu::kRsSpecularMaterialSource] = 0u; // D3DMCS_MATERIAL
      g_state.rs[gpu::kRsEmissiveMaterialSource] = 0u; // D3DMCS_MATERIAL
      for (int s = 1; s <= 3; ++s) {
        g_state.tss[s][gpu::kTssColorOp] = 1u; // D3DTOP_DISABLE
        g_state.tss[s][gpu::kTssAlphaOp] = 1u;
      }
    }
    ~ScopedVmPipelineFix() {
      g_state.rs[gpu::kRsSpecularMaterialSource] = spec_src;
      g_state.rs[gpu::kRsEmissiveMaterialSource] = emis_src;
      for (int i = 0; i < 3; ++i) {
        g_state.tss[1 + i][gpu::kTssColorOp] = cop[i];
        g_state.tss[1 + i][gpu::kTssAlphaOp] = aop[i];
      }
    }
  } vm_pipeline_fix;

  g_state.vs_handle = 0x52u; // D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE
  g_state.vs = nullptr;
  g_state.ps = nullptr;
  g_state.textures = {};
  g_state.base_vertex = 0;
  g_state.scissor_count = 0;
  g_state.viewport.min_z = 0.0f;
  g_state.viewport.max_z =
      0.08f; // foreground slice + triggers metallic Fresnel specular hook
  std::memcpy(g_state.transforms[2].data(), view, 64);
  std::memcpy(g_state.transforms[3].data(), proj, 64);
  std::memcpy(g_state.transforms[256].data(), world, 64);

  g_state.rs[gpu::kRsZEnable] = 1;
  g_state.rs[gpu::kRsCullMode] =
      0; // double-sided avoids thin shroud back-face holes
  g_state.rs[gpu::kRsStencilEnable] = 0;
  g_state.rs[gpu::kRsFillMode] = 0x1B02u;
  g_state.rs[gpu::kRsLighting] = 1;
  g_state.rs[gpu::kRsSpecularEnable] = 1;
  g_state.rs[gpu::kRsLocalViewer] = 1;
  g_state.rs[gpu::kRsNormalizeNormals] = 1;
  g_state.rs[gpu::kRsColorVertex] = 1;
  g_state.rs[gpu::kRsDiffuseMaterialSource] = 1; // D3DMCS_COLOR1
  g_state.rs[gpu::kRsAmbientMaterialSource] = 1; // D3DMCS_COLOR1
  g_state.rs[gpu::kRsAmbient] = 0xFF202226u;
  g_state.rs[gpu::kRsFogEnable] = 0;
  g_state.rs[gpu::kRsTwoSidedLighting] = 0;

  g_state.simple[gpu::kMZWriteEnable >> 2] = 1;
  g_state.simple[gpu::kMZFunc >> 2] = 0x203u; // D3DCMP_LESSEQUAL
  g_state.simple[gpu::kMBlendEnable >> 2] = 0;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = 0;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = 0x01010101u;
  g_state.simple[gpu::kMShadeMode >> 2] = 0x1D01u;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = 0;

  g_state.tss[0][gpu::kTssColorOp] = 2u;   // D3DTOP_SELECTARG1
  g_state.tss[0][gpu::kTssColorArg1] = 0u; // D3DTA_DIFFUSE
  g_state.tss[0][gpu::kTssAlphaOp] = 2u;
  g_state.tss[0][gpu::kTssAlphaArg1] = 0u;

  const float mat[17] = {
      1.00f, 1.00f,
      1.00f, 1.0f, // Diffuse (modulated with vertex color)
      0.88f, 0.88f,
      0.88f, 1.0f, // Ambient
      0.78f, 0.78f,
      0.82f, 1.0f, // Specular highlights on steel/copper/olive canisters
      0.04f, 0.035f,
      0.03f, 0.0f, // Subtle bounce lift so shadowed crevices stay crisp
      38.0f        // Specular power
  };
  std::memcpy(g_state.material.data(), mat, sizeof(mat));

  auto write_light =
      [](std::array<uint32_t, 26> &dst, uint32_t type, const float diff[4],
         const float spec[4], const float amb[4], const float pos[3],
         const float dir[3], float range, float att0, float att1, float att2) {
        float f[26] = {};
        std::memcpy(f + 1, diff, 16);
        std::memcpy(f + 5, spec, 16);
        std::memcpy(f + 9, amb, 16);
        if (pos)
          std::memcpy(f + 13, pos, 12);
        if (dir)
          std::memcpy(f + 16, dir, 12);
        f[19] = range;
        f[20] = 1.0f;
        f[21] = att0;
        f[22] = att1;
        f[23] = att2;
        std::memcpy(dst.data(), f, sizeof(f));
        dst[0] = type;
      };

  // Light 0: Overhead key light aligned gently with camera view vector
  const float k0_diff[4] = {0.95f, 0.94f, 0.92f, 1.0f};
  const float k0_spec[4] = {0.95f, 0.96f, 1.00f, 1.0f};
  const float k0_amb[4] = {0.28f, 0.28f, 0.30f, 1.0f};
  const float k0_dir[3] = {-0.32f, -0.82f, -0.47f};
  write_light(g_state.lights[0], 3u /* D3DLIGHT_DIRECTIONAL */, k0_diff,
              k0_spec, k0_amb, nullptr, k0_dir, 1000.0f, 1.0f, 0.0f, 0.0f);

  // Light 1: Warm lower rim fill light
  const float k1_diff[4] = {0.34f, 0.33f, 0.31f, 1.0f};
  const float k1_spec[4] = {0.22f, 0.22f, 0.24f, 1.0f};
  const float k1_amb[4] = {0.10f, 0.10f, 0.10f, 1.0f};
  const float k1_dir[3] = {0.42f, 0.72f, 0.55f};
  write_light(g_state.lights[1], 3u /* D3DLIGHT_DIRECTIONAL */, k1_diff,
              k1_spec, k1_amb, nullptr, k1_dir, 1000.0f, 1.0f, 0.0f, 0.0f);

  // Light 2: Dynamic nozzle pilot + firing flame point light in world space
  const float fi = std::clamp(flame_intensity, 0.0f, 2.5f);
  const float k2_diff[4] = {1.00f * fi, 0.72f * fi, 0.32f * fi, 1.0f};
  const float k2_spec[4] = {1.00f * fi, 0.84f * fi, 0.46f * fi, 1.0f};
  const float k2_amb[4] = {0.06f * fi, 0.02f * fi, 0.005f * fi, 1.0f};
  write_light(g_state.lights[2], 1u /* D3DLIGHT_POINT */, k2_diff, k2_spec,
              k2_amb, flame_pos_world, nullptr, 4.5f, 0.85f, 1.15f, 0.65f);
  g_state.light_enable = (fi > 0.01f) ? 0x7u : 0x3u;

  g_state.streams[0] = {s_vm_vb_hdr, uint32_t(sizeof(ViewmodelVertex))};
  g_backend->Draw(g_state, 5 /* D3DPT_TRIANGLELIST */, 0, index_count,
                  reinterpret_cast<const uint8_t *>(indices));

  // Restore original D3D8 state
  g_state.streams[0] = saved_stream;
  g_state.textures = saved_textures;
  g_state.vs_handle = saved_vs_handle;
  g_state.vs = saved_vs;
  g_state.ps = saved_ps;
  g_state.base_vertex = saved_base_vertex;
  g_state.scissor_count = saved_scissor_count;
  g_state.viewport = saved_viewport;
  g_state.transforms[2] = saved_view;
  g_state.transforms[3] = saved_proj;
  g_state.transforms[256] = saved_world;
  g_state.material = saved_material;
  g_state.lights[0] = saved_lights0;
  g_state.lights[1] = saved_lights1;
  g_state.lights[2] = saved_lights2;
  g_state.light_enable = saved_light_enable;

  g_state.rs[gpu::kRsZEnable] = saved_zen;
  g_state.rs[gpu::kRsCullMode] = saved_cull;
  g_state.rs[gpu::kRsStencilEnable] = saved_sten;
  g_state.rs[gpu::kRsFillMode] = saved_fill;
  g_state.rs[gpu::kRsLighting] = saved_light;
  g_state.rs[gpu::kRsSpecularEnable] = saved_spec;
  g_state.rs[gpu::kRsLocalViewer] = saved_locv;
  g_state.rs[gpu::kRsNormalizeNormals] = saved_norm;
  g_state.rs[gpu::kRsColorVertex] = saved_colv;
  g_state.rs[gpu::kRsDiffuseMaterialSource] = saved_dms;
  g_state.rs[gpu::kRsAmbientMaterialSource] = saved_ams;
  g_state.rs[gpu::kRsAmbient] = saved_amb;
  g_state.rs[gpu::kRsFogEnable] = saved_fog;
  g_state.rs[gpu::kRsTwoSidedLighting] = saved_two;

  g_state.simple[gpu::kMZWriteEnable >> 2] = saved_zwrite;
  g_state.simple[gpu::kMZFunc >> 2] = saved_zfunc;
  g_state.simple[gpu::kMBlendEnable >> 2] = saved_blend;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = saved_atest;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = saved_cwrite;
  g_state.simple[gpu::kMShadeMode >> 2] = saved_shade;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = saved_solid_off;
  g_state.tss[0][gpu::kTssColorOp] = saved_tss0_cop;
  g_state.tss[0][gpu::kTssColorArg1] = saved_tss0_ca1;
  g_state.tss[0][gpu::kTssAlphaOp] = saved_tss0_aop;
  g_state.tss[0][gpu::kTssAlphaArg1] = saved_tss0_aa1;
}

namespace {

uint32_t EnsureViewmodelBtexTexture(uint64_t cache_key, const uint32_t *swizzled_bgra,
                                    uint32_t width, uint32_t height) {
  if (!cache_key || !swizzled_bgra || width != 1024u || height != 1024u) {
    return 0u;
  }
  struct BtexSlot {
    uint64_t key = 0u;
    uint32_t hdr_va = 0u;
  };
  static std::array<BtexSlot, 8> s_cache{};
  for (const auto &entry : s_cache) {
    if (entry.key == cache_key && entry.hdr_va != 0u) return entry.hdr_va;
  }

  const uint32_t byte_size = width * height * 4u;
  const uint32_t data_va = mem::AllocSystem(byte_size, 256u);
  const uint32_t hdr_va = mem::AllocSystem(kHeaderBytes, 16u);
  if (!data_va || !hdr_va) return 0u;

  uint32_t *dst = reinterpret_cast<uint32_t *>(HostPtr(data_va));
  if (!dst) return 0u;
  std::memcpy(dst, swizzled_bgra, byte_size);

  std::memset(HostPtr(hdr_va), 0, kHeaderBytes);
  W32(hdr_va + kResCommon, 0x00040001u); // D3DTYPE_TEXTURE
  W32(hdr_va + kResData, mem::PhysicalFromVirtual(data_va));
  W32(hdr_va + kResLock, 0u);
  W32(hdr_va + kContainerFormat, 0x0AA10621u); // 1024x1024 swizzled A8R8G8B8, 1 level
  W32(hdr_va + 16, 0u);
  gpu::MarkDirty(data_va);

  static size_t s_next = 0u;
  s_cache[s_next % s_cache.size()] = {cache_key, hdr_va};
  s_next++;
  return hdr_va;
}

} // namespace

void DrawViewmodelTexturedBatches(const ViewmodelTexturedVertex *verts, uint32_t vertex_count,
                                  const ViewmodelTexturedBatch *batches, uint32_t batch_count,
                                  const float world[16], const float view[16], const float proj[16],
                                  const float flame_pos_world[3], float flame_intensity) {
  if (!g_backend || !verts || !batches || vertex_count == 0u || batch_count == 0u ||
      !world || !view || !proj) {
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(g_lock);

  // Fence-synchronized 8 MB ring buffer (matches UpVertices pattern) so multiple viewmodel
  // draws in the same frame never overwrite each other before the Render Thread consumes them.
  constexpr uint32_t kTexRingBytes = 8u << 20;
  constexpr uint32_t kTexRingHalf = kTexRingBytes / 2u;
  static uint32_t s_tex_ring = 0u;
  static uint32_t s_tex_hdr = 0u;
  static uint32_t s_tex_pos = 0u;
  static uint32_t s_tex_fence[2] = {};
  if (!s_tex_ring) {
    s_tex_ring = mem::AllocContiguous(kTexRingBytes, 0u, 0xFFFFFFFFu, 0x1000u);
    s_tex_hdr = mem::AllocSystem(kHeaderBytes, 16u);
    if (!s_tex_ring || !s_tex_hdr) return;
    std::memset(HostPtr(s_tex_hdr), 0, kHeaderBytes);
    W32(s_tex_hdr + kResCommon, 1u);
  }

  const uint32_t clamped_vc = std::min<uint32_t>(vertex_count, 65500u);
  const uint32_t upload_bytes = std::min<uint32_t>(
      clamped_vc * uint32_t(sizeof(ViewmodelTexturedVertex)), kTexRingHalf);
  const uint32_t room = (upload_bytes + 63u) & ~63u;
  const uint32_t pos = (s_tex_pos + room > kTexRingBytes) ? 0u : s_tex_pos;
  const uint32_t from = s_tex_pos ? (s_tex_pos - 1u) / kTexRingHalf : 1u;
  const uint32_t to = (pos + room - 1u) / kTexRingHalf;
  if (to != from) {
    s_tex_fence[from] = g_backend->Fence();
    g_backend->WaitFence(s_tex_fence[to]);
  }
  std::memcpy(HostPtr(s_tex_ring + pos), verts, upload_bytes);
  s_tex_pos = pos + room;
  W32(s_tex_hdr + kResData, mem::PhysicalFromVirtual(s_tex_ring + pos));
  gpu::MarkDirty(s_tex_ring + pos);

  std::vector<uint32_t> batch_tex(batch_count, 0u);
  for (uint32_t bi = 0; bi < batch_count; ++bi) {
    const auto &b = batches[bi];
    batch_tex[bi] = EnsureViewmodelBtexTexture(
        b.texture_cache_key, b.swizzled_bgra_pixels, b.tex_width, b.tex_height);
  }
  const uint32_t first_tex_hdr = batch_tex[0];
  const uint32_t tex_hdr = first_tex_hdr;

  const auto saved_stream = g_state.streams[0];
  const auto saved_viewport = g_state.viewport;
  const auto saved_view = g_state.transforms[2];
  const auto saved_proj = g_state.transforms[3];
  const auto saved_world = g_state.transforms[256];
  const auto saved_material = g_state.material;
  const auto saved_lights0 = g_state.lights[0];
  const auto saved_lights1 = g_state.lights[1];
  const auto saved_lights2 = g_state.lights[2];
  const uint32_t saved_light_enable = g_state.light_enable;
  const auto saved_textures = g_state.textures;
  const auto saved_palettes = g_state.palettes;
  const uint32_t saved_vs_handle = g_state.vs_handle;
  const auto saved_vs = g_state.vs;
  const auto saved_ps = g_state.ps;
  const uint32_t saved_base = g_state.base_vertex;
  const uint32_t saved_scissor = g_state.scissor_count;
  const auto saved_tss = g_state.tss;

  const uint32_t saved_ze = g_state.rs[gpu::kRsZEnable];
  const uint32_t saved_cull = g_state.rs[gpu::kRsCullMode];
  const uint32_t saved_fill = g_state.rs[gpu::kRsFillMode];
  const uint32_t saved_light = g_state.rs[gpu::kRsLighting];
  const uint32_t saved_spec = g_state.rs[gpu::kRsSpecularEnable];
  const uint32_t saved_loc = g_state.rs[gpu::kRsLocalViewer];
  const uint32_t saved_norm = g_state.rs[gpu::kRsNormalizeNormals];
  const uint32_t saved_cv = g_state.rs[gpu::kRsColorVertex];
  const uint32_t saved_diff_src = g_state.rs[gpu::kRsDiffuseMaterialSource];
  const uint32_t saved_amb_src = g_state.rs[gpu::kRsAmbientMaterialSource];
  const uint32_t saved_spec_src = g_state.rs[gpu::kRsSpecularMaterialSource];
  const uint32_t saved_emis_src = g_state.rs[gpu::kRsEmissiveMaterialSource];
  const uint32_t saved_amb = g_state.rs[gpu::kRsAmbient];
  const uint32_t saved_fog = g_state.rs[gpu::kRsFogEnable];
  const uint32_t saved_two = g_state.rs[gpu::kRsTwoSidedLighting];
  const uint32_t saved_sten = g_state.rs[gpu::kRsStencilEnable];
  const uint32_t saved_zw = g_state.simple[gpu::kMZWriteEnable >> 2];
  const uint32_t saved_zf = g_state.simple[gpu::kMZFunc >> 2];
  const uint32_t saved_at = g_state.simple[gpu::kMAlphaTestEnable >> 2];
  const uint32_t saved_be = g_state.simple[gpu::kMBlendEnable >> 2];
  const uint32_t saved_cw = g_state.simple[gpu::kMColorWriteEnable >> 2];
  const uint32_t saved_sm = g_state.simple[gpu::kMShadeMode >> 2];
  const uint32_t saved_so = g_state.simple[gpu::kMSolidOffsetEnable >> 2];

  const ScopedOverlayExtraState extra_state_guard;

  g_state.vs_handle = tex_hdr ? 0x152u : 0x52u;
  g_state.vs = nullptr;
  g_state.ps = nullptr;
  g_state.textures = {};
  g_state.textures[0] = tex_hdr;
  g_state.palettes = {};
  g_state.base_vertex = 0u;
  g_state.scissor_count = 0u;

  g_state.viewport.min_z = 0.0f;
  g_state.viewport.max_z = 0.08f;
  std::memcpy(g_state.transforms[2].data(), view, 64);
  std::memcpy(g_state.transforms[3].data(), proj, 64);
  std::memcpy(g_state.transforms[256].data(), world, 64);

  g_state.rs[gpu::kRsZEnable] = 1u;
  g_state.simple[gpu::kMZWriteEnable >> 2] = 1u;
  g_state.simple[gpu::kMZFunc >> 2] = 0x203u; // D3DCMP_LESSEQUAL
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = 0u;
  g_state.simple[gpu::kMBlendEnable >> 2] = 0u;
  g_state.rs[gpu::kRsCullMode] = 0u;          // double-sided for shroud holes & stamped metal
  g_state.rs[gpu::kRsFillMode] = 0x1B02u;     // D3DFILL_SOLID
  g_state.rs[gpu::kRsLighting] = 1u;
  g_state.rs[gpu::kRsSpecularEnable] = 1u;
  g_state.rs[gpu::kRsLocalViewer] = 1u;
  g_state.rs[gpu::kRsNormalizeNormals] = 1u;
  g_state.rs[gpu::kRsColorVertex] = 1u;
  g_state.rs[gpu::kRsDiffuseMaterialSource] = 1u;  // D3DMCS_COLOR1
  g_state.rs[gpu::kRsAmbientMaterialSource] = 1u;  // D3DMCS_COLOR1
  g_state.rs[gpu::kRsSpecularMaterialSource] = 0u; // D3DMCS_MATERIAL (prevent white blowout!)
  g_state.rs[gpu::kRsEmissiveMaterialSource] = 0u; // D3DMCS_MATERIAL
  g_state.rs[gpu::kRsAmbient] = 0xFF282A30u;
  g_state.rs[gpu::kRsFogEnable] = 0u;
  g_state.rs[gpu::kRsTwoSidedLighting] = 0u;
  g_state.rs[gpu::kRsStencilEnable] = 0u;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = 0x01010101u;
  g_state.simple[gpu::kMShadeMode >> 2] = 0x1D01u;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = 0u;

  if (tex_hdr) {
    g_state.tss[0][gpu::kTssAddressU] = 1u; // D3DTADDRESS_WRAP
    g_state.tss[0][gpu::kTssAddressV] = 1u;
    g_state.tss[0][gpu::kTssMagFilter] = 2u; // D3DTEXF_LINEAR
    g_state.tss[0][gpu::kTssMinFilter] = 2u;
    g_state.tss[0][gpu::kTssTexCoordIndex] = 0u;
    g_state.tss[0][gpu::kTssAlphaKill] = 0u;
    g_state.tss[0][gpu::kTssColorOp] = 4u;   // D3DTOP_MODULATE
    g_state.tss[0][gpu::kTssColorArg1] = 2u; // D3DTA_TEXTURE
    g_state.tss[0][gpu::kTssColorArg2] = 0u; // D3DTA_DIFFUSE
    g_state.tss[0][gpu::kTssAlphaOp] = 4u;
    g_state.tss[0][gpu::kTssAlphaArg1] = 2u;
    g_state.tss[0][gpu::kTssAlphaArg2] = 0u;
  } else {
    g_state.tss[0][gpu::kTssColorOp] = 2u;
    g_state.tss[0][gpu::kTssColorArg1] = 0u;
    g_state.tss[0][gpu::kTssAlphaOp] = 2u;
    g_state.tss[0][gpu::kTssAlphaArg1] = 0u;
  }
  for (int s = 1; s <= 3; ++s) {
    g_state.tss[s][gpu::kTssColorOp] = 1u; // D3DTOP_DISABLE
    g_state.tss[s][gpu::kTssAlphaOp] = 1u;
  }

  const float mat[17] = {
      0.92f, 0.92f, 0.92f, 1.0f, // Diffuse (tempered so steel/walnut grain stays crisp)
      0.44f, 0.44f, 0.48f, 1.0f, // Ambient
      0.32f, 0.33f, 0.36f, 1.0f, // Specular (machined gunmetal edge glint without blowout)
      0.0f, 0.0f, 0.0f, 0.0f,    // Emissive (strictly zero!)
      38.0f                      // Power (tight metallic highlight)
  };
  std::memcpy(g_state.material.data(), mat, sizeof(mat));

  auto write_light = [](std::array<uint32_t, 26> &dst, uint32_t type,
                        const float diff[4], const float spec[4], const float amb[4],
                        const float pos[3], const float dir[3], float range,
                        float att0, float att1, float att2) {
    auto fbits = [](float v) -> uint32_t {
      uint32_t u = 0u;
      std::memcpy(&u, &v, 4);
      return u;
    };
    dst.fill(0u);
    dst[0] = type;
    for (int k = 0; k < 4; ++k) {
      dst[1 + k] = fbits(diff[k]);
      dst[5 + k] = fbits(spec[k]);
      dst[9 + k] = fbits(amb[k]);
    }
    for (int k = 0; k < 3; ++k) {
      dst[13 + k] = fbits(pos[k]);
      dst[16 + k] = fbits(dir[k]);
    }
    dst[19] = fbits(range);
    dst[20] = fbits(1.0f);
    dst[21] = fbits(att0);
    dst[22] = fbits(att1);
    dst[23] = fbits(att2);
  };

  const float z3[3] = {0.0f, 0.0f, 0.0f};
  const float k0_dir[3] = {-0.55f, -0.52f, 0.65f};
  const float k0_diff[4] = {0.74f, 0.72f, 0.68f, 1.0f};
  const float k0_spec[4] = {0.36f, 0.36f, 0.38f, 1.0f};
  const float k0_amb[4] = {0.13f, 0.13f, 0.15f, 1.0f};
  write_light(g_state.lights[0], 3u, k0_diff, k0_spec, k0_amb, z3, k0_dir, 1000.0f, 1.0f, 0.0f, 0.0f);

  const float k1_dir[3] = {0.64f, 0.52f, -0.56f};
  const float k1_diff[4] = {0.28f, 0.32f, 0.42f, 1.0f};
  const float k1_spec[4] = {0.18f, 0.22f, 0.28f, 1.0f};
  const float k1_amb[4] = {0.05f, 0.05f, 0.06f, 1.0f};
  write_light(g_state.lights[1], 3u, k1_diff, k1_spec, k1_amb, z3, k1_dir, 1000.0f, 1.0f, 0.0f, 0.0f);

  const float fi = std::clamp(flame_intensity, 0.0f, 1.0f);
  const float fpos[3] = {flame_pos_world ? flame_pos_world[0] : 0.0f,
                         flame_pos_world ? flame_pos_world[1] : 0.0f,
                         flame_pos_world ? flame_pos_world[2] : 0.0f};
  const float k2_diff[4] = {0.58f * fi, 0.32f * fi, 0.08f * fi, 1.0f};
  const float k2_spec[4] = {0.28f * fi, 0.15f * fi, 0.04f * fi, 1.0f};
  const float k2_amb[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  write_light(g_state.lights[2], 1u, k2_diff, k2_spec, k2_amb, fpos, k0_dir, 3.8f, 0.7f, 1.2f, 0.9f);
  g_state.light_enable = (fi > 0.01f) ? 0x7u : 0x3u;

  g_state.streams[0] = {s_tex_hdr, uint32_t(sizeof(ViewmodelTexturedVertex))};
  for (uint32_t bi = 0; bi < batch_count; ++bi) {
    const auto &b = batches[bi];
    if (!b.indices || b.index_count < 3u) continue;
    g_state.textures[0] = batch_tex[bi];
    if (!batch_tex[bi]) {
      g_state.tss[0][gpu::kTssColorOp] = 2u;
      g_state.tss[0][gpu::kTssColorArg1] = 0u;
    } else {
      g_state.tss[0][gpu::kTssColorOp] = 4u;
      g_state.tss[0][gpu::kTssColorArg1] = 2u;
    }
    g_backend->Draw(g_state, 5 /* D3DPT_TRIANGLELIST */, 0, b.index_count,
                    reinterpret_cast<const uint8_t *>(b.indices));
  }

  g_state.streams[0] = saved_stream;
  g_state.textures = saved_textures;
  g_state.palettes = saved_palettes;
  g_state.vs_handle = saved_vs_handle;
  g_state.vs = saved_vs;
  g_state.ps = saved_ps;
  g_state.base_vertex = saved_base;
  g_state.scissor_count = saved_scissor;
  g_state.viewport = saved_viewport;
  g_state.transforms[2] = saved_view;
  g_state.transforms[3] = saved_proj;
  g_state.transforms[256] = saved_world;
  g_state.material = saved_material;
  g_state.lights[0] = saved_lights0;
  g_state.lights[1] = saved_lights1;
  g_state.lights[2] = saved_lights2;
  g_state.light_enable = saved_light_enable;
  g_state.tss = saved_tss;
  g_state.rs[gpu::kRsZEnable] = saved_ze;
  g_state.rs[gpu::kRsCullMode] = saved_cull;
  g_state.rs[gpu::kRsFillMode] = saved_fill;
  g_state.rs[gpu::kRsLighting] = saved_light;
  g_state.rs[gpu::kRsSpecularEnable] = saved_spec;
  g_state.rs[gpu::kRsLocalViewer] = saved_loc;
  g_state.rs[gpu::kRsNormalizeNormals] = saved_norm;
  g_state.rs[gpu::kRsColorVertex] = saved_cv;
  g_state.rs[gpu::kRsDiffuseMaterialSource] = saved_diff_src;
  g_state.rs[gpu::kRsAmbientMaterialSource] = saved_amb_src;
  g_state.rs[gpu::kRsSpecularMaterialSource] = saved_spec_src;
  g_state.rs[gpu::kRsEmissiveMaterialSource] = saved_emis_src;
  g_state.rs[gpu::kRsAmbient] = saved_amb;
  g_state.rs[gpu::kRsFogEnable] = saved_fog;
  g_state.rs[gpu::kRsTwoSidedLighting] = saved_two;
  g_state.rs[gpu::kRsStencilEnable] = saved_sten;
  g_state.simple[gpu::kMZWriteEnable >> 2] = saved_zw;
  g_state.simple[gpu::kMZFunc >> 2] = saved_zf;
  g_state.simple[gpu::kMAlphaTestEnable >> 2] = saved_at;
  g_state.simple[gpu::kMBlendEnable >> 2] = saved_be;
  g_state.simple[gpu::kMColorWriteEnable >> 2] = saved_cw;
  g_state.simple[gpu::kMShadeMode >> 2] = saved_sm;
  g_state.simple[gpu::kMSolidOffsetEnable >> 2] = saved_so;
}

void DrawViewmodelTexturedMesh(const ViewmodelTexturedVertex *verts, uint32_t vertex_count,
                               const uint16_t *indices, uint32_t index_count,
                               uint64_t texture_cache_key, const uint32_t *swizzled_bgra_pixels,
                               uint32_t tex_width, uint32_t tex_height,
                               const float world[16], const float view[16], const float proj[16],
                               const float flame_pos_world[3], float flame_intensity) {
  const ViewmodelTexturedBatch batch{
      indices, index_count, texture_cache_key, swizzled_bgra_pixels, tex_width, tex_height};
  DrawViewmodelTexturedBatches(verts, vertex_count, &batch, 1u, world, view, proj,
                               flame_pos_world, flame_intensity);
}

} // namespace d3d
