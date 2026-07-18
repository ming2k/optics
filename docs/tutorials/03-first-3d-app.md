# Your first 3D scene application

You will draw a cube. By the end you will understand `flux_camera`,
`flux_mesh`, `flux_material`, and the depth-tested pass.

## Step 1 — Start from `examples/flux/scene_cube.c`

The example is the smallest 3D program possible. Copy it; we will
narrate what each part does instead of writing from scratch.

## Step 2 — read the vertex array

Look at `cube_verts`. Eight corners, indexed for 12 triangles (CCW from
outside). Each vertex is `flux_vertex`: position (vec3), normal (vec3),
uv (vec2). The indexed draw uses 36 indices (6 faces × 2 triangles ×
3 verts).

You can substitute your own geometry by providing different
`vertices` / `indices` to `flux_mesh_create`.

## Step 3 — read the camera setup

    flux_camera cam;
    flux_camera_perspective(&cam, 1.0f,
                            (float)width / (float)height, 0.1f, 100.0f);
    flux_camera_look_at(&cam,
        (flux_vec3){ 3, 2.5f, 4 },    /* eye */
        (flux_vec3){ 0, 0, 0 },       /* target */
        (flux_vec3){ 0, 1, 0 });      /* up */

`flux_camera_perspective` produces a Vulkan-friendly projection: depth in
[0, 1] and Y flipped to match the default viewport. You can call the
underlying `flux_mat4_*` functions directly if you want a custom matrix.

## Step 4 — read the per-frame loop

    float t = (float)glfwGetTime();
    flux_quat q = flux_quat_axis_angle(
        flux_vec3_normalize((flux_vec3){ 0.3f, 1.0f, 0.2f }),
        t * 0.8f);
    flux_mat4 world = flux_mat4_rotation_quat(q);

    flux_pass_attachment color = {
        .view        = VK_NULL_HANDLE,
        .load_op     = FLUX_LOAD_CLEAR,
        .store_op    = FLUX_STORE_STORE,
        .clear_color = { 0.05f, 0.05f, 0.08f, 1.0f },
    };
    flux_pass_depth_attachment depth_att = {
        .view         = depth.view,
        .format       = DEPTH_FORMAT,
        .load_op      = FLUX_LOAD_CLEAR,
        .store_op     = FLUX_STORE_DONT_CARE,
        .clear_depth  = 1.0f,
    };
    flux_pass_desc pass = {
        .type                   = FLUX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments      = &color,
        .depth                  = &depth_att,
    };
    flux_frame_begin_pass(frame, &pass);

    flux_scene_draw_mesh(frame, &cam, world, cube, mat);

    flux_frame_end_pass(frame);

`flux_scene_draw_mesh` records the mesh draw into the current pass. It
internally builds the MVP matrix from `camera.view` × `camera.projection`
and pushes it as a push constant. The material pipeline is bound
automatically.

The depth attachment is the key difference from a 2D pass: the caller
owns the depth image and supplies it in `flux_pass_desc.depth`. The
"modules own their attachments" rule lives in
[ADR-0001](../adr/0001-project-foundations.md), and the per-module
device state plumbing that supports it is recorded in
[ADR-0002](../adr/0002-per-module-device-state.md).

## Step 5 — make the cube bigger

Add a scaling matrix to the world transform:

    flux_mat4 scale = flux_mat4_scale(1.5f, 1.5f, 1.5f);
    flux_mat4 world = flux_mat4_multiply(scale, flux_mat4_rotation_quat(q));

Order matters: `flux_mat4_multiply(a, b)` means apply `b` first, then `a`.

## Step 6 — change the colour

    flux_material_desc mdesc = {
        .type         = FLUX_TYPE_MATERIAL_DESC,
        .kind         = FLUX_MATERIAL_UNLIT,
        .base_color   = { 1.0f, 0.4f, 0.25f, 1.0f },
        .color_format = flux_format_from_vk(flux_surface_vk_format(surface)),
        .depth_format = FLUX_FORMAT_D32_SFLOAT,
    };
    flux_material *mat = nullptr;
    if (flux_material_create(device, &mdesc, &mat) != FLUX_OK) {
        /* see flux_get_last_error for detail */
        return 1;
    }

Pass `mat` to `flux_scene_draw_mesh`. The material is refcounted; release
it on shutdown.

## What you now know

- `flux_mesh` is GPU vertex + index buffers; refcounted.
- `flux_material` (unlit) is a colour-per-mesh; refcounted.
- The caller owns the depth image; you supply it in the pass descriptor.
- 4×4 matrices are column-major and Vulkan-friendly.

## What's next

| Want to...                              | Read                                               |
|-----------------------------------------|----------------------------------------------------|
| Combine 2D and 3D in one frame          | [Application architecture](../explanation/application-architecture.md) |
| Understand the math types               | [`libs/flux/include/flux/math.h`](../../libs/flux/include/flux/math.h) |
| Add directional lighting                | [How to light a mesh](../how-to/light-a-mesh.md) |
