// Native renderer for Rayman Origins: draws the game's D3D draw calls with
// Vulkan and the game's own shaders (XenosRecomp SPIR-V), without emulating
// the Xenos GPU. Used offline (tools/native_renderer/vkrender, on a frame dump)
// and in-game (rex/src/native_renderer.cpp).
//
// Input per draw: the D3D device state block (device + 0x480 .. + 0x3700, big
// endian, see docs/D3D_MAP.md), the draw arguments, and guest physical memory.
// Pipeline interface: XenosRecomp HLSL rewritten by hlsl_ubo.py (no 64-bit
// addresses, so it runs on stock Adreno drivers without shaderInt64), then
// packed by spirv_patch.h into 4 sets with fixed-size heaps (plain Vulkan 1.1,
// no descriptor indexing):
//   set 0 Texture2D heap, set 1 Texture3D (binding 0) and TextureCube (binding 1),
//   set 2 samplers, set 3 uniform buffers: 0 VS constants, 1 PS constants, 2 shared constants.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "spirv_inputs.h"
#include "spirv_patch.h"
#include "xenos_texture.h"

namespace native {

constexpr uint32_t kStateBegin = 0x480, kStateEnd = 0x3700;  // device range the renderer reads

struct DrawCall {
  const uint8_t* state;  // device + kStateBegin .. kStateEnd, big-endian
  uint32_t entry;        // 0 DrawIndexed, 1 Draw, 2 DrawUP
  uint32_t primitive, baseVertex, startIndex, indexCount;
  uint64_t vs, ps;       // XXH3 hashes of the shader containers
  uint32_t ibWord0, ibAddress;
  const uint8_t* upData = nullptr;  // DrawVerticesUP: the vertices (host pointer, big-endian)
};

class Renderer {
 public:
  struct Layout {
    uint32_t stride;
    std::vector<VkVertexInputAttributeDescription> attributes;  // binding 0
    std::vector<uint32_t> byteOrderedOffsets;  // 4-byte attributes kept in memory byte order
  };
  struct Recorded {
    VkPipeline pipeline;
    uint32_t vsConst, psConst, shared;  // offsets into the constant buffer
    size_t vertexOffset, indexOffset;
    uint32_t indexCount;
    VkIndexType indexType;
    uint32_t firstVertex, vertexCount;  // non-indexed draws
    uint64_t target;                    // render target (TargetOf)
    float viewport[4];                  // x, y, width, height in the target's pixels
  };
  using ShaderSource = std::function<std::vector<uint32_t>(uint64_t hash, bool vertex)>;
  using SurfaceFactory = std::function<VkSurfaceKHR(VkInstance)>;

  // loader: vkGetInstanceProcAddr to use (null: volk finds the system loader).
  // surface: creates the window surface (null: offscreen, EndFrame reads back).
  // extraInstanceExtensions: what the window system needs (e.g. from SDL).
  // Optional progress log (initialization stages), e.g. for device bring-up.
  std::function<void(const char*)> log;

