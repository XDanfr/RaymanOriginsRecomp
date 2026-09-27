// Rewrites the SPIR-V that XenosRecomp + DXC produce so it runs on plain
// Vulkan 1.1 GPUs: no descriptor indexing and only 4 descriptor sets (Adreno
// 6xx on their stock drivers report maxBoundDescriptorSets = 4).
//
// XenosRecomp declares its descriptor heaps as unsized arrays, one per set:
//   set 0 Texture2D[], set 1 Texture3D[], set 2 TextureCube[], set 3 SamplerState[],
//   set 4 the three constant buffers (bindings 0-2).
// After the patch:
//   set 0 binding 0 Texture2D[textures]
//   set 1 binding 0 Texture3D[1], binding 1 TextureCube[1]
//   set 2 binding 0 SamplerState[samplers]
//   set 3 bindings 0-2 constant buffers
// The heaps are indexed with a per-draw uniform index, which Vulkan 1.0's
// shaderSampledImageArrayDynamicIndexing covers, so NonUniform goes too.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spirv {

struct Binding {
  uint32_t set, binding, count;  // count: array length for the heaps
};

// Where a (set, binding) of the original shader goes.
inline Binding Remap(uint32_t set, uint32_t binding, uint32_t textures, uint32_t samplers) {
  switch (set) {
    case 0: return {0, binding, textures};
    case 1: return {1, 0, 1};
    case 2: return {1, 1, 1};
    case 3: return {2, binding, samplers};
    default: return {3, binding, 0};
  }
}

constexpr uint32_t kSets = 4;

inline std::vector<uint32_t> PatchForVulkan11(const std::vector<uint32_t>& code, uint32_t textures, uint32_t samplers) {
  if (code.size() < 5 || code[0] != 0x07230203u) return code;
  enum : uint32_t {
    OpExtension = 10, OpCapability = 17, OpTypeInt = 21, OpTypeArray = 28, OpTypeRuntimeArray = 29,
    OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71,
    DecBinding = 33, DecDescriptorSet = 34, DecNonUniform = 5300,
  };
  auto isIndexingCapability = [](uint32_t c) { return c >= 5301 && c <= 5311; };  // ShaderNonUniform .. StorageTexelBufferArrayNonUniformIndexing

  // Pass 1: what points at what.
  std::unordered_map<uint32_t, uint32_t> setOf, bindingOf, pointee, variableType;
  std::unordered_set<uint32_t> runtimeArrays;
  uint32_t uintType = 0;
  for (size_t i = 5; i < code.size();) {
    const uint32_t* w = &code[i];
    uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
    if (!n || i + n > code.size()) return code;  // malformed: leave it alone
    if (op == OpDecorate && n >= 4 && w[2] == DecDescriptorSet) setOf[w[1]] = w[3];
    if (op == OpDecorate && n >= 4 && w[2] == DecBinding) bindingOf[w[1]] = w[3];
    if (op == OpTypePointer && n >= 4) pointee[w[1]] = w[3];
    if (op == OpVariable && n >= 4) variableType[w[2]] = w[1];
    if (op == OpTypeRuntimeArray) runtimeArrays.insert(w[1]);
    if (op == OpTypeInt && n >= 4 && w[2] == 32 && w[3] == 0) uintType = w[1];
    i += n;
  }
  // Each runtime array's new length, from the set of the variable that uses it.
  std::unordered_map<uint32_t, uint32_t> lengthOf;
  for (auto& [var, ptr] : variableType) {
    auto p = pointee.find(ptr);
    if (p == pointee.end() || !runtimeArrays.count(p->second)) continue;
    uint32_t set = setOf.count(var) ? setOf[var] : 0;
    uint32_t len = Remap(set, bindingOf.count(var) ? bindingOf[var] : 0, textures, samplers).count;
    uint32_t& at = lengthOf[p->second];
    at = std::max(at, std::max(len, 1u));
  }
  for (uint32_t ra : runtimeArrays)
    if (!lengthOf.count(ra)) lengthOf[ra] = 1;

  // Pass 2: rewrite.
  uint32_t bound = code[3];
  bool newUint = !uintType;
  if (newUint) uintType = bound++;
  std::unordered_map<uint32_t, uint32_t> constantFor;  // length -> OpConstant id
  for (auto& [ra, len] : lengthOf)
    if (!constantFor.count(len)) constantFor[len] = bound++;

  std::vector<uint32_t> out(code.begin(), code.begin() + 5);
  out.reserve(code.size() + 16);
  bool typesEmitted = false;
  for (size_t i = 5; i < code.size();) {
    const uint32_t* w = &code[i];
    uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
    i += n;
    if (op == OpCapability && isIndexingCapability(w[1])) continue;
    if (op == OpExtension) {
      std::string name(reinterpret_cast<const char*>(w + 1), (n - 1) * 4);
      if (name.c_str() == std::string("SPV_EXT_descriptor_indexing")) continue;
    }
    if (op == OpDecorate && n >= 3 && w[2] == DecNonUniform) continue;
    if (op == OpDecorate && n >= 4 && (w[2] == DecDescriptorSet || w[2] == DecBinding)) {
      uint32_t id = w[1];
      Binding b = Remap(setOf.count(id) ? setOf[id] : 0, bindingOf.count(id) ? bindingOf[id] : 0, textures, samplers);
      out.insert(out.end(), {w[0], id, w[2], w[2] == DecDescriptorSet ? b.set : b.binding});
      continue;
    }
    if (op == OpTypeInt && w[1] == uintType && typesEmitted) continue;  // already moved up
    if (op == OpTypeRuntimeArray || (op == OpTypeInt && w[1] == uintType)) {
      if (!typesEmitted) {
        // The uint type and the lengths, ahead of the first array that needs them.
        out.insert(out.end(), {(4u << 16) | OpTypeInt, uintType, 32, 0});
        for (auto& [len, id] : constantFor) out.insert(out.end(), {(4u << 16) | OpConstant, uintType, id, len});
        typesEmitted = true;
      }
      if (op == OpTypeInt) continue;
      out.insert(out.end(), {(4u << 16) | OpTypeArray, w[1], w[2], constantFor[lengthOf[w[1]]]});
      continue;
    }
    out.insert(out.end(), w, w + n);
  }
  out[3] = bound;
  return out;
}

