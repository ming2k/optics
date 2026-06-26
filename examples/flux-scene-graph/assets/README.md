# flux-scene-graph example assets

Test glTF models used by the examples. Each entry lists its upstream source and
license; these files are third-party content, not flux project code.

## Duck.glb

- Source: Khronos® glTF-Sample-Models / glTF-Sample-Assets, `2.0/Duck/glTF-Binary/Duck.glb`
- Upstream: <https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/Duck>
- Copyright: © 2006 Sony Computer Entertainment Inc.
- License: **SCEA Shared Source License, Version 1.0**
  <https://web.archive.org/web/20160320123355/http://research.scea.com/scea_shared_source_license.html>

### Why this model

`Duck.glb` is an indexed mesh carrying `POSITION` and `NORMAL` attributes with no
skinning, so it exercises the loader's supported v0.1 subset and shades cleanly
under the built-in PHONG material (the loader does not decode textures). Its
POSITION bounds are roughly X[-69, 96], Y[10, 164], Z[-61, 54], so the camera
framing constants in `gltf_camera_tour.c` target roughly (13, 87, -4).
