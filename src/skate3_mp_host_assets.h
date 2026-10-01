#pragma once

// Host-built render assets for the multiplayer module: meshes and textures
// that do not exist in guest memory, such as another player's outfit rebuilt
// from the local game's Create-a-Skater archives.
//
// They live in the renderer's ordinary caches under keys no guest address
// uses: a mesh under DrawItem::mesh = its key, a texture reached through
// DrawItem::*_tex = its object key (a tex_routes entry pointing at a
// tex_store entry). Items that use them must be `retained`, so the draw path
// never tries to read guest memory for them.
//
// Uploads are queued from any thread and recorded at the start of the next
// rendered frame (outside any render pass). The caches may still evict an
// entry that goes unused for long enough, or flush everything from the debug
// dialog, so a user re-checks readiness each frame and queues again.

#include <cstdint>
#include <vector>

namespace skate3::mp_host_assets {

struct Mesh {
  std::uint32_t key = 0;          // DrawItem::mesh
  std::uint64_t fingerprint = 0;  // must equal DrawItem::fingerprint
  std::vector<float> vertices;    // 14 floats per vertex, the scene VS layout
  std::vector<std::uint16_t> indices;
};

struct Texture {
  std::uint32_t object_key = 0;  // what DrawItem texture fields name
  std::uint64_t store_key = 0;   // content key in the texture store
  bool bc3 = false;              // BC1 otherwise
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;  // bytes per row of 4x4 blocks
  std::vector<std::uint8_t> bytes;
};

void Queue(std::vector<Mesh> meshes, std::vector<Texture> textures);
void Release(const std::vector<std::uint32_t>& mesh_keys,
             const std::vector<std::uint32_t>& texture_object_keys);

// Render thread only (e.g. from mp::Hooks::on_render_scene).
bool MeshReady(std::uint32_t key);
bool TextureReady(std::uint32_t object_key);

}  // namespace skate3::mp_host_assets