  bool Init(PFN_vkGetInstanceProcAddr loader, const std::vector<const char*>& extraInstanceExtensions,
            SurfaceFactory surface, uint32_t width, uint32_t height) {
    auto stage = [this](const char* s) { if (log) log(s); };
    width_ = width;
    height_ = height;
    stage("loader");
    if (loader) volkInitializeCustom(loader);
    else if (volkInitialize() != VK_SUCCESS) return Fail("no Vulkan loader");
    stage("instance");
    if (!CreateInstance(extraInstanceExtensions)) return false;
    if (surface) {
      stage("surface");
      surface_ = surface(instance_);
      if (!surface_) return Fail("surface creation failed");
    }
    stage("device");
    if (!CreateDevice()) return false;
    stage(surface_ ? "swapchain" : "offscreen target");
    if (surface_ && !CreateSwapchain()) return false;
    if (!surface_ && !CreateOffscreen()) return false;
    stage("descriptors");
    {
      VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
      pc.initialDataSize = pipelineCacheSeed_.size();
      pc.pInitialData = pipelineCacheSeed_.empty() ? nullptr : pipelineCacheSeed_.data();
      // A cache from another driver or GPU is rejected by the driver: start empty then.
      if (vkCreatePipelineCache(device_, &pc, nullptr, &pipelineCache_) != VK_SUCCESS) {
        pc.initialDataSize = 0, pc.pInitialData = nullptr;
        vkCreatePipelineCache(device_, &pc, nullptr, &pipelineCache_);
      }
      pipelineCacheSeed_.clear();
    }
    if (!CreateRenderPass()) return Fail("render pass failed");
    if (!CreateDescriptors()) return false;
    // Per-frame resources: kFrames frames in flight, so the CPU records the
    // next frame while the GPU draws this one. Sizes are per frame; the
    // constants and vertices copied per draw are only what the shaders and
    // primitives use.
    for (uint32_t i = 0; i < kFrames; ++i) {
      Frame& f = frames_[i];
      f.constants = CreateBuffer(16u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      f.vertices = CreateBuffer(24u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      f.indices = CreateBuffer(8u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      f.staging = CreateBuffer(48u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
      // The three constant blocks, bound with per-draw dynamic offsets.
      VkDescriptorBufferInfo infos[3] = {{f.constants.buffer, 0, 4096}, {f.constants.buffer, 0, 4096},
                                         {f.constants.buffer, 0, 512}};
      VkWriteDescriptorSet w[3]{};
      for (uint32_t b = 0; b < 3; ++b) {
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = f.constantSet;
        w[b].dstBinding = b;
        w[b].descriptorCount = 1;
        w[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[b].pBufferInfo = &infos[b];
      }
      vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vkCreateFence(device_, &fi, nullptr, &f.fence);
      VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      vkCreateSemaphore(device_, &si, nullptr, &f.acquired);
      vkCreateSemaphore(device_, &si, nullptr, &f.rendered);
    }
    LoadFrame(0);
    ready_ = true;
    return true;
  }

  void SetShaderSource(ShaderSource source) { shaders_ = std::move(source); }

  // ---- Pipeline persistence (no compile hitches after the first session) ----
  // A pipeline the game used: shaders, blend state, vertex stride.
  struct PipelineDesc {
    uint64_t vs, ps;
    uint32_t blend, stride;
  };
  // Driver cache contents from a previous run (call before Init).
  void SetPipelineCacheData(std::vector<uint8_t> data) { pipelineCacheSeed_ = std::move(data); }
  std::vector<uint8_t> PipelineCacheData() {
    size_t size = 0;
    if (!pipelineCache_ || vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS) return {};
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) return {};
    data.resize(size);
    return data;
  }
  // Every pipeline created so far, and creating a known list up front.
  std::vector<PipelineDesc> PipelineDescs() const {
    std::vector<PipelineDesc> out;
    for (auto& [key, pipe] : pipelines_)
      if (pipe) out.push_back({key.vs, key.ps, key.blend, key.stride});
    return out;
  }
  uint32_t Prewarm(const std::vector<PipelineDesc>& descs) {
    uint32_t made = 0;
    for (const PipelineDesc& p : descs) {
      VkShaderModule vsm = Module(p.vs, true), psm = Module(p.ps, false);
      std::string missing;
      const Layout* layout = vsm && psm ? LayoutFor(p.vs, p.stride, &missing) : nullptr;
      if (layout && Pipeline(p.vs, p.ps, vsm, psm, *layout, p.blend)) ++made;
    }
    return made;
  }
  void SetMemory(xenos::MemoryReader memory) { memory_ = std::move(memory); }
  bool ready() const { return ready_; }
  const std::string& error() const { return error_; }

  struct Stats {
    uint32_t draws = 0, skipped = 0, pipelines = 0, textures = 0;  // pipelines/textures: totals
    // This frame: what got created or re-decoded (hitch attribution), and
    // where EndFrame spent its time.
    uint32_t newPipelines = 0, newTextures = 0, reuploads = 0;
    size_t uploadBytes = 0;
    double recordMs = 0, waitMs = 0;
  };
  // Why draws were skipped since the last call, e.g. "layout vs=...(0,4,8,9,12)" -> count.
  std::map<std::string, uint32_t> TakeSkipReasons() { return std::move(skipReasons_); }
  // Render-to-texture diagnostics since the last call: resolves seen, textures
  // that overlap a resolve's destination without matching it, textures whose
  // guest memory is all zeros (written by the GPU on the console, not here).
  std::map<std::string, uint32_t> TakeNotes() { return std::move(notes_); }
  const Stats& stats() const { return stats_; }

  // Starts recording a frame into the next set of per-frame resources. The
  // GPU may still be drawing the previous frame; only the frame that used
  // these resources last (kFrames ago) is waited for.
  void BeginFrame() {
    stats_.draws = stats_.skipped = 0;
    stats_.newPipelines = stats_.newTextures = stats_.reuploads = 0;
    stats_.uploadBytes = 0;
    stats_.recordMs = stats_.waitMs = 0;
    StashFrame();
    LoadFrame((cur_ + 1) % kFrames);
    if (pending_[cur_]) {
      auto start = std::chrono::steady_clock::now();
      vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
      stats_.waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      pending_[cur_] = false;
    }
    FreeFrame();
    // Texture heap slots added while this frame's set was in flight.
    for (; heapApplied_[cur_] < heapLog_.size(); ++heapApplied_[cur_]) {
      auto [index, view] = heapLog_[heapApplied_[cur_]];
      WriteHeapSlot(sets_[kTextureSet], index, view);
    }
    constants_.used = vertices_.used = indices_.used = staging_.used = 0;
    ++frame_;
    cmd_ = AllocCommandBuffer();
    uploads_ = AllocCommandBuffer();
    frameOps_.clear();
  }

  // Render-target traffic from the D3D layer; state = device + kStateBegin.
  // Clear: fills the current render target with an ARGB color.
  void Clear(const uint8_t* state, uint32_t argb) {
    Op op{};
    op.kind = Op::kClear;
    op.target = TargetOf(state);
    op.color = argb;
    frameOps_.push_back(op);
  }

  // Resolve: copies a rectangle of the current render target (x1, y1, x2, y2
  // in its pixels; null = the current viewport) into the texture described by
  // destFetch (its fetch constant, big-endian). Later draws that fetch that
  // address sample the copy instead of decoding guest memory, which the GPU
  // would have written on the console.
  void Resolve(const uint8_t* state, const int32_t* rect, const uint8_t* destFetch) {
    xenos::TextureFetch t = xenos::DecodeFetch(destFetch);
    // The destination texture's header carries its virtual address, while the
    // draws that later sample it fetch by physical address: translate, the way
    // the console maps it (0xE0000000+ views are offset by a page).
    if (t.base >= 0x20000000u) t.base = (t.base & 0x1FFFFFFFu) + (t.base >= 0xE0000000u ? 0x1000u : 0u);
    {
      char note[160];
      std::snprintf(note, sizeof(note), "resolve target %llx rect %s(%d,%d,%d,%d) -> %08X %ux%u fmt 0x%02X",
                    (unsigned long long)TargetOf(state), rect ? "" : "viewport ", rect ? rect[0] : 0, rect ? rect[1] : 0,
                    rect ? rect[2] : 0, rect ? rect[3] : 0, t.base, t.width, t.height, t.format);
      ++notes_[note];
    }
    if (t.width < 8 || t.height < 8) return;  // not a color texture (the front buffer)
    Op op{};
    op.kind = Op::kResolve;
    op.target = TargetOf(state);
    if (rect) {
      for (int i = 0; i < 4; ++i) op.rect[i] = float(rect[i]);
    } else {
      float v[4];
      GameViewport(state, v);
      op.rect[0] = v[0], op.rect[1] = v[1], op.rect[2] = v[0] + v[2], op.rect[3] = v[1] + v[3];
    }
    op.resolved = ResolvedFor(t);
    if (op.resolved) frameOps_.push_back(op);
  }

  // Captures one draw: copies its vertices, indices and constants right away,
  // since the game reuses the memory after the call. Every primitive type is
  // turned into an indexed triangle list (32-bit indices).
  void Draw(const DrawCall& d) {
    uint32_t prim = d.primitive;
    bool known = prim == 4 || prim == 5 || prim == 6 || prim == 13;  // list, fan, strip, quad list
    if (d.entry > 2 || !known || (d.entry == 0 && !d.ibAddress) || (d.entry == 2 && !d.upData)) {
      Skip("draw entry " + std::to_string(d.entry) + " primitive " + std::to_string(prim));
      return;
    }
    VkShaderModule vsm = Module(d.vs, true), psm = Module(d.ps, false);
    if (!vsm || !psm) {
      Skip(std::string("missing SPIR-V ") + Hex(!vsm ? d.vs : d.ps) + (!vsm ? "_vs" : "_ps"));
      return;
    }
    // Stream 0's stride: SetStreamSource keeps stride / 4 in a byte at device + 0x3268 + stream.
    // DrawVerticesUP passes its own.
    uint32_t stride = d.entry == 2 ? d.indexCount : uint32_t(d.state[0x3268 - kStateBegin]) * 4;
    std::string missing;
    const Layout* layout = LayoutFor(d.vs, stride, &missing);
    if (!layout) {
      std::string in;
      for (uint32_t l : inputs_[d.vs]) in += (in.empty() ? "" : ",") + std::to_string(l);
      Skip("vertex layout " + Hex(d.vs) + " inputs (" + in + "): " + missing);
      return;
    }
    // The vertices (source and size) and the vertex indices the primitive uses.
    const uint8_t* vb = nullptr;
    uint32_t vsize = 0;
    int64_t vertexBias = 0;  // indexed draws: base vertex
    std::vector<uint32_t>& src = srcScratch_;  // reused: no allocation per draw
    bool resettable = false;
    if (d.entry == 2) {
      // DrawVerticesUP(prim, vertexCount, data, stride): vertices inline.
      uint32_t count = d.baseVertex;
      vb = d.upData;
      vsize = count * stride;
      src.resize(count);
      for (uint32_t i = 0; i < count; ++i) src[i] = i;
    } else {
      const uint8_t* vf = d.state + 95 * 8;  // stream 0 = vertex fetch 95
      uint32_t vbase = xenos::BE32(vf) & 0x1FFFFFFC;
      vsize = ((xenos::BE32(vf + 4) >> 2) & 0xFFFFFF) * 4;
      vb = memory_(vbase, vsize);
      if (d.entry == 1) {
        // DrawVertices(prim, start, count).
        src.resize(d.startIndex);
        for (uint32_t i = 0; i < d.startIndex; ++i) src[i] = d.baseVertex + i;
      } else {
        uint32_t isize = (d.ibWord0 & 0x80000000u) ? 4 : 2;
        uint32_t ibPhys = (d.ibAddress & 0x1FFFFFFF) + (d.ibAddress >= 0xE0000000u ? 0x1000 : 0);
        const uint8_t* idx = memory_(ibPhys + d.startIndex * isize, d.indexCount * isize);
        if (!idx) { ++stats_.skipped; return; }
        src.resize(d.indexCount);
        for (uint32_t i = 0; i < d.indexCount; ++i)
          src[i] = isize == 2 ? uint32_t(idx[i * 2] << 8 | idx[i * 2 + 1]) : xenos::BE32(idx + i * 4);
        vertexBias = int32_t(d.baseVertex);
        resettable = true;
      }
    }
    std::vector<uint32_t>& tris = trisScratch_;
    Triangles(prim, src, resettable, tris);
    if (!vb || !vsize || tris.empty()) {
      ++stats_.skipped;
      return;
    }
    // Copy (and byte-swap) only the vertices the primitive uses, not the whole
    // buffer the fetch constant describes: the game shares big vertex buffers
    // between many small draws. Indices are rebased to the copied range.
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t v : tris) lo = std::min(lo, v), hi = std::max(hi, v);
    int64_t first = vertexBias + int64_t(lo);
    size_t begin = 0, bytes = vsize;
    uint32_t rebase = 0;
    bool ranged = first >= 0 && uint64_t(first) * stride < vsize;
    if (ranged) {
      begin = size_t(first) * stride;
      bytes = std::min<size_t>(vsize - begin, (size_t(hi - lo) + 1) * stride);
      rebase = lo;
    }
    if (vertices_.used + bytes + 16 > vertices_.size || indices_.used + tris.size() * 4 > indices_.size ||
        constants_.used + 16384 > constants_.size) {
      Skip("per-frame buffers full");
      return;
    }
    Recorded r{};
    r.vertexOffset = ranged ? CopyVertices(vb + begin, uint32_t(bytes), *layout)
                            : CopyVertices(vb, vsize, *layout) + size_t(vertexBias * layout->stride);
    r.indexOffset = indices_.Alloc(tris.size() * 4, 4);
    uint32_t* out = reinterpret_cast<uint32_t*>(indices_.map + r.indexOffset);
    for (size_t i = 0; i < tris.size(); ++i) out[i] = tris[i] - rebase;
    r.indexCount = uint32_t(tris.size());
    r.indexType = VK_INDEX_TYPE_UINT32;
    FillConstants(d, r);
    uint32_t blend = xenos::BE32(d.state + (0x2934 - kStateBegin) + 4);  // RB_BLENDCONTROL0
    r.pipeline = Pipeline(d.vs, d.ps, vsm, psm, *layout, blend);
    if (!r.pipeline) { ++stats_.skipped; return; }
    r.target = TargetOf(d.state);
    GameViewport(d.state, r.viewport);
    Op op{};
    op.kind = Op::kDraw;
    op.target = r.target;
    op.draw = r;
    frameOps_.push_back(op);
    ++stats_.draws;
  }

  // Render target identity: EDRAM base and format (RB_COLOR_INFO) and pitch
  // (RB_SURFACE_INFO). Each one gets its own image.
  static uint64_t TargetOf(const uint8_t* state) {
    uint32_t surface = xenos::BE32(state + (0x2880 - kStateBegin));
    uint32_t color = xenos::BE32(state + (0x2884 - kStateBegin));
    return uint64_t(color & 0x000F0FFF) << 16 | (surface & 0x3FFF);
  }
  static uint32_t PitchOf(uint64_t target) { return uint32_t(target & 0x3FFF); }

  // The game's viewport (PA_CL_VPORT_* at device + 0x2908) in target pixels.
  static void GameViewport(const uint8_t* state, float v[4]) {
    auto f = [&](uint32_t off) {
      uint32_t b = xenos::BE32(state + (off - kStateBegin));
      float x;
      std::memcpy(&x, &b, 4);
      return std::fabs(x);
    };
    float xs = f(0x2908), xo = f(0x290C), ys = f(0x2910), yo = f(0x2914);
    v[0] = xo - xs, v[1] = yo - ys, v[2] = 2 * xs, v[3] = 2 * ys;
  }

  // Triangle-list indices for a Xenos primitive over the vertex indices `v`.
  // Strips and fans restart at the reset index (0xFFFF / 0xFFFFFFFF) when indexed.
  static void Triangles(uint32_t prim, const std::vector<uint32_t>& v, bool resettable, std::vector<uint32_t>& out) {
    out.clear();
    auto reset = [&](uint32_t i) { return resettable && (i == 0xFFFFu || i == 0xFFFFFFFFu); };
    if (prim == 4) {
      out.assign(v.begin(), v.begin() + v.size() / 3 * 3);
    } else if (prim == 13) {
      for (size_t q = 0; q + 3 < v.size(); q += 4) out.insert(out.end(), {v[q], v[q + 1], v[q + 2], v[q], v[q + 2], v[q + 3]});
    } else {
      size_t begin = 0;
      for (size_t i = 0; i <= v.size(); ++i) {
        if (i < v.size() && !reset(v[i])) continue;
        // Run [begin, i) without reset indices.
        for (size_t k = begin + 2; k < i; ++k) {
          if (prim == 6) {
            // Strip: keep a consistent winding (culling is off, but be exact).
            bool odd = (k - begin) % 2 == 1;
            out.insert(out.end(), {v[k - 2], odd ? v[k] : v[k - 1], odd ? v[k - 1] : v[k]});
          } else {
            out.insert(out.end(), {v[begin], v[k - 1], v[k]});  // fan
          }
        }
        begin = i + 1;
      }
    }
  }

  // Vertex data: 8in32 swap of the whole range, then back to memory order for
  // 8-bit integer attributes (e.g. blend indices), which Vulkan reads byte-wise.
  size_t CopyVertices(const uint8_t* vb, uint32_t vsize, const Layout& layout) {
    size_t at = vertices_.Alloc(vsize, 16);
    for (uint32_t i = 0; i + 4 <= vsize; i += 4) {
      uint32_t w = xenos::BE32(vb + i);
      std::memcpy(vertices_.map + at + i, &w, 4);
    }
    for (uint32_t offset : layout.byteOrderedOffsets)
      for (uint32_t v = 0; v + layout.stride <= vsize; v += layout.stride)
        std::memcpy(vertices_.map + at + v + offset, vb + v + offset, 4);
    return at;
  }

  void FillConstants(const DrawCall& d, Recorded& r) {
    auto bytes = [this](uint64_t key) {
      auto it = constantBytes_.find(key);
      return it != constantBytes_.end() ? it->second : 4096u;
    };
    r.vsConst = CopyConstants(d.state + (0x780 - kStateBegin), bytes(d.vs ^ 0x8000000000000000ull));
    r.psConst = CopyConstants(d.state + (0x1780 - kStateBegin), bytes(d.ps));
    size_t sharedAt = constants_.Alloc(512, 256);
    uint8_t* shared = constants_.map + sharedAt;
    std::memset(shared, 0, 512);
    for (uint32_t s = 0; s < 16; ++s) {
      const uint8_t* tp = d.state + s * 24;
      if ((xenos::BE32(tp) & 3) != 2) continue;
      xenos::TextureFetch t = xenos::DecodeFetch(tp);
      int index = Texture(t, tp);
      if (index < 0) continue;
      uint32_t u = uint32_t(index), samp = std::min(t.clampX, 2u) * 3 + std::min(t.clampY, 2u);
      std::memcpy(shared + s * 4, &u, 4);
      std::memcpy(shared + 192 + s * 4, &samp, 4);
    }
    r.shared = uint32_t(sharedAt);
  }

  // Submits the frame. Presents to the window, or reads back RGBA8 into `readback`.
  bool EndFrame(std::vector<uint8_t>* readback) {
    auto recordStart = std::chrono::steady_clock::now();
    EndCommandBuffer(uploads_);
    uint32_t imageIndex = 0;
    VkImage present = offscreen_.image;
    if (surface_) {
      VkResult r = swapchain_ ? vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &imageIndex)
                              : VK_ERROR_OUT_OF_DATE_KHR;
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        swapchainStale_ = true;
        SubmitUploadsOnly();
        return false;
      }
      present = swapchainImages_[imageIndex];
    }
    // The main target is the widest one (the back buffer); its frame goes to
    // the screen letterboxed to the game's aspect (wider with the widescreen
    // patch: the game then squeezes a wider view into the same back buffer).
    uint64_t main = 0;
    float mainW = 0, mainH = 0;
    std::map<uint64_t, float> heights;  // lowest row each target uses, in game pixels
    for (const Op& op : frameOps_) {
      float bottom = op.kind == Op::kDraw ? op.draw.viewport[1] + op.draw.viewport[3]
                     : op.kind == Op::kResolve ? op.rect[3] : 0;
      heights[op.target] = std::max(heights[op.target], bottom);
      uint32_t pitch = PitchOf(op.target);
      if (pitch > PitchOf(main) || (pitch == PitchOf(main) && op.target < main)) main = op.target;
    }
    for (const Op& op : frameOps_)
      if (op.kind == Op::kDraw && op.target == main) {
        mainW = std::max(mainW, op.draw.viewport[0] + op.draw.viewport[2]);
        mainH = std::max(mainH, op.draw.viewport[1] + op.draw.viewport[3]);
      }
    if (mainW < 1 || mainH < 1) mainW = 1280, mainH = 720;
    // pw x ph: the picture on screen. rw x rh: the size it is rendered at
    // (render scale), stretched to pw x ph by the final blit.
    float pw = std::min(float(extent_.width), extent_.height * aspect_), ph = pw / aspect_;
    float rw = std::max(1.0f, std::round(pw * renderScale_)), rh = std::max(1.0f, std::round(ph * renderScale_));
    float sx = rw / mainW, sy = rh / mainH;  // game pixels -> target image pixels

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, kConstantSet, sets_, 0, nullptr);
    Target* current = nullptr;
    auto endPass = [&] {
      if (current) vkCmdEndRenderPass(cmd_);
      current = nullptr;
    };
    auto use = [&](uint64_t key) -> Target* {
      float h = std::max(heights[key], key == main ? mainH : 1.0f);
      uint32_t w = key == main ? uint32_t(rw) : uint32_t(PitchOf(key) * sx + 0.5f);
      return TargetImage(key, std::max(w, 1u), std::max(uint32_t(h * sy + 0.5f), 1u));
    };
    for (const Op& op : frameOps_) {
      if (op.kind == Op::kResolve) {
        endPass();
        Target* src = use(op.target);
        Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Barrier(cmd_, op.resolved->image.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {int32_t(op.rect[0] * sx), int32_t(op.rect[1] * sy), 0};
        blit.srcOffsets[1] = {std::min(int32_t(op.rect[2] * sx + 0.5f), int32_t(src->width)),
                              std::min(int32_t(op.rect[3] * sy + 0.5f), int32_t(src->height)), 1};
        blit.dstOffsets[1] = {int32_t(op.resolved->width), int32_t(op.resolved->height), 1};
        if (blit.srcOffsets[1].x > blit.srcOffsets[0].x && blit.srcOffsets[1].y > blit.srcOffsets[0].y)
          vkCmdBlitImage(cmd_, src->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, op.resolved->image.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        Barrier(cmd_, op.resolved->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        continue;
      }
      if (!current || current->key != op.target) {
        endPass();
        current = use(op.target);
        Transition(current, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = renderPass_;
        rb.framebuffer = current->framebuffer;
        rb.renderArea = {{0, 0}, {current->width, current->height}};
        vkCmdBeginRenderPass(cmd_, &rb, VK_SUBPASS_CONTENTS_INLINE);
        VkRect2D scissor{{0, 0}, {current->width, current->height}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
      }
      if (op.kind == Op::kClear) {
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        ca.clearValue.color = {{((op.color >> 16) & 255) / 255.0f, ((op.color >> 8) & 255) / 255.0f,
                                (op.color & 255) / 255.0f, (op.color >> 24) / 255.0f}};
        VkClearRect cr{{{0, 0}, {current->width, current->height}}, 0, 1};
        vkCmdClearAttachments(cmd_, 1, &ca, 1, &cr);
        continue;
      }
      const Recorded& r = op.draw;
      VkViewport viewport{r.viewport[0] * sx, r.viewport[1] * sy, r.viewport[2] * sx, r.viewport[3] * sy, 0, 1};
      if (viewport.width <= 0 || viewport.height <= 0) continue;
      vkCmdSetViewport(cmd_, 0, 1, &viewport);
      vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
      uint32_t offsets[3] = {r.vsConst, r.psConst, r.shared};
      vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, kConstantSet, 1,
                              &sets_[kConstantSet], 3, offsets);
      VkDeviceSize vo = r.vertexOffset;
      vkCmdBindVertexBuffers(cmd_, 0, 1, &vertices_.buffer, &vo);
      vkCmdBindIndexBuffer(cmd_, indices_.buffer, r.indexOffset, r.indexType);
      vkCmdDrawIndexed(cmd_, r.indexCount, 1, 0, 0, 0);
    }
    endPass();

    // The main target, letterboxed, onto the screen (or the offscreen image).
    VkImageLayout presentLayout = surface_ ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Barrier(cmd_, present, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{{0, 0, 0, 1}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, present, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    auto it = targets_.find(main);
    if (it != targets_.end()) {
      Target* t = &it->second;
      Transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkImageBlit blit{};
      blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      // Only the rendered area: the image keeps its largest size when the
      // render scale goes down.
      blit.srcOffsets[1] = {std::min(int32_t(rw), int32_t(t->width)),
                            std::min(int32_t(std::round(mainH * sy)), int32_t(t->height)), 1};
      int32_t x0 = int32_t((extent_.width - pw) / 2), y0 = int32_t((extent_.height - ph) / 2);
      blit.dstOffsets[0] = {x0, y0, 0};
      blit.dstOffsets[1] = {x0 + int32_t(pw), y0 + int32_t(ph), 1};
      vkCmdBlitImage(cmd_, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    Barrier(cmd_, present, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, presentLayout);
    if (readback) {
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {extent_.width, extent_.height, 1};
      if (surface_) Barrier(cmd_, present, presentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      vkCmdCopyImageToBuffer(cmd_, present, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &copy);
      if (surface_) Barrier(cmd_, present, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, presentLayout);
    }
    vkEndCommandBuffer(cmd_);
    VkCommandBuffer cmds[2] = {uploads_, cmd_};
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = cmds;
    if (surface_) {
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &acquired_;
      submit.pWaitDstStageMask = &waitStage;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &rendered_;
    }
    vkResetFences(device_, 1, &fence_);
    vkQueueSubmit(queue_, 1, &submit, fence_);
    if (surface_) {
      VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      presentInfo.waitSemaphoreCount = 1;
      presentInfo.pWaitSemaphores = &rendered_;
      presentInfo.swapchainCount = 1;
      presentInfo.pSwapchains = &swapchain_;
      presentInfo.pImageIndices = &imageIndex;
      VkResult pr = vkQueuePresentKHR(queue_, &presentInfo);
      // Not VK_SUBOPTIMAL_KHR: Android returns it on every present while the
      // swapchain's pre-transform (identity, the compositor rotates) differs
      // from the display's rotation, and the frame still shows correctly.
      // Treating it as stale re-created the swapchain (device idle + new
      // gralloc buffers) on every frame.
      if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_ERROR_SURFACE_LOST_KHR) swapchainStale_ = true;
    }
    stats_.recordMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordStart).count();
    // No wait here: the next BeginFrame uses the other frame's resources and
    // only waits for this one when it comes around again.
    pending_[cur_] = true;
    if (readback) {
      vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
      pending_[cur_] = false;
      readback->assign(readback_.map, readback_.map + size_t(extent_.width) * extent_.height * 4);
      if (surface_ && swapchainBgra_)
        for (size_t i = 0; i + 3 < readback->size(); i += 4) std::swap((*readback)[i], (*readback)[i + 2]);
    }
    return true;
  }

  // Drops the recorded frame without drawing it (no surface: the app is in
  // the background). Texture uploads are still submitted, since the cache
  // already counts them as done.
  void DiscardFrame() {
    EndCommandBuffer(uploads_);
    SubmitUploadsOnly();
  }

  // True after the swapchain stopped matching its surface (resize, rotation,
  // surface lost): call Recreate().
  bool stale() const { return swapchainStale_; }

  // New swapchain for the current surface, or for a new surface from the
  // factory (Android hands out a new window when the app comes back).
  bool Recreate(SurfaceFactory surface = nullptr) {
    vkDeviceWaitIdle(device_);
    swapchainImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (surface) {
      if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
      surface_ = surface(instance_);
      if (!surface_) return Fail("surface creation failed");
    }
    swapchainStale_ = false;
    return CreateSwapchain();
  }

  // Aspect ratio of the frames the game draws (width / height).
  void SetAspect(float aspect) { aspect_ = aspect; }

  // Fraction of the on-screen size the game is rendered at (0.25 .. 1); the
  // frame is stretched to the screen. Takes effect on the next frame.
  void SetRenderScale(float scale) { renderScale_ = std::clamp(scale, 0.25f, 1.0f); }
  float renderScale() const { return renderScale_; }

  uint32_t width() const { return extent_.width; }
  uint32_t height() const { return extent_.height; }

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    size_t size = 0, used = 0;
    size_t Alloc(size_t bytes, size_t align) {
      used = (used + align - 1) & ~(align - 1);
      size_t at = used;
      used += bytes;
      return at;
    }
  };
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  void Skip(const std::string& reason) {
    ++stats_.skipped;
    ++skipReasons_[reason];
  }
  static std::string Hex(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)v);
    return buf;
  }

  bool Fail(const std::string& message) {
    error_ = message;
    return false;
  }

  bool CreateInstance(const std::vector<const char*>& extra) {
    std::vector<const char*> exts = extra;
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> avail(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());
    bool portability = false;
    for (auto& e : avail) portability |= !std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (portability) exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Rayman Origins native renderer";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (portability) info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(exts.size());
    info.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) return Fail("vkCreateInstance failed");
    volkLoadInstance(instance_);
    return true;
  }

  bool CreateDevice() {
    auto stage = [this](const char* st) { if (log) log(st); };
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (!count) return Fail("no Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    // A discrete GPU when there is one (laptops list the integrated one first).
    phys_ = devices[0];
    for (VkPhysicalDevice d : devices) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        phys_ = d;
        break;
      }
    }
    stage("device: physical device found");
    vkGetPhysicalDeviceProperties(phys_, &props_);
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &families, fam.data());
    for (uint32_t i = 0; i < families; ++i) {
      VkBool32 present = VK_TRUE;
      if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(phys_, i, surface_, &present);
      if ((fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { queueFamily_ = i; break; }
    }
    stage("device: queue family chosen");
    if (log) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "device: %s, api %u.%u, driver 0x%x, %u descriptor sets", props_.deviceName,
                    VK_API_VERSION_MAJOR(props_.apiVersion), VK_API_VERSION_MINOR(props_.apiVersion),
                    props_.driverVersion, props_.limits.maxBoundDescriptorSets);
      log(buf);
    }
    // Plain Vulkan 1.0 features only: the heaps are fixed-size arrays indexed
    // with a uniform index (spirv_patch.h).
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(phys_, &have);
    stage("device: features queried");
    if (!have.shaderSampledImageArrayDynamicIndexing) return Fail("GPU lacks shaderSampledImageArrayDynamicIndexing");
    if (props_.limits.maxBoundDescriptorSets < spirv::kSets ||
        props_.limits.maxPerStageDescriptorSampledImages < kMaxTextures + 2)
      return Fail("GPU descriptor limits too low");
    VkPhysicalDeviceFeatures want{};
    want.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    stage("device: features ok");
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, exts.data());
    std::vector<const char*> enable;
    for (auto& e : exts)
      if (!std::strcmp(e.extensionName, "VK_KHR_portability_subset")) enable.push_back("VK_KHR_portability_subset");
    if (surface_) enable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pEnabledFeatures = &want;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = uint32_t(enable.size());
    info.ppEnabledExtensionNames = enable.data();
    stage("device: vkCreateDevice");
    if (vkCreateDevice(phys_, &info, nullptr, &device_) != VK_SUCCESS) return Fail("vkCreateDevice failed");
    stage("device: created");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = queueFamily_;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device_, &pool, nullptr, &cmdPool_);
    return true;
  }

