// Reads the vertex input locations a SPIR-V vertex shader declares
// (Input-storage OpVariables decorated with Location, built-ins excluded).
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

namespace spirv {

inline std::vector<uint32_t> VertexInputLocations(const std::vector<uint32_t>& code) {
  std::unordered_map<uint32_t, uint32_t> location;  // id -> Location
  std::set<uint32_t> builtins;
  std::set<uint32_t> inputPointerTypes;  // OpTypePointer ids with storage class Input
  std::vector<std::pair<uint32_t, uint32_t>> variables;  // (result type, id) of OpVariable Input
  for (size_t i = 5; i < code.size();) {
    uint32_t word = code[i], op = word & 0xFFFF, count = word >> 16;
    if (!count || i + count > code.size()) break;
    const uint32_t* w = &code[i];
    if (op == 71 && count >= 4) {  // OpDecorate id decoration ...
      if (w[2] == 30) location[w[1]] = w[3];   // Location
      if (w[2] == 11) builtins.insert(w[1]);   // BuiltIn
    } else if (op == 32 && count >= 4 && w[2] == 1) {  // OpTypePointer Input
      inputPointerTypes.insert(w[1]);
    } else if (op == 59 && count >= 4 && w[3] == 1) {  // OpVariable type id Input
      variables.push_back({w[1], w[2]});
    }
    i += count;
  }
  std::vector<uint32_t> out;
  for (auto& [type, id] : variables) {
    if (builtins.count(id)) continue;
    auto it = location.find(id);
    if (it != location.end()) out.push_back(it->second);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Bytes the uniform block at (set, binding) actually spans: its last member's
// offset plus that member's size. XenosRecomp declares only the constant
// registers a shader reads, so this is usually far below the 4 KB the block
// can hold. Returns 0 when the shader has no block there, and `fallback` when
// the layout can't be worked out, or when the shader indexes the block with a
// computed index (relative constant addressing, c[a0 + n]): XenosRecomp sizes
// those arrays by the registers the microcode names, but the index can run
// past them (the map's vines index c145.. up to c224), so the whole register
// file has to be there, as on the console.
inline uint32_t UniformBlockBytes(const std::vector<uint32_t>& code, uint32_t set, uint32_t binding,
                                  uint32_t fallback) {
  enum : uint32_t {
    OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeArray = 28, OpTypeStruct = 30,
    OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpAccessChain = 65, OpDecorate = 71,
    OpMemberDecorate = 72,
    DecArrayStride = 6, DecMatrixStride = 7, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35,
  };
  std::unordered_map<uint32_t, uint32_t> setOf, bindingOf, pointee, constant, arrayStride, matrixStride;
  std::unordered_map<uint32_t, std::vector<uint32_t>> structMembers;  // struct id -> member types
  std::unordered_map<uint64_t, uint32_t> memberOffset;                        // struct << 32 | member
  std::unordered_map<uint32_t, uint32_t> scalarBytes, vectorOf, vectorCount, arrayElem, arrayLength, matrixCols, matrixCol;
  std::vector<std::pair<uint32_t, uint32_t>> variables;  // (pointer type, id)
  std::vector<std::vector<uint32_t>> chains;             // OpAccessChain: base, indices...
  for (size_t i = 5; i < code.size();) {
    uint32_t op = code[i] & 0xFFFF, n = code[i] >> 16;
    if (!n || i + n > code.size()) return fallback;
    const uint32_t* w = &code[i];
    switch (op) {
      case OpDecorate:
        if (n >= 4 && w[2] == DecDescriptorSet) setOf[w[1]] = w[3];
        if (n >= 4 && w[2] == DecBinding) bindingOf[w[1]] = w[3];
        if (n >= 4 && w[2] == DecArrayStride) arrayStride[w[1]] = w[3];
        break;
      case OpMemberDecorate:
        if (n >= 5 && w[3] == DecOffset) memberOffset[uint64_t(w[1]) << 32 | w[2]] = w[4];
        if (n >= 5 && w[3] == DecMatrixStride) matrixStride[w[1]] = w[4];
        break;
      case OpTypeInt:
      case OpTypeFloat: scalarBytes[w[1]] = w[2] / 8; break;
      case OpTypeVector: vectorOf[w[1]] = w[2], vectorCount[w[1]] = w[3]; break;
      case OpTypeMatrix: matrixCol[w[1]] = w[2], matrixCols[w[1]] = w[3]; break;
      case OpTypeArray: arrayElem[w[1]] = w[2], arrayLength[w[1]] = w[3]; break;
      case OpTypeStruct: structMembers[w[1]].assign(w + 2, w + n); break;
      case OpTypePointer: pointee[w[1]] = w[3]; break;
      case OpConstant: if (n >= 4) constant[w[2]] = w[3]; break;
      case OpVariable: variables.push_back({w[1], w[2]}); break;
      case OpAccessChain: if (n >= 5) chains.emplace_back(w + 3, w + n); break;
    }
    i += n;
  }
  // Size of a type in the block's layout (std140-like: vectors of 4 bytes each).
  std::function<uint32_t(uint32_t)> size = [&](uint32_t type) -> uint32_t {
    if (auto s = scalarBytes.find(type); s != scalarBytes.end()) return s->second;
    if (auto v = vectorOf.find(type); v != vectorOf.end()) return size(v->second) * vectorCount[type];
    if (auto m = matrixCol.find(type); m != matrixCol.end()) return 16 * matrixCols[type];
    if (auto a = arrayElem.find(type); a != arrayElem.end()) {
      auto len = constant.find(arrayLength[type]);
      auto stride = arrayStride.find(type);
      if (len == constant.end()) return 0;
      return (stride != arrayStride.end() ? stride->second : size(a->second)) * len->second;
    }
    if (auto st = structMembers.find(type); st != structMembers.end()) {
      uint32_t end = 0;
      for (uint32_t m = 0; m < st->second.size(); ++m) {
        auto off = memberOffset.find(uint64_t(type) << 32 | m);
        if (off == memberOffset.end()) return 0;
        uint32_t ms = size(st->second[m]);
        if (!ms) return 0;
        end = std::max(end, off->second + ms);
      }
      return end;
    }
    return 0;
  };
  for (auto& [ptr, var] : variables) {
    if (!setOf.count(var) || setOf[var] != set || bindingOf[var] != binding) continue;
    auto p = pointee.find(ptr);
    if (p == pointee.end()) return fallback;
    for (const auto& chain : chains)
      if (chain[0] == var)
        for (size_t k = 1; k < chain.size(); ++k)
          if (!constant.count(chain[k])) return fallback;  // computed index
    uint32_t bytes = size(p->second);
    return bytes ? std::min(fallback, (bytes + 15) & ~15u) : fallback;
  }
  return 0;  // the shader has no such block
}

}  // namespace spirv
