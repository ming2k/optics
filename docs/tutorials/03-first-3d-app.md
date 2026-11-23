# Your first 3D scene application

You will draw a cube. By the end you will understand `optics_gfx_camera`,
`optics_gfx_mesh`, `optics_gfx_material`, and the depth-tested pass.

## Step 1 — Start from `examples/showcase/scene_studio/main.c`

The example is the smallest 3D program possible. Copy it; we will
narrate what each part does instead of writing from scratch.

## Step 2 — read the vertex array

Look at `cube_verts`. Eight corners, indexed for 12 triangles (CCW from
outside). Each vertex is `optics_gfx_vertex`: position (vec3), normal (vec3),
uv (vec2). The indexed draw uses 36 indices (6 faces × 2 triangles ×
3 verts).

You can substitute your own geometry by providing different
`vertices` / `indices` to `optics_gfx_mesh_create`.

## Step 3 — read the camera setup

    optics_gfx_camera cam;
    optics_gfx_camera_perspective(&cam, &(optics_gfx_camera_perspective_opts){
        .fov_y_rad = 1.0f,
        .aspect = (float)width / (float)height,
        .z_near = 0.1f,
        .z_far = 100.0f,
    });
    optics_gfx_camera_look_at(&cam,
        (optics_gfx_vec3){ 3, 2.5f, 4 },    /* eye */
        (optics_gfx_vec3){ 0, 0, 0 },       /* target */
        (optics_gfx_vec3){ 0, 1, 0 });      /* up */

`optics_gfx_camera_perspective` produces a Vulkan-friendly projection: depth in
[0, 1] and Y flipped to match the default viewport. You can call the
underlying `optics_gfx_mat4_*` functions directly if you want a custom matrix.

## Step 4 — read the per-frame loop

    float t = (float)glfwGetTime();
    optics_gfx_quat q = optics_gfx_quat_axis_angle(
        optics_gfx_vec3_normalize((optics_gfx_vec3){ 0.3f, 1.0f, 0.2f }),
        t * 0.8f);
    optics_gfx_mat4 world = optics_gfx_mat4_rotation_quat(q);

    optics_gfx_pass_attachment color = {
        .view        = VK_NULL_HANDLE,
        .load_op     = OPTICS_GFX_LOAD_CLEAR,
        .store_op    = OPTICS_GFX_STORE_STORE,
        .clear_color = { 0.05f, 0.05f, 0.08f, 1.0f },
    };
    optics_gfx_pass_depth_attachment depth_att = {
        .view         = depth.view,
        .format       = DEPTH_FORMAT,
        .load_op      = OPTICS_GFX_LOAD_CLEAR,
        .store_op     = OPTICS_GFX_STORE_DONT_CARE,
        .clear_depth  = 1.0f,
    };
    optics_gfx_pass_desc pass = {
        .type                   = OPTICS_GFX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments      = &color,
        .depth                  = &depth_att,
    };
    optics_gfx_frame_begin_pass(frame, &pass);

    optics_gfx_scene_draw_mesh(frame, &cam, world, cube, mat);

    optics_gfx_frame_end_pass(frame);

`optics_gfx_scene_draw_mesh` records the mesh draw into the current pass. It
internally builds the MVP matrix from `camera.view` × `camera.projection`
and pushes it as a push constant. The material pipeline is bound
automatically.

The depth attachment is the key difference from a 2D pass: the caller
owns the depth image and supplies it in `optics_gfx_pass_desc.depth`. The
"modules own their attachments" rule lives in
[ADR-0001](../adr/0001-project-foundations.md), and the per-module
device state plumbing that supports it is recorded in
[ADR-0002](../adr/0002-per-module-device-state.md).

## Step 5 — make the cube bigger

Add a scaling matrix to the world transform:

    optics_gfx_mat4 scale = optics_gfx_mat4_scale(1.5f, 1.5f, 1.5f);
    optics_gfx_mat4 world = optics_gfx_mat4_multiply(scale, optics_gfx_mat4_rotation_quat(q));

Order matters: `optics_gfx_mat4_multiply(a, b)` means apply `b` first, then `a`.

## Step 6 — change the colour

    optics_gfx_material_desc mdesc = {
        .type         = OPTICS_GFX_TYPE_MATERIAL_DESC,
        .kind         = OPTICS_GFX_MATERIAL_UNLIT,
        .base_color   = { 1.0f, 0.4f, 0.25f, 1.0f },
        .color_format = optics_gfx_format_from_vk(optics_gfx_surface_vk_format(surface)),
        .depth_format = OPTICS_GFX_FORMAT_D32_SFLOAT,
    };
    optics_gfx_material *mat = nullptr;
    if (optics_gfx_material_create(device, &mdesc, &mat) != OPTICS_GFX_OK) {
        /* see optics_gfx_get_last_error for detail */
        return 1;
    }

Pass `mat` to `optics_gfx_scene_draw_mesh`. The material is refcounted; release
it on shutdown.

## What you now know

- `optics_gfx_mesh` is GPU vertex + index buffers; refcounted.
- `optics_gfx_material` (unlit or Phong) is refcounted and may retain a sampled
  base-color image/sampler through `optics_gfx_material_surface_desc`. See
  [How to Texture a Mesh](../how-to/texture-a-mesh.md).
- The caller owns the depth image; you supply it in the pass descriptor.
- 4×4 matrices are column-major and Vulkan-friendly.

## What's next

| Want to...                              | Read                                               |
|-----------------------------------------|----------------------------------------------------|
| Combine 2D and 3D in one frame          | [Application architecture](../explanation/application-architecture.md) |
| Understand the math types               | [`libs/gfx/include/optics/gfx/math.h`](../../libs/gfx/include/optics/gfx/math.h) |
| Add directional lighting                | [How to light a mesh](../how-to/light-a-mesh.md) |