  // Render targets: RGBA8, contents kept between passes (EDRAM-like).
  static constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
  bool CreateRenderPass() {
    VkAttachmentDescription color{};
    color.format = kTargetFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    return vkCreateRenderPass(device_, &rp, nullptr, &renderPass_) == VK_SUCCESS;
  }

  bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &count, formats.data());
    VkSurfaceFormatKHR format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (auto& f : formats)
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { format = f; break; }
    extent_ = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{width_, height_};
    // Android reports a rotation from the panel's natural (portrait)
    // orientation; the extent is already the window's (landscape). Render
    // upright and let the compositor rotate: identity transform.
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = std::max(2u, caps.minImageCount);
    sc.imageFormat = format.format;
    sc.imageColorSpace = format.colorSpace;
    sc.imageExtent = extent_;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // filled by a blit
    sc.preTransform = transform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &sc, nullptr, &swapchain_) != VK_SUCCESS) return Fail("swapchain failed");
    uint32_t images = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &images, nullptr);
    std::vector<VkImage> list(images);
    vkGetSwapchainImagesKHR(device_, swapchain_, &images, list.data());
    swapchainImages_ = list;
    swapchainBgra_ = format.format == VK_FORMAT_B8G8R8A8_UNORM;
    if (readback_.size < size_t(extent_.width) * extent_.height * 4)
      readback_ = CreateBuffer(size_t(extent_.width) * extent_.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    return true;
  }

  bool CreateOffscreen() {
    extent_ = {width_, height_};
    offscreen_ = CreateImage(width_, height_, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    readback_ = CreateBuffer(size_t(width_) * height_ * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    return true;
  }

  // ---- Render targets and resolved textures ----
  struct Target {
    uint64_t key = 0;
    Image image;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  struct Resolved {
    Image image;
    int index = -1;
    uint32_t width = 0, height = 0;
  };
  struct Op {
    enum Kind { kDraw, kClear, kResolve } kind;
    uint64_t target;
    Recorded draw;
    uint32_t color;
    float rect[4];
    Resolved* resolved;
  };

  // The image behind a render target, (re)created at the size this frame needs.
  Target* TargetImage(uint64_t key, uint32_t w, uint32_t h) {
    Target& t = targets_[key];
    if (t.image.image && t.width >= w && t.height >= h) return &t;
    if (t.image.image) {  // grow: wait for the frames in flight, which may still use the old one
      vkQueueWaitIdle(queue_);
      vkDestroyFramebuffer(device_, t.framebuffer, nullptr);
      DestroyImage(t.image);
      w = std::max(w, t.width), h = std::max(h, t.height);
    }
    t.key = key;
    t.width = w, t.height = h;
    t.image = CreateImage(w, h, kTargetFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = renderPass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &t.image.view;
    fb.width = w, fb.height = h, fb.layers = 1;
    vkCreateFramebuffer(device_, &fb, nullptr, &t.framebuffer);
    // Start black (EDRAM contents are undefined anyway).
    Barrier(cmd_, t.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{{0, 0, 0, 1}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    t.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    return &t;
  }

  void Transition(Target* t, VkImageLayout to) {
    if (t->layout == to) return;
    Barrier(cmd_, t->image.image, t->layout, to);
    t->layout = to;
  }

  // The texture a resolve writes, keyed by its guest address; registered in
  // the texture heap once, sampled by index like any decoded texture.
  Resolved* ResolvedFor(const xenos::TextureFetch& t) {
    uint64_t key = uint64_t(t.base) << 32 | t.width << 16 | t.height;
    auto it = resolved_.find(key);
    if (it != resolved_.end()) return &it->second;
    if (textureCount_ >= kMaxTextures) return nullptr;
    Resolved& r = resolved_[key];
    r.width = t.width, r.height = t.height;
    r.image = CreateImage(t.width, t.height, kTargetFormat,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    // Black until the first resolve lands (in this frame's upload commands).
    if (!uploadsBegun_) {
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(uploads_, &begin);
      uploadsBegun_ = true;
    }
    Barrier(uploads_, r.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{{0, 0, 0, 0}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(uploads_, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    Barrier(uploads_, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    r.index = int(textureCount_++);
    resolvedByBase_[t.base] = &r;
    AddHeapSlot(uint32_t(r.index), r.image.view);
    stats_.textures = textureCount_;
    return &r;
  }

  void DestroyImage(Image& img) {
    vkDestroyImageView(device_, img.view, nullptr);
    vkDestroyImage(device_, img.image, nullptr);
    vkFreeMemory(device_, img.memory, nullptr);
    img = Image{};
  }

  static constexpr uint32_t kMaxTextures = 1024, kMaxSamplers = 16;
  // Descriptor sets (spirv_patch.h): 2D heap, 3D + cube, samplers, constants.
  static constexpr uint32_t kTextureSet = 0, kVolumeCubeSet = 1, kSamplerSet = 2, kConstantSet = 3;

  bool CreateDescriptors() {
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    auto layout = [&](uint32_t set, std::vector<VkDescriptorSetLayoutBinding> bindings) {
      for (auto& b : bindings) b.stageFlags = stages;
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      li.bindingCount = uint32_t(bindings.size());
      li.pBindings = bindings.data();
      return vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayouts_[set]) == VK_SUCCESS;
    };
    const VkDescriptorType image = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ubo = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    if (!layout(kTextureSet, {{0, image, kMaxTextures}}) || !layout(kVolumeCubeSet, {{0, image, 1}, {1, image, 1}}) ||
        !layout(kSamplerSet, {{0, VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers}}) ||
        !layout(kConstantSet, {{0, ubo, 1}, {1, ubo, 1}, {2, ubo, 1}}))
      return Fail("set layout");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = spirv::kSets;
    pl.pSetLayouts = setLayouts_;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) return Fail("pipeline layout");
    // The texture heap and the constant set are per frame in flight; the
    // 3D/cube placeholders and the samplers never change after this.
    VkDescriptorPoolSize sizes[] = {{image, kMaxTextures * kFrames + 2},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers},
                                    {ubo, 3 * kFrames}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = spirv::kSets + 2 * (kFrames - 1);
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_, &dp, nullptr, &pool_);
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = pool_;
    da.descriptorSetCount = spirv::kSets;
    da.pSetLayouts = setLayouts_;
    if (vkAllocateDescriptorSets(device_, &da, sets_) != VK_SUCCESS) return Fail("descriptor sets");
    frames_[0].textureSet = sets_[kTextureSet], frames_[0].constantSet = sets_[kConstantSet];
    for (uint32_t i = 1; i < kFrames; ++i) {
      VkDescriptorSetLayout pair[2] = {setLayouts_[kTextureSet], setLayouts_[kConstantSet]};
      VkDescriptorSet sets[2];
      da.descriptorSetCount = 2;
      da.pSetLayouts = pair;
      if (vkAllocateDescriptorSets(device_, &da, sets) != VK_SUCCESS) return Fail("descriptor sets");
      frames_[i].textureSet = sets[0], frames_[i].constantSet = sets[1];
    }
    // Without partially bound descriptors every slot must hold a valid image:
    // 1x1 placeholders until real textures take their slots.
    Image dummy2D = CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Image dummy3D = CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_VIEW_TYPE_3D);
    Image dummyCube = CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_VIEW_TYPE_CUBE);
    {
      VkCommandBuffer cmd = AllocCommandBuffer();
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(cmd, &begin);
      VkClearColorValue black{{0, 0, 0, 0}};
      for (auto [img, layers] : {std::pair{dummy2D.image, 1u}, {dummy3D.image, 1u}, {dummyCube.image, 6u}}) {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        Barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers);
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        Barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layers);
      }
      vkEndCommandBuffer(cmd);
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd;
      vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
      vkQueueWaitIdle(queue_);
      vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
    }
    {
      std::vector<VkDescriptorImageInfo> heap(kMaxTextures, {VK_NULL_HANDLE, dummy2D.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
      VkDescriptorImageInfo vol{VK_NULL_HANDLE, dummy3D.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo cube{VK_NULL_HANDLE, dummyCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w[2 + kFrames]{};
      for (auto& x : w) x = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, x.descriptorType = image, x.descriptorCount = 1;
      w[0].dstSet = sets_[kVolumeCubeSet], w[0].dstBinding = 0, w[0].pImageInfo = &vol;
      w[1].dstSet = sets_[kVolumeCubeSet], w[1].dstBinding = 1, w[1].pImageInfo = &cube;
      for (uint32_t i = 0; i < kFrames; ++i)
        w[2 + i].dstSet = frames_[i].textureSet, w[2 + i].descriptorCount = kMaxTextures, w[2 + i].pImageInfo = heap.data();
      vkUpdateDescriptorSets(device_, 2 + kFrames, w, 0, nullptr);
    }
    // Samplers: index = clampX * 3 + clampY over {wrap, mirror, clamp}.
    std::vector<VkDescriptorImageInfo> infos;
    for (uint32_t x = 0; x < 3; ++x)
      for (uint32_t y = 0; y < 3; ++y) {
        auto mode = [](uint32_t m) {
          return m == 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                 : m == 1 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        };
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = mode(x);
        si.addressModeV = mode(y);
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler s;
        vkCreateSampler(device_, &si, nullptr, &s);
        infos.push_back({s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
      }
    infos.resize(kMaxSamplers, infos[0]);  // every slot valid
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[kSamplerSet];
    w.descriptorCount = uint32_t(infos.size());
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = infos.data();
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return true;
  }

  uint32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
  }

  Buffer CreateBuffer(size_t size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    vkCreateBuffer(device_, &info, nullptr, &b.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &alloc, nullptr, &b.memory);
    vkBindBufferMemory(device_, b.buffer, b.memory, 0);
    vkMapMemory(device_, b.memory, 0, size, 0, reinterpret_cast<void**>(&b.map));
    return b;
  }

  Image CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D, VkComponentMapping swizzle = {}) {
    Image img;
    uint32_t layers = type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : 1;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (type == VK_IMAGE_VIEW_TYPE_CUBE) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    vkCreateImage(device_, &info, nullptr, &img.image);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &alloc, nullptr, &img.memory);
    vkBindImageMemory(device_, img.image, img.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = img.image;
    view.viewType = type;
    view.format = format;
    view.components = swizzle;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCreateImageView(device_, &view, nullptr, &img.view);
    return img;
  }

  VkCommandBuffer AllocCommandBuffer() {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = cmdPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc, &cmd);
    return cmd;
  }

  void EndCommandBuffer(VkCommandBuffer cmd) {
    if (!uploadsBegun_) {
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(cmd, &begin);
    }
    vkEndCommandBuffer(cmd);
    uploadsBegun_ = false;
  }

  // Submits only the frame's texture uploads (uploads_ already ended), then frees it.
  void SubmitUploadsOnly() {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &uploads_;
    vkResetFences(device_, 1, &fence_);
    vkQueueSubmit(queue_, 1, &submit, fence_);
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    FreeFrame();
  }

  void FreeFrame() {
    VkCommandBuffer cmds[2] = {uploads_, cmd_};
    vkFreeCommandBuffers(device_, cmdPool_, 2, cmds);
    uploads_ = cmd_ = VK_NULL_HANDLE;
  }

  // ---- Frames in flight ----
  // The members the recording code uses (constants_, cmd_, fence_, the
  // texture and constant sets...) are the current frame's; switching frames
  // swaps them with the stored ones.
  void StashFrame() {
    Frame& f = frames_[cur_];
    f.constants = constants_, f.vertices = vertices_, f.indices = indices_, f.staging = staging_;
    f.fence = fence_, f.acquired = acquired_, f.rendered = rendered_;
    f.cmd = cmd_, f.uploads = uploads_;
    f.textureSet = sets_[kTextureSet], f.constantSet = sets_[kConstantSet];
  }
  void LoadFrame(uint32_t index) {
    cur_ = index;
    const Frame& f = frames_[index];
    constants_ = f.constants, vertices_ = f.vertices, indices_ = f.indices, staging_ = f.staging;
    fence_ = f.fence, acquired_ = f.acquired, rendered_ = f.rendered;
    cmd_ = f.cmd, uploads_ = f.uploads;
    sets_[kTextureSet] = f.textureSet, sets_[kConstantSet] = f.constantSet;
  }

  // A new texture heap slot: written to the current frame's set now, and to
  // the other frames' sets when they come around (not while the GPU may be
  // reading them).
  void AddHeapSlot(uint32_t index, VkImageView view) {
    heapLog_.push_back({index, view});
    WriteHeapSlot(sets_[kTextureSet], index, view);
    heapApplied_[cur_] = heapLog_.size();
  }
  void WriteHeapSlot(VkDescriptorSet set, uint32_t index, VkImageView view) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t layers = 1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  }

  // Byte-swaps the first `bytes` of a 4 KB constant block (the registers the
  // shader reads); the descriptor's range stays 4 KB, the rest is never read.
  uint32_t CopyConstants(const uint8_t* src, uint32_t bytes) {
    size_t at = constants_.Alloc(std::max(bytes, 16u), 256);
    for (uint32_t i = 0; i < bytes; i += 4) {
      uint32_t w = xenos::BE32(src + i);
      std::memcpy(constants_.map + at + i, &w, 4);
    }
    return uint32_t(at);
  }

  // Texture cache keyed by the fetch constant (address, format, size). The
  // game reuses addresses for new content (movie frames are rewritten in place
  // every frame, streaming reuses memory), so each entry keeps a cheap content
  // signature and is re-decoded into the same image when it changes.
  struct TextureEntry {
    int index = -1;
    Image image;
    uint32_t span = 0;
    uint64_t signature = 0;
    uint64_t checkedFrame = 0;
  };

  uint64_t Signature(uint32_t base, uint32_t span) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < 32; ++i) {
      uint32_t off = uint32_t(uint64_t(span > 8 ? span - 8 : 0) * i / 31);
      const uint8_t* p = memory_(base + off, 8);
      uint64_t v = 0;
      if (p) std::memcpy(&v, p, 8);
      h = (h ^ v) * 1099511628211ull;
    }
    return h;
  }

  // Decodes the texture straight into the staging buffer and records its copy
  // into the image. False (nothing recorded) if it doesn't decode or fit.
  bool Upload(const Image& img, const xenos::TextureFetch& t, bool fresh) {
    size_t bytes = xenos::DecodedSize(t);
    size_t start = staging_.used;
    size_t at = staging_.Alloc(bytes, 16);
    if (staging_.used > staging_.size || !xenos::DecodeTextureTo(memory_, t, staging_.map + at)) {
      staging_.used = start;
      return false;
    }
    if (!uploadsBegun_) {
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(uploads_, &begin);
      uploadsBegun_ = true;
    }
    stats_.uploadBytes += bytes;
    ++(fresh ? stats_.newTextures : stats_.reuploads);
    if (fresh) {
      // All zeros: memory the console's GPU would have written (render to texture).
      const uint8_t* p = staging_.map + at;
      bool zero = true;
      for (size_t i = 0; zero && i < std::min<size_t>(bytes, 65536); ++i) zero = p[i] == 0;
      if (zero) {
        char note[128];
        std::snprintf(note, sizeof(note), "texture %08X %ux%u fmt 0x%02X is all zeros in guest memory", t.base, t.width,
                      t.height, t.format);
        ++notes_[note];
      }
    }
    Barrier(uploads_, img.image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t.width, t.height, 1};
    vkCmdCopyBufferToImage(uploads_, staging_.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(uploads_, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
  }

  int Texture(const xenos::TextureFetch& t, const uint8_t* fetch) {
    if (auto rit = resolvedByBase_.find(t.base); rit != resolvedByBase_.end()) return rit->second->index;
    uint64_t key = uint64_t(xenos::BE32(fetch + 4)) << 32 | xenos::BE32(fetch + 8);
    auto it = textures_.find(key);
    if (it != textures_.end()) {
      TextureEntry& e = it->second;
      if (e.index < 0) return -1;
      // Content changes are checked once per frame, not on every draw that
      // samples the texture (movie planes change between frames).
      if (e.checkedFrame == frame_) return e.index;
      e.checkedFrame = frame_;
      uint64_t signature = Signature(t.base, e.span);
      if (signature != e.signature && Upload(e.image, t, false)) e.signature = signature;
      return e.index;
    }
    // A texture inside a resolve's destination that doesn't start at it: the
    // resolved copy won't be found by address.
    for (auto& [rbase, r] : resolvedByBase_) {
      uint32_t rsize = r->width * r->height * 4;
      if (t.base != rbase && t.base >= rbase && t.base < rbase + rsize) {
        char note[160];
        std::snprintf(note, sizeof(note), "texture %08X %ux%u fmt 0x%02X lies inside resolve dest %08X %ux%u", t.base,
                      t.width, t.height, t.format, rbase, r->width, r->height);
        ++notes_[note];
      }
    }
    TextureEntry& e = textures_[key];
    if (!xenos::Supported(t.format) || textureCount_ >= kMaxTextures) {
      ++skipReasons_["texture format 0x" + std::to_string(t.format) + " (" + std::to_string(t.width) + "x" +
                     std::to_string(t.height) + ") not supported"];
      return -1;
    }
    // k_8 stays one channel: R8 read through an RRRR swizzle, the same values
    // as the four replicated channels at a quarter of the bytes.
    bool r8 = xenos::DecodedBytesPerTexel(t.format) == 1;
    e.image = CreateImage(t.width, t.height, r8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D,
                          r8 ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                                  VK_COMPONENT_SWIZZLE_R}
                             : VkComponentMapping{});
    if (!Upload(e.image, t, true)) {
      DestroyImage(e.image);
      textures_.erase(key);
      return -1;
    }
    // Bytes the base level spans (an upper bound over its tiled layout).
    bool block = t.format == 0x12 || t.format == 0x13 || t.format == 0x14;
    uint32_t blockBytes = t.format == 0x12 ? 8 : block ? 16 : t.format == 0x06 ? 4 : 1;
    uint32_t bw = block ? (t.width + 3) / 4 : t.width, bh = block ? (t.height + 3) / 4 : t.height;
    e.span = ((std::max(t.pitch / (block ? 4 : 1), bw) + 31) & ~31u) * ((bh + 31) & ~31u) * blockBytes;
    e.signature = Signature(t.base, e.span);
    e.checkedFrame = frame_;
    e.index = int(textureCount_++);
    AddHeapSlot(uint32_t(e.index), e.image.view);
    stats_.textures = textureCount_;
    return e.index;
  }

  VkShaderModule Module(uint64_t hash, bool vertex) {
    uint64_t key = hash ^ (vertex ? 0x8000000000000000ull : 0);
    auto it = modules_.find(key);
    if (it != modules_.end()) return it->second;
    std::vector<uint32_t> code = shaders_ ? shaders_(hash, vertex) : std::vector<uint32_t>{};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!code.empty()) {
      VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = code.size() * 4;
      ci.pCode = code.data();
      if (vertex) inputs_[hash] = spirv::VertexInputLocations(code);
      // Constant bytes this shader reads (set 4 binding 0 VS / 1 PS before the patch).
      constantBytes_[key] = spirv::UniformBlockBytes(code, 4, vertex ? 0 : 1, 4096);
      code = spirv::PatchForVulkan11(spirv::FixSetpInv(code), kMaxTextures, kMaxSamplers);
      ci.codeSize = code.size() * 4;
      ci.pCode = code.data();
      vkCreateShaderModule(device_, &ci, nullptr, &m);
    }
    return modules_[key] = m;
  }

  // UbiArt vertex structs by stride (measured from captured vertex data,
  // docs/D3D_MAP.md). Locations follow XenosRecomp: 0 position, 1 normal,
  // 4..7 texcoord 0..3, 8 color, 9 blend indices. D3D patches the vertex
  // fetches at draw time from the stream stride, so the stride picks the
  // struct and the shader's inputs pick the attributes from it.
  struct Attribute {
    uint32_t location;
    VkFormat format;
    uint32_t offset;
    bool byteOrdered;  // 8-bit integer attribute, kept in memory byte order
  };
  static const std::vector<Attribute>* Struct(uint32_t stride) {
    const VkFormat f2 = VK_FORMAT_R32G32_SFLOAT, f3 = VK_FORMAT_R32G32B32_SFLOAT, f4 = VK_FORMAT_R32G32B32A32_SFLOAT,
                   c = VK_FORMAT_B8G8R8A8_UNORM, u4 = VK_FORMAT_R8G8B8A8_UINT;
    static const std::vector<Attribute> pc{{0, f3, 0}, {8, c, 12}};                              // 16
    static const std::vector<Attribute> pt{{0, f3, 0}, {4, f2, 12}};                             // 20
    static const std::vector<Attribute> pct{{0, f3, 0}, {8, c, 12}, {4, f2, 16}};                // 24
    static const std::vector<Attribute> font{{0, f3, 0}, {8, c, 12}, {9, u4, 16, true}, {4, f2, 20}};  // 28
    static const std::vector<Attribute> pnct{{0, f3, 0}, {1, f3, 12}, {8, c, 24}, {4, f2, 28}};  // 36
    static const std::vector<Attribute> patch{{0, f3, 0}, {8, c, 12}, {4, f2, 16}, {5, f4, 24}, {6, f4, 40}, {7, f2, 56}};  // 64
    switch (stride) {
      case 16: return &pc;
      case 20: return &pt;
      case 24: return &pct;
      case 28: return &font;
      case 36: return &pnct;
      case 64: return &patch;
      default: return nullptr;
    }
  }

  const Layout* LayoutFor(uint64_t vs, uint32_t stride, std::string* missing) {
    auto it = inputs_.find(vs);
    if (it == inputs_.end()) return nullptr;
    LayoutKey key{vs, stride};
    auto cached = layouts_.find(key);
    if (cached != layouts_.end()) return &cached->second;
    const std::vector<Attribute>* fields = Struct(stride);
    if (!fields) {
      *missing = "stride " + std::to_string(stride);
      return nullptr;
    }
    Layout layout{stride, {}, {}};
    for (uint32_t location : it->second) {
      const Attribute* a = nullptr;
      for (const Attribute& f : *fields)
        if (f.location == location) a = &f;
      if (!a) {
        *missing = "location " + std::to_string(location) + " in stride " + std::to_string(stride);
        return nullptr;
      }
      layout.attributes.push_back({location, 0, a->format, a->offset});
      if (a->byteOrdered) layout.byteOrderedOffsets.push_back(a->offset);
    }
    return &(layouts_[key] = layout);
  }

  static VkBlendFactor BlendFactor(uint32_t x) {
    static const VkBlendFactor map[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return x < 17 ? map[x] : VK_BLEND_FACTOR_ONE;
  }
  static VkBlendOp BlendOp(uint32_t x) {
    switch (x) {
      case 1: return VK_BLEND_OP_SUBTRACT;
      case 2: return VK_BLEND_OP_MIN;
      case 3: return VK_BLEND_OP_MAX;
      case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
      default: return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline Pipeline(uint64_t vs, uint64_t ps, VkShaderModule vsm, VkShaderModule psm, const Layout& layout,
                      uint32_t blend) {
    PipelineKey key{vs, ps, blend, layout.stride};
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psm, "main"};
    VkVertexInputBindingDescription bind{0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = uint32_t(layout.attributes.size());
    vi.pVertexAttributeDescriptions = layout.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    uint32_t cs = blend & 31, cop = (blend >> 5) & 7, cd = (blend >> 8) & 31;
    uint32_t as = (blend >> 16) & 31, aop = (blend >> 21) & 7, ad = (blend >> 24) & 31;
    cb.blendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
    cb.srcColorBlendFactor = BlendFactor(cs);
    cb.dstColorBlendFactor = BlendFactor(cd);
    cb.colorBlendOp = BlendOp(cop);
    cb.srcAlphaBlendFactor = BlendFactor(as);
    cb.dstAlphaBlendFactor = BlendFactor(ad);
    cb.alphaBlendOp = BlendOp(aop);
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &ds;
    gp.layout = pipelineLayout_;
    gp.renderPass = renderPass_;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &gp, nullptr, &pipe);
    stats_.pipelines = uint32_t(pipelines_.size() + 1);
    ++stats_.newPipelines;
    return pipelines_[key] = pipe;
  }

  uint32_t width_ = 0, height_ = 0;
  float aspect_ = 16.0f / 9.0f;
  float renderScale_ = 1.0f;
  VkExtent2D extent_{};
  bool ready_ = false;
  std::string error_;
  ShaderSource shaders_;
  xenos::MemoryReader memory_;
  Stats stats_;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props_{};
  VkPhysicalDeviceMemoryProperties memProps_{};
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> swapchainImages_;
  bool swapchainStale_ = false;
  bool swapchainBgra_ = false;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  Image offscreen_;
  Buffer readback_, constants_, vertices_, indices_, staging_;
  VkDescriptorSetLayout setLayouts_[spirv::kSets]{};
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet sets_[spirv::kSets]{};
  // Frames in flight (see StashFrame / LoadFrame).
  static constexpr uint32_t kFrames = 2;
  struct Frame {
    Buffer constants, vertices, indices, staging;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE, uploads = VK_NULL_HANDLE;
    VkDescriptorSet textureSet = VK_NULL_HANDLE, constantSet = VK_NULL_HANDLE;
  };
  Frame frames_[kFrames];
  uint32_t cur_ = 0;
  bool pending_[kFrames] = {};                         // submitted, fence not waited yet
  std::vector<std::pair<uint32_t, VkImageView>> heapLog_;  // texture heap slots, in order
  size_t heapApplied_[kFrames] = {};                   // heapLog_ entries in each frame's set
  VkFence fence_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE, rendered_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE, uploads_ = VK_NULL_HANDLE;
  bool uploadsBegun_ = false;
  std::vector<Op> frameOps_;
  std::vector<uint32_t> srcScratch_, trisScratch_;  // per-draw index lists, reused
  uint64_t frame_ = 0;                              // BeginFrame count
  std::unordered_map<uint64_t, uint32_t> constantBytes_;  // shader module key -> constant bytes it reads
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  std::vector<uint8_t> pipelineCacheSeed_;
  std::map<uint64_t, Target> targets_;
  std::unordered_map<uint64_t, Resolved> resolved_;
  std::unordered_map<uint32_t, Resolved*> resolvedByBase_;
  std::unordered_map<uint64_t, TextureEntry> textures_;
  uint32_t textureCount_ = 0;
  std::unordered_map<uint64_t, VkShaderModule> modules_;
  std::unordered_map<uint64_t, std::vector<uint32_t>> inputs_;
  // Cache keys (plain values: looked up on every draw).
  struct PipelineKey {
    uint64_t vs, ps;
    uint32_t blend, stride;
    bool operator==(const PipelineKey& o) const { return vs == o.vs && ps == o.ps && blend == o.blend && stride == o.stride; }
  };
  struct LayoutKey {
    uint64_t vs;
    uint32_t stride;
    bool operator==(const LayoutKey& o) const { return vs == o.vs && stride == o.stride; }
  };
  struct KeyHash {
    static uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull + (h >> 29); }
    size_t operator()(const PipelineKey& k) const {
      return size_t(Mix(Mix(Mix(k.vs, k.ps), k.blend), k.stride));
    }
    size_t operator()(const LayoutKey& k) const { return size_t(Mix(k.vs, k.stride)); }
  };
  std::unordered_map<PipelineKey, VkPipeline, KeyHash> pipelines_;
  std::unordered_map<LayoutKey, Layout, KeyHash> layouts_;
  std::map<std::string, uint32_t> skipReasons_;
  std::map<std::string, uint32_t> notes_;
};

}  // namespace native
