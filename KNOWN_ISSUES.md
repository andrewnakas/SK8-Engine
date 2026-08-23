# Known issues

This is an experimental preview rather than a finished standalone game engine.

- The Custom Engine Layer is fully validated only on Windows with D3D12. It
  builds and runs on Linux with Vulkan - the layer activates, the scene, shadow
  and HDR pipelines are created from the committed offline SPIR-V, and owned
  `.skate` maps load - but sustained gameplay has not been signed off. macOS
  still inherits upstream support without any Custom Engine Layer validation.
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
