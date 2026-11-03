// Phase 2 motion guide: the shader modification bits are part of the pipeline cache
// key, so their position is a contract. If a bit moves, every stored pipeline and
// translation can be reused for a different variant, and the guide would sample a
// shader that never wrote the clip arena. Pin the positions and the size.

#include <rex/graphics/pipeline/shader/spirv_translator.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("motion guide modification bits are pinned", "[graphics]") {
  using Modification = rex::graphics::SpirvShaderTranslator::Modification;

  CHECK(sizeof(Modification) == sizeof(uint64_t));

  Modification vertex_modification;
  CHECK(vertex_modification.value == 0);
  vertex_modification.vertex.motion_guide = 1;
  // 32 bits of word 0, then host_vertex_shader_type (4) and tessellation_mode (2).
  CHECK(vertex_modification.value == (UINT64_C(1) << 38));

  Modification pixel_modification;
  CHECK(pixel_modification.value == 0);
  pixel_modification.pixel.motion_guide = 1;
  // 32 bits of word 0, then 8 + 1 + 4 + 1 + 3 bits of word 1.
  CHECK(pixel_modification.value == (UINT64_C(1) << 49));

  // The vertex and pixel bits are distinct, so a vertex-only variant cannot be
  // mistaken for a full one.
  CHECK(vertex_modification.value != pixel_modification.value);
}
