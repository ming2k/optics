# ADR-0002: Per-module device state via opaque slot + destroy hook

- Status: Accepted
- Date: Stage 6.5

## Context

Modules (`canvas`, `scene`, `compute`) need to attach long-lived
state to `flux_device`. The canvas pipeline cache is the
load-bearing example: per-canvas pipeline creation is expensive
(~ms), so we cache pipelines keyed by swapchain colour format on
the device, shared by every canvas the app creates. The scene
module is likely to grow a similar cache for material pipelines.
Compute is closer to per-pipeline-object today but may grow shared
state (e.g. a system-default compute root signature) later.

The naive approach is to add typed fields to `flux_device` for
each module: `canvas_module_state *canvas;
scene_module_state *scene; compute_module_state *compute;`. That
forces `<core/internal.h>` to include every module's internal
header, dragging canvas + scene + compute internals into core. Core
should not need to know about its consumers.

We also need symmetric teardown: when the device is destroyed,
every module's state must be freed before the underlying
`VkDevice`, otherwise pipelines / shader modules / descriptor pools
leak. Burying module-specific teardown in `flux_device_release`
re-creates the dependency in the opposite direction.

## Decision

`flux_device` exposes one untyped state slot per module the device
explicitly knows about:

```c
struct flux_device {
    /* ... */
    void  *canvas_state;
    void (*canvas_state_destroy)(flux_device *d);
    /* future: scene_state / compute_state similar */
};
```

A module lazily attaches its state on first use:

```c
static canvas_module_state *canvas_state_get_or_init(flux_device *d) {
    if (d->canvas_state) return d->canvas_state;
    canvas_module_state *st = flux_internal_alloc(d, sizeof(*st));
    /* ... init ... */
    d->canvas_state         = st;
    d->canvas_state_destroy = canvas_state_destroy;
    return st;
}
```

`flux_device_release` invokes the destroy hook (if set) *before*
`vkDestroyDevice`, so module destructors still have a valid Vulkan
device to call against:

```c
if (d->canvas_state_destroy) d->canvas_state_destroy(d);
/* ... bindless heap destroy ... */
vkDestroyDevice(d->device, nullptr);
```

## Consequences

Positive:

- Zero header coupling between core and modules. `core/internal.h`
  exposes `void *` slots; module internals stay in their own
  translation units.
- One typed pointer + one callback per module. New modules add
  themselves with the same shape; no surgery on core.
- Teardown order is correct by construction: the destroy hook runs
  before the `VkDevice` is torn down.
- State is built lazily, so apps that never touch a module pay
  nothing for it (the canvas state slot stays NULL on a
  scene-only app).

Negative:

- Module state isn't fully type-safe at the device boundary
  (`void *` casts in each module). Acceptable: every cast is local
  to the module that owns the type.
- Generic registration (e.g. `flux_device_attach_state(name,
  destroy_fn, data)`) would be more uniform but adds a hash-table
  lookup per first-use and requires a name registry. Not worth it
  for the small handful of modules we'll ever have.

## Alternatives considered

- **Typed fields per module on `flux_device`.** Rejected: drags
  module headers into core, breaks the one-way coupling tenet.
- **Module-owned global state (file-static
  `canvas_module_state *`).** Rejected: a process with two flux
  devices (rare but not impossible — multi-GPU, multi-window
  servers) would share state across them.
- **A registration-based generic state map.** Rejected as
  premature; the slot pattern is sufficient for the small number
  of modules a graphics library has.

## See also

- `src/core/internal.h` — `canvas_state` / `canvas_state_destroy`
  fields on `struct flux_device`.
- `src/canvas/canvas.c` — `canvas_module_state`,
  `canvas_state_get_or_init`, `canvas_state_destroy`.
