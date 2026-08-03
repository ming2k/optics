# ADR-0049: Strict DRM identity for Vulkan device selection

- Status: Accepted
- Date: 2026-08-03

## Context

`flux_device_create` scored every Vulkan physical device that satisfied its
feature and extension requirements. That policy is appropriate for a generic
application, but a direct-display compositor already owns a specific DRM/KMS
device. Independent scoring can select a discrete renderer while KMS belongs
to an integrated GPU, silently changing dma-buf import, modifier, explicit
sync, power, and scanout behavior into an unowned cross-GPU path.

Environment variables can influence individual Vulkan implementations, but
they are process-global, vendor-specific policy rather than a library
contract. Validating the selected device after creation also cannot select
the correct device and wastes a complete Vulkan device initialization.

## Decision

Add `flux_device_drm_node_desc` as an optional `flux_device_desc.next`
extension. Its DRM character-device major/minor pair is a strict physical-GPU
constraint. `libs/flux/src/core/device.c` queries
`VK_EXT_physical_device_drm` while enumerating physical devices and accepts a
candidate only when either its primary or render node matches the requested
identity. Normal capability checks and scoring run only across matching
candidates.

If the Vulkan extension is absent or no matching candidate satisfies the
device descriptor, creation returns `FLUX_ERROR_UNSUPPORTED`. Flux never
falls back to a different GPU. The safe Rust binding exposes the same
contract as `DrmNode` and `Device::new_for_drm_node`.

## Alternatives Considered

- **Keep independent best-device scoring.** Rejected because direct-display
  hosts need affinity with an already-selected KMS device, not a second GPU
  policy decision.
- **Validate after `flux_device_create`.** Rejected because validation detects
  the bug after selection and resource creation but cannot correct it.
- **Set loader- or driver-specific environment variables.** Rejected because
  process-global configuration is not a typed, deterministic API contract.
- **Silently permit cross-GPU dma-buf import.** Rejected because Flux does not
  own the copy, modifier negotiation, synchronization, and presentation policy
  required to make that path correct.

## Consequences

- Direct-display hosts can bind Flux to the physical GPU selected for KMS.
- Generic and nested applications retain the existing score-based default by
  omitting the extension.
- Linux hosts requesting strict affinity require
  `VK_EXT_physical_device_drm`; absence is a hard, actionable failure.
- Intentional multi-GPU rendering requires a separately designed copy and
  synchronization path above Flux rather than an implicit selector fallback.

## References

- [Vulkan Backend](../explanation/vulkan-backend.md)
- [Vulkan `VK_EXT_physical_device_drm`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_physical_device_drm.html)
