# Known issues

This is an experimental preview rather than a finished standalone game engine.

- The Custom Engine Layer is fully validated only on Windows with D3D12. It
  builds and runs on Linux with Vulkan - the layer activates, the scene, shadow
  and HDR pipelines are created from the committed offline SPIR-V, and owned
  `.skate` maps load - but sustained gameplay has not been signed off. macOS
  still inherits upstream support without any Custom Engine Layer validation.
- A correct binary cannot be generated from a clean checkout on any platform.
  The runtime applies TU3 at load time, so the generated code must contain the
  TU3 function roots, but codegen can only consume a whole image:
  `patched_file_path` replaces it, and `patch_file_path` - which would apply
  the patch while keeping the base layout - is parsed but unimplemented
  ("XexPatcher not available"). Stable-base codegen therefore registers 45 of
  1,727 roots and dies on the first TU3-only call, while the patched image
  reaches 1727/1727 but discards every boundary override in
  `skate3_functions.toml`. This is why `AGENTS.md` requires building from a
  previously validated `generated/` tree. Wiring XexPatcher into codegen would
  remove the requirement.
- Built from the patched image (the only configuration that boots), the game is
  playable but takes an intermittent SIGSEGV in guest code at `sub_82B7C500`,
  storing through a pointer into the `0xA0000000` physical alias whose page was
  never allocated. The runtime's own access-violation recovery declines it, so
  the pointer is bad rather than merely unmapped - consistent with the missing
  base-image boundary overrides.
- Linux builds currently cannot use the stable-base codegen path that the layer
  is designed around. `generate-all` aborts inside the recompiler with a heap
  buffer overflow in `BuilderContext::emit_function_call`: a `CallTarget` holds
  a raw `FunctionNode*`, and `FunctionGraph::addFunction` frees the superseded
  node when a higher-authority add replaces it, leaving those edges dangling.
  This is very likely the same defect behind the partial TU3 root registration
  recorded in `AGENTS.md`. Until it is fixed, a Linux build needs either a
  previously validated `generated/` tree or the experimental
  `-DSKATE3_CODEGEN_PATCHED_TITLE_UPDATE=ON` path - and note that path replaces
  the function-boundary config, so the overrides in `skate3_functions.toml` are
  not applied and the result is not a supported configuration.
- On Vulkan the static world renders black under world-shading v2, while
  dynamic objects, the skater, the sky and the HUD all render correctly. The
  geometry is drawn - it occludes the sky along the horizon - so this is the
  v2 shading path evaluating to zero, not missing draws. v2 is therefore held
  off on Vulkan and the world falls back to the v1 flat response, which is
  visible and skateable; `skate3_native_render_scene_world_v2_vulkan=true`
  restores v2 for anyone debugging it. D3D12 is unaffected and still uses v2.
  The underlying defect is not yet understood: it is not MSAA resolve, the
  lightmaps, HDR, the GPU selected, the shader/SPIR-V table, or presentation -
  each of those was ruled out by A/B test. With v1 the ground still shades
  black, which may be the same defect or a second one.
- The raytraced mirrors and puddles are D3D12-only. The rexglue RHI has no
  ray-tracing abstraction, so on Linux the pass is compiled out and the
  authored planes are simply not drawn, exactly as on D3D12 hardware below
  raytracing tier 1.1.
- On Linux, Steam multiplayer, screenshots, memory snapshots, the in-game
  updater, procedural thunder audio, the input-lab command pipe, "Open Maps
  Folder", and ultrawide monitor auto-detection are inert. Each is guarded and
  degrades quietly rather than failing.
- Native AI/NPC route records export in SKATE v8, but reliable route following
  is shelved. Maps should request zero AI skaters for release use.
- Very large maps currently load complete visual and collision packages.
  Collision streaming, HLOD generation, asynchronous asset I/O, and a formal
  memory budget are future work.
- Automatic Blender-scene import favours immediate playability and may create
  more detailed collision than a shipping map needs. Authors can replace it
  with simpler collision proxies for performance and cleaner contact.
- DXR mirrors require compatible D3D12 ray-tracing hardware. Raster fallback
  behavior is intentionally limited.
- Map changes restart the process so static collision, grind, door, renderer,
  and physics resources are rebuilt coherently.
- The layer activates once normal Skate 3 gameplay reaches a stable local
  player. Frontend and loading screens still use the upstream runtime.
- No retail game files or retail maps are bundled. The included Feature Park
  is original project content. A legally obtained Skate 3 ISO is required on
  first start.
- Internet multiplayer is an App 480 development preview. Its first launch
  needs GitHub access to acquire the pinned Steam runtime and a running,
  signed-in Steam client. If setup fails, inspect
  `.cel-steam/bootstrap.log`; ordinary gameplay and the localhost multiplayer
  fallback remain available.
- Remote skeletal animation can retain a small, rapid movement jitter even
  when packet delivery is stable. Root position, rotation, and independent
  player animation are functional, but this presentation defect remains open.
- Remote skateboard wheels can remain slightly misaligned or deform during
  skating. Their final procedural transforms are replicated independently
  from the canonical body skeleton; the major wheel/hat separation is fixed,
  but this smaller wheel presentation defect remains open.

When reporting a problem, include the map name, GPU, driver version, selected
Graphics API and Renderer, framerate cap, controller backend, and the latest
`logs/skate3_*.log`, but never upload retail game data.
