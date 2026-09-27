// Tests for the SPIR-V fixes the native renderer applies to XenosRecomp's
// shaders (spirv_patch.h, spirv_inputs.h). Plain C++, no framework:
//   clang++ -std=c++20 -I tools/native_renderer tools/native_renderer/tests/spirv_patch_test.cpp -o spirv_patch_test
//   ./spirv_patch_test
// The modules are minimal instruction streams, enough for these passes (they
// scan instructions and don't need a valid entry point).
#include <cstdio>
#include <vector>

#include "spirv_inputs.h"
#include "spirv_patch.h"

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

uint32_t Op(uint32_t op, uint32_t words) { return words << 16 | op; }

std::vector<uint32_t> Header(uint32_t bound) { return {0x07230203u, 0x00010000u, 0, bound, 0}; }

// Finds `op` instructions and returns their words.
std::vector<std::vector<uint32_t>> Find(const std::vector<uint32_t>& code, uint32_t op) {
  std::vector<std::vector<uint32_t>> out;
  for (size_t i = 5; i < code.size();) {
    uint32_t n = code[i] >> 16;
    if ((code[i] & 0xFFFF) == op) out.emplace_back(code.begin() + i, code.begin() + i + n);
    i += n;
  }
  return out;
}

// %1 bool, %2 float, %3 = 0.0, %4 = 1.0, %5 = a (any float value),
// %6 = (a == 0.0), %7 = select(%6, 1.0, a): XenosRecomp's setp_inv.
std::vector<uint32_t> SetpInvModule(uint32_t selectTrue) {
  std::vector<uint32_t> m = Header(8);
  m.insert(m.end(), {Op(20, 2), 1});                        // OpTypeBool %1
  m.insert(m.end(), {Op(22, 3), 2, 32});                    // OpTypeFloat %2 32
  m.insert(m.end(), {Op(43, 4), 2, 3, 0});                  // OpConstant %2 %3 0.0
  m.insert(m.end(), {Op(43, 4), 2, 4, 0x3F800000u});        // OpConstant %2 %4 1.0
  m.insert(m.end(), {Op(43, 4), 2, 5, 0x40000000u});        // %5: stands in for a computed value
  m.insert(m.end(), {Op(180, 5), 1, 6, 5, 3});              // OpFOrdEqual %1 %6 %5 %3
  m.insert(m.end(), {Op(169, 6), 2, 7, 6, selectTrue, 5});  // OpSelect %2 %7 %6 <true> %5
  return m;
}

void TestSetpInv() {
  std::vector<uint32_t> fixed = spirv::FixSetpInv(SetpInvModule(4));
  auto selects = Find(fixed, 169), equals = Find(fixed, 180);
  Check(selects.size() == 2 && equals.size() == 2, "setp_inv: one compare and one select added");
  // The outer select keeps the original result id: select(a == 1.0, 0.0, inner).
  bool outer = false, inner = false, isOne = false;
  uint32_t innerId = 0, isOneId = 0;
  for (auto& e : equals)
    if (e[3] == 5 && e[4] == 4) isOne = true, isOneId = e[2];
  for (auto& s : selects)
    if (s[2] != 7 && s[3] == 6 && s[4] == 4 && s[5] == 5) inner = true, innerId = s[2];
  for (auto& s : selects)
    if (s[2] == 7 && s[3] == isOneId && s[4] == 3 && s[5] == innerId) outer = true;
  Check(isOne, "setp_inv: compares a with 1.0");
  Check(inner, "setp_inv: the original select survives as the inner one");
  Check(outer, "setp_inv: result = select(a == 1.0, 0.0, inner)");
  Check(fixed[3] == 10, "setp_inv: id bound grows by the two new ids");
}

void TestOtherSelectsUntouched() {
  // select(a == 0.0, 0.0, a) is not setp_inv (it is setp_rstr's shape).
  std::vector<uint32_t> m = SetpInvModule(3);
  Check(spirv::FixSetpInv(m) == m, "other selects are left alone");
}

// A uniform block at set 4 binding 0: struct { vec4 c[64]; }, read through
// an access chain whose index is a constant or a computed value.
std::vector<uint32_t> BlockModule(bool computedIndex) {
  std::vector<uint32_t> m = Header(20);
  m.insert(m.end(), {Op(71, 4), 5, 6, 16});         // OpDecorate %5 ArrayStride 16
  m.insert(m.end(), {Op(72, 5), 6, 0, 35, 0});      // OpMemberDecorate %6 0 Offset 0
  m.insert(m.end(), {Op(71, 4), 8, 34, 4});         // OpDecorate %8 DescriptorSet 4
  m.insert(m.end(), {Op(71, 4), 8, 33, 0});         // OpDecorate %8 Binding 0
  m.insert(m.end(), {Op(22, 3), 1, 32});            // OpTypeFloat %1 32
  m.insert(m.end(), {Op(23, 4), 2, 1, 4});          // OpTypeVector %2 %1 4
  m.insert(m.end(), {Op(21, 4), 3, 32, 1});         // OpTypeInt %3 32 1
  m.insert(m.end(), {Op(43, 4), 3, 4, 64});         // OpConstant %3 %4 64
  m.insert(m.end(), {Op(28, 4), 5, 2, 4});          // OpTypeArray %5 %2 %4
  m.insert(m.end(), {Op(30, 3), 6, 5});             // OpTypeStruct %6 %5
  m.insert(m.end(), {Op(32, 4), 7, 2, 6});          // OpTypePointer %7 Uniform %6
  m.insert(m.end(), {Op(59, 4), 7, 8, 2});          // OpVariable %7 %8 Uniform
  m.insert(m.end(), {Op(43, 4), 3, 9, 0});          // OpConstant %3 %9 0
  m.insert(m.end(), {Op(32, 4), 11, 2, 2});         // OpTypePointer %11 Uniform %2
  // %10 is not a constant: a computed index (c[a0 + n]).
  m.insert(m.end(), {Op(65, 6), 11, 12, 8, 9, computedIndex ? 10u : 9u});  // OpAccessChain
  return m;
}

void TestUniformBlockBytes() {
  Check(spirv::UniformBlockBytes(BlockModule(false), 4, 0, 4096) == 1024,
        "constant indices: the block's declared size (64 registers)");
  Check(spirv::UniformBlockBytes(BlockModule(true), 4, 0, 4096) == 4096,
        "computed index: the whole register file");
  Check(spirv::UniformBlockBytes(BlockModule(false), 4, 1, 4096) == 0, "no block at that binding");
}

}  // namespace

int main() {
  TestSetpInv();
  TestOtherSelectsUntouched();
  TestUniformBlockBytes();
  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
