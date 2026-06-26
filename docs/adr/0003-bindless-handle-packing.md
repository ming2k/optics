# ADR-0003: Bindless handles pack binding type into the high bits

- Status: Accepted
- Date: Stage 6.5

## Context

The bindless descriptor heap (set 0) has four bindings:

| Binding | Slot type        | Default cap |
|---------|------------------|-------------|
| 0       | `SAMPLED_IMAGE`  | 16 384      |
| 1       | `STORAGE_IMAGE`  |  4 096      |
| 2       | `SAMPLER`        |    256      |
| 3       | `STORAGE_BUFFER` |  4 096      |

Each binding has its own free-list-stack slot allocator. The public
API exposes `flux_bindless_handle` (`uint32_t`) and four
registration entry points that take resource-specific Vulkan
handles:

```c
flux_result flux_bindless_register_image  (..., flux_bindless_handle *);
flux_result flux_bindless_register_sampler(..., flux_bindless_handle *);
/* ... */
void        flux_bindless_release(flux_device *d, flux_bindless_handle h);
```

`flux_bindless_release` is a single entry point but must route the
slot index back to the correct binding's free-list stack. A bare
`uint32_t` slot index doesn't tell the release path which binding
to push to.

## Decision

Pack the binding type into the top 4 bits of the handle:

```
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|binding| slot index (28 bits)                                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Constants in `src/core/device.c`:

```c
#define FLUX_BL_BIND_SHIFT 28u
#define FLUX_BL_SLOT_MASK  ((1u << FLUX_BL_BIND_SHIFT) - 1u)
```

Registration packs `(binding << 28) | slot`; release unpacks.
Shaders that read the heap by index must mask off the top 4 bits
(or the library does it for them when storing the index into push
constants — see `canvas_image.frag` for the pattern).

## Consequences

Positive:

- Single `void release(handle)` entry point. No type-specific
  release functions; no side table.
- The packing is invisible to consumers who only ever pass the
  handle back to release. Consumers who want to use the handle as
  a shader index get the slot via a one-line mask.
- The 28-bit slot space (~268M) far exceeds any reasonable Vulkan
  binding capacity. We don't expect to hit it.
- Adding a new binding type (we have 16 slots; 4 are used) is one
  enum value + one pool — no handle layout change.

Negative:

- The packing leaks into shader code that uses handles directly:
  shaders must mask off the top 4 bits before indexing the heap.
  The canvas image fragment does this explicitly. The convention
  is documented at every `bindless_handle` use site in shaders.
- A binding-type tag is more bookkeeping than a bare uint32_t —
  but it's free at the call site (the registration function
  knows its binding) and worth the one mask in shaders for the
  zero-state release path.

## Alternatives considered

- **Type-specific release functions** (`flux_bindless_release_image`,
  `..._sampler`, etc.). Rejected: leaks the binding count into
  the public API; consumers that genuinely don't care still have
  to know the type to release.
- **A device-side side table mapping handle → binding.** Rejected:
  extra memory + a lookup per release. The packing is O(1) and
  zero-storage.
- **Use the full 32 bits for slot index, look the binding up via
  a per-binding-handle-range check.** Rejected: binding capacities
  vary by hardware, so the ranges aren't known until device
  creation. Storing the binding once in the handle is simpler.

## See also

- `src/core/device.c` — `pack_handle`, `handle_binding`,
  `handle_slot`, `FLUX_BL_BIND_SHIFT`.
- `src/canvas/shaders/canvas_image.frag` — the mask-off-tag
  pattern in shader code.
- `tests/test_device.c` — asserts the binding tag matches the
  expected binding index on a real handle.