// Fixes XenosRecomp's setp_inv (predicate counter invert). The Xenos scalar op
// (ucode.h, kSetpInv) is
//     dest = src == 1.0 ? 0.0 : (src == 0.0 ? 1.0 : src)
// XenosRecomp emits `src == 0.0 ? 1.0 : src` and drops the first case, so the
// counter stays at 1 instead of dropping to 0; a later setp_*_push then counts
// 2 instead of 1 and the `== 1` branch it guards never runs. The map's vines
// and the sky bridge (a frieze, VS DDADD473) come out broken or invisible.
// The pattern, OpSelect %float (OpFOrdEqual a 0.0) 1.0 a, is setp_inv's
// alone; each one becomes select(a == 1.0, 0.0, <the original select>).
inline std::vector<uint32_t> FixSetpInv(const std::vector<uint32_t>& code) {
  if (code.size() < 5 || code[0] != 0x07230203u) return code;
  enum : uint32_t { OpTypeBool = 20, OpTypeFloat = 22, OpConstant = 43, OpSelect = 169, OpFOrdEqual = 180 };
  uint32_t floatType = 0, boolType = 0;
  std::unordered_map<uint32_t, uint32_t> floatConstant;                 // id -> bits
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> equal;    // OpFOrdEqual id -> operands
  for (size_t i = 5; i < code.size();) {
    const uint32_t* w = &code[i];
    uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
    if (!n || i + n > code.size()) return code;
    if (op == OpTypeFloat && n >= 3 && w[2] == 32) floatType = w[1];
    if (op == OpTypeBool) boolType = w[1];
    if (op == OpConstant && n == 4 && w[1] == floatType) floatConstant[w[2]] = w[3];
    if (op == OpFOrdEqual && n == 5) equal[w[2]] = {w[3], w[4]};
    i += n;
  }
  uint32_t zero = 0, one = 0;
  for (auto& [id, bits] : floatConstant) {
    if (bits == 0) zero = id;
    if (bits == 0x3F800000u) one = id;
  }
  if (!floatType || !boolType || !zero || !one) return code;
  uint32_t bound = code[3];
  std::vector<uint32_t> out(code.begin(), code.begin() + 5);
  out.reserve(code.size() + 64);
  for (size_t i = 5; i < code.size();) {
    const uint32_t* w = &code[i];
    uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
    i += n;
    if (op == OpSelect && n == 6 && w[1] == floatType && w[4] == one) {
      auto eq = equal.find(w[3]);
      uint32_t a = w[5];
      if (eq != equal.end() && ((eq->second.first == a && eq->second.second == zero) ||
                                (eq->second.first == zero && eq->second.second == a))) {
        uint32_t inner = bound++, isOne = bound++;
        out.insert(out.end(), {w[0], floatType, inner, w[3], one, a});
        out.insert(out.end(), {(5u << 16) | OpFOrdEqual, boolType, isOne, a, one});
        out.insert(out.end(), {(6u << 16) | OpSelect, floatType, w[2], isOne, zero, inner});
        continue;
      }
    }
    out.insert(out.end(), w, w + n);
  }
  out[3] = bound;
  return out;
}

}  // namespace spirv
