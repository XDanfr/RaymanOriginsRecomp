// vkrender: renders a frame dump (rex/src/native_capture.cpp) offline with the
// native renderer (vk_renderer.h): the game's own shaders as SPIR-V, no Xenos
// emulation.
//
// Usage: vkrender <native_frame.bin> <spirv dir> <out.tga>
//   <spirv dir> holds <HASH>_vs.spv / <HASH>_ps.spv.
#define DUMPVIEW_NO_MAIN
#include "dumpview.cpp"

#include <cstdlib>
#include <fstream>
#include <set>

#include "vk_renderer.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s native_frame.bin spirv_dir out.tga\n", argv[0]);
    return 1;
  }
  dv::Dump dump;
  if (!dv::Load(argv[1], dump)) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  std::string dir = argv[2];
  native::Renderer renderer;
  if (!renderer.Init(nullptr, {}, nullptr, 1280, 720)) {
    std::fprintf(stderr, "renderer: %s\n", renderer.error().c_str());
    return 1;
  }
  renderer.SetShaderSource([&](uint64_t hash, bool vertex) {
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llX_%s.spv", (unsigned long long)hash, vertex ? "vs" : "ps");
    std::ifstream f(dir + name, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
  });
  renderer.SetMemory([&](uint32_t address, uint32_t size) { return dv::Memory(dump, address, size); });

  // VKRENDER_ONLY=12,13: render just those draws (by index), to isolate one.
  std::set<size_t> only;
  if (const char* list = std::getenv("VKRENDER_ONLY"))
    for (const char* p = list; *p;) {
      char* end;
      only.insert(std::strtoul(p, &end, 10));
      p = *end ? end + 1 : end;
    }
  renderer.BeginFrame();
  for (size_t i = 0; i < dump.draws.size(); ++i) {
    const dv::Draw& d = dump.draws[i];
    if (!only.empty() && !only.count(i)) continue;
    native::DrawCall call{d.device.data(), d.entry, d.prim, d.a5, d.a6, d.a7, d.vs, d.ps, d.ibWord0, d.ibAddress};
    renderer.Draw(call);
  }
  std::vector<uint8_t> rgba;
  renderer.EndFrame(&rgba);
  for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
  dv::WriteTga(argv[3], renderer.width(), renderer.height(), rgba);
  auto& s = renderer.stats();
  std::printf("%u draws, %u skipped, %u pipelines, %u textures -> %s\n", s.draws, s.skipped, s.pipelines,
              s.textures, argv[3]);
  return 0;
}
