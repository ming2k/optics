/*
 * Differential parity for the SDF derivative (ADR-0148, [INV-SDF-02]).
 *
 * `[INV-SDF-02]` says a distance field is defined once, together with its
 * derivative, and that "any other evaluation is a projection of that definition
 * and is pinned by a differential test". Before this test the device side had
 * no such pin: `test_sdf` compared the host implementation against a *hand
 * transcribed* reference of the shader expression, which cannot detect a change
 * to `flux_sdf.glsl` at all. The pin was described in prose and enforced by
 * nothing.
 *
 * This test dispatches the real `flux_sdf.glsl` — the same header the shipped
 * materials include — and compares its distance and normal against
 * `<flux/sdf.h>` sample by sample. Drift in either direction fails here:
 *
 *   - edit the GLSL field or edge rule   -> GPU moves, host does not
 *   - edit the host field or gradient    -> host moves, GPU does not
 *   - edit both inconsistently           -> both move, disagreement remains
 *
 * Tolerance is float32 arithmetic-order noise, not a fudge factor: the two
 * implementations evaluate the same expression in a different order, so the
 * budget is a few ulp at these magnitudes, asserted as an absolute epsilon
 * well below a 1/255 pixel step.
 */
#include "test_helpers.h"
#include <flux/compute.h>
#include <flux/flux.h>
#include <flux/sdf.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_embed) && !defined(FLUX_SHADER_NO_EMBED)
#if __has_embed("sdf_parity.comp.spv")
#define TEST_SHADERS_EMBED 1
#endif
#endif

#ifdef TEST_SHADERS_EMBED
alignas(uint32_t) static const unsigned char parity_spv[] = {
#embed "sdf_parity.comp.spv"
};
#else
#include "sdf_parity_comp_spv.h"
#endif

#define SAMPLE_COUNT 4096u
#define WORKGROUP 64u
#define FLOATS_PER_SAMPLE 4u
#define BYTES (SAMPLE_COUNT * FLOATS_PER_SAMPLE * sizeof(float))

typedef struct push_constants {
    uint64_t out_address;
    float half_x;
    float half_y;
    float radius;
    float curvature;
    float aa;
    uint32_t sample_count;
    uint32_t mode; /* 0 = shading normal, 1 = continuum gradient */
    float pad1;
    float pad2;
} push_constants;

/* Mirrors the lattice sdf_parity.comp builds, so the host evaluates the same
 * points. Kept here rather than in the shader's order-independent form because
 * the test owns the sampling policy; the field is what is under test. */
static flux_point lattice_point(uint32_t i, float half_x, float half_y, float *out_extent) {
    uint32_t cols = (uint32_t)ceilf(sqrtf((float)SAMPLE_COUNT));
    uint32_t rows = (SAMPLE_COUNT + cols - 1u) / cols;
    float u = (rows <= 1u) ? 0.5f : ((float)(i / cols) / (float)(rows - 1u));
    float v = (cols <= 1u) ? 0.5f : ((float)(i % cols) / (float)(cols - 1u));
    float extent = fmaxf(half_x, half_y);
    *out_extent = extent;
    return (flux_point){(v * 2.0f - 1.0f) * extent * 2.2f, (u * 2.0f - 1.0f) * extent * 2.2f};
}

typedef struct case_desc {
    const char *name;
    float half_x;
    float half_y;
    float radius;
    float curvature;
    float aa;
} case_desc;

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_sdf_parity: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    VkDevice vk = flux_device_vk_device(d);
    VkQueue queue = flux_device_vk_graphics_queue(d);
    uint32_t qfam = flux_device_vk_graphics_family(d);

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BYTES,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        EXPECT(vkCreateBuffer(vk, &bci, nullptr, &buffer) == VK_SUCCESS);

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk, buffer, &mr);
        uint32_t mt = test_helpers_find_memory_type(d, mr.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        EXPECT(mt != UINT32_MAX);
        VkMemoryAllocateFlagsInfo flags = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &flags,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
        };
        EXPECT(vkAllocateMemory(vk, &mai, nullptr, &memory) == VK_SUCCESS);
        EXPECT(vkBindBufferMemory(vk, buffer, memory, 0) == VK_SUCCESS);

        VkBufferDeviceAddressInfo bdai = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer,
        };
        (void)bdai;
        EXPECT(vkMapMemory(vk, memory, 0, BYTES, 0, &mapped) == VK_SUCCESS);
    }

    VkBufferDeviceAddressInfo bdai = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    const uint64_t buffer_address = vkGetBufferDeviceAddress(vk, &bdai);
    EXPECT(buffer_address != 0);

    flux_compute_pipeline *pipeline = nullptr;
    {
        flux_compute_pipeline_desc desc = {
            .type = FLUX_TYPE_COMPUTE_PIPELINE_DESC,
            .spirv = (const uint32_t *)parity_spv,
            .spirv_word_count = sizeof(parity_spv) / sizeof(uint32_t),
            .push_constant_bytes = sizeof(push_constants),
        };
        flux_result r = flux_compute_pipeline_create(d, &desc, &pipeline);
        EXPECT(r == FLUX_OK);
        if (r != FLUX_OK) {
            flux_error_info info = {0};
            flux_get_last_error(&info);
            fprintf(stderr, "test_sdf_parity: pipeline failed: %s (%s:%d)\n",
                    info.message ? info.message : "(no detail)", info.file ? info.file : "?",
                    info.line);
        }
    }

    /* A sweep rather than one shape: a box, a circle-ish rrect, a G2 squircle,
     * a wide pill, and a degenerate half extent. Between them they exercise the
     * L2 branch, the L4 branch, the blend, and the interior crease. */
    const case_desc cases[] = {
        {"box", 40.0f, 24.0f, 0.0f, 0.0f, 1.0f},
        {"rrect", 32.0f, 32.0f, 12.0f, 0.0f, 1.0f},
        {"squircle_g2", 36.0f, 28.0f, 10.0f, 1.0f, 1.0f},
        {"pill", 60.0f, 8.0f, 8.0f, 0.5f, 1.0f},
        {"tiny", 4.0f, 3.0f, 1.0f, 0.75f, 1.0f},
        {"subpixel_aa", 20.0f, 20.0f, 6.0f, 1.0f, 1e-3f},
    };
    const size_t case_count = sizeof(cases) / sizeof(cases[0]);

    for (size_t c = 0; c < case_count; ++c) {
        const case_desc *cs = &cases[c];

        /* Both public entry points on both sides. `mode 0` is the shading
         * normal the shipped material uses; `mode 1` is the continuum
         * gradient, which no shader calls yet but which [INV-SDF-02]
         * requires to exist and agree, so leaving it unverified would make
         * the invariant a claim again. */
        for (uint32_t mode = 0; mode < 3u; ++mode) {
            memset(mapped, 0, BYTES);

            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            {
                VkCommandPoolCreateInfo pci = {
                    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                    .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                    .queueFamilyIndex = qfam,
                };
                EXPECT(vkCreateCommandPool(vk, &pci, nullptr, &pool) == VK_SUCCESS);
                VkCommandBufferAllocateInfo cbai = {
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                    .commandPool = pool,
                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                    .commandBufferCount = 1,
                };
                EXPECT(vkAllocateCommandBuffers(vk, &cbai, &cmd) == VK_SUCCESS);
                VkCommandBufferBeginInfo bi = {
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                EXPECT(vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS);
            }

            push_constants pc = {
                .out_address = buffer_address,
                .half_x = cs->half_x,
                .half_y = cs->half_y,
                .radius = cs->radius,
                .curvature = cs->curvature,
                .aa = cs->aa,
                .sample_count = SAMPLE_COUNT,
                .mode = mode,
            };
            flux_compute_dispatch(cmd, pipeline, &pc, sizeof(pc), SAMPLE_COUNT / WORKGROUP, 1, 1);

            /* Make the dispatch's stores visible to the host before it reads the
             * mapping. Host coherence makes the *view* consistent; it does not
             * order the GPU's writes before a host read. Omitting this produced
             * an occasional all-zero page (reproducible under MALLOC_PERTURB_),
             * which is a harness race, not a field disagreement. */
            {
                VkMemoryBarrier2 mb = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                    .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                };
                VkDependencyInfo di = {
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1,
                    .pMemoryBarriers = &mb,
                };
                vkCmdPipelineBarrier2(cmd, &di);
            }

            EXPECT(vkEndCommandBuffer(cmd) == VK_SUCCESS);
            {
                VkSubmitInfo si = {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &cmd,
                };
                EXPECT(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
                EXPECT(vkQueueWaitIdle(queue) == VK_SUCCESS);
            }
            vkDestroyCommandPool(vk, pool, nullptr);

            const float *gpu = mapped;
            double worst_d = 0.0, worst_n = 0.0;
            uint32_t compared = 0;
            uint32_t skipped_discontinuity = 0;

            /* Tolerances are DERIVED, not tuned to whatever the code happened to
             * produce. Both sides evaluate one expression in different orders, so
             * the disagreement is float32 arithmetic noise; what varies is how
             * much the expression amplifies it.
             *
             * The interior crease ramp is `h = 0.5 + (q.x-q.y)/(2*aa)`, so one unit
             * of normal is worth `2*aa` units of coordinate, and coordinate noise
             * of order `eps*extent` lands in the normal amplified by
             * `eps*extent/(2*aa)`. With a realistic `aa` (one unit per pixel) that
             * term is ~1e-5 and the floor dominates — the ramp is well conditioned.
             * With a deliberately tiny `aa` the ramp approaches a step and the term
             * grows: at `aa = 1e-3` over a 20-unit shape it is ~9.5e-3, which is the
             * bound this assertion uses. Measured drift on that case is 6.75e-3,
             * inside the bound.
             *
             * Stating it this way keeps the assertion meaningful: it fails on a real
             * disagreement, and it does not fail merely because a stress case is
             * ill-conditioned at float32. */
            const float eps = 1.19209290e-7f; /* FLT_EPSILON */
            double shape_extent = fmax((double)cs->half_x, (double)cs->half_y);
            double noise = 8.0 * (double)eps * shape_extent;
            /* Base floor: the measured worst disagreement across the well-conditioned
             * cases is ~1.4e-5, so 1e-4 is 7x headroom — enough that a legitimate
             * reordering cannot trip it, and tight enough to catch a real drift.
             * That is not a claim; it is verified: perturbing the host's L4 gradient
             * by 0.1% moves the worst disagreement to 3.5e-4 and fails here. (An
             * earlier 2e-3 floor did not, which is why it is not 2e-3.) */
            double normal_tol = 1e-4;
            if (cs->aa > 0.0f) {
                double amplified = noise / (2.0 * (double)cs->aa);
                if (amplified > normal_tol)
                    normal_tol = amplified;
            }

            for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) {
                float extent = 0.0f;
                flux_point p = lattice_point(i, cs->half_x, cs->half_y, &extent);

                /* The continuum gradient is DISCONTINUOUS across the interior
                 * medial axis by definition — `flux_sdf_squircle_gradient` is the
                 * hard pick of the dominant edge, and the header says it flips by
                 * 90 degrees across zero width. On a square that axis is the exact
                 * diagonal, and the two sides can land on opposite sides of the
                 * strict tie-break purely from float rounding in `p`. Demanding
                 * agreement there would be demanding agreement at a discontinuity.
                 *
                 * Skip a hairline band around it and count what was skipped, so the
                 * exclusion is visible in the output rather than silent. Everywhere
                 * else the comparison is exact in intent. */
                if (mode == 1u) {
                    float qx = fabsf(p.x) - (cs->half_x - cs->radius);
                    float qy = fabsf(p.y) - (cs->half_y - cs->radius);
                    if (qx <= 0.0f && qy <= 0.0f && fabsf(qx - qy) < 1e-3f) {
                        skipped_discontinuity++;
                        continue;
                    }
                }

                /* Compare like with like. mode 1 is the continuum gradient on both
                 * sides; comparing the GPU's gradient against the host's *shading*
                 * normal would report the interior crease rule as a defect, which
                 * is the one difference the two entry points exist to express. */
                flux_sdf_sample host;
                if (mode == 2u) {
                    /* The fused-body composition: two shapes and their smooth
                     * union, which is the only path a metaball merge takes and
                     * the one a differenced normal had to guess at. */
                    flux_point p_b = {p.x - cs->half_x * 0.75f, p.y};
                    flux_sdf_sample a_s = flux_sdf_squircle_sample(
                        p, cs->half_x, cs->half_y, cs->radius * 0.25f, cs->curvature, cs->aa);
                    flux_sdf_sample b_s =
                        flux_sdf_squircle_sample(p_b, cs->half_x * 0.7f, cs->half_y * 0.7f,
                                                 cs->radius * 0.25f, cs->curvature, cs->aa);
                    host.distance = flux_sdf_smooth_union(a_s.distance, b_s.distance, cs->radius);
                    host.normal = flux_sdf_smooth_union_gradient(
                        a_s.normal, b_s.normal, a_s.distance, b_s.distance, cs->radius);
                } else if (mode == 1u) {
                    host = flux_sdf_squircle_gradient(p, cs->half_x, cs->half_y, cs->radius,
                                                      cs->curvature);
                } else {
                    host = flux_sdf_squircle_sample(p, cs->half_x, cs->half_y, cs->radius,
                                                    cs->curvature, cs->aa);
                }
                const float *g = &gpu[i * FLOATS_PER_SAMPLE];
                float gpu_d = g[0], gpu_nx = g[1], gpu_ny = g[2];

                if (!isfinite(host.distance) || !isfinite(host.normal.x) ||
                    !isfinite(host.normal.y)) {
                    fprintf(stderr, "test_sdf_parity[%s]: host non-finite at sample %u\n", cs->name,
                            i);
                    EXPECT(false);
                    continue;
                }

                double dn = fabs((double)host.distance - (double)gpu_d);
                double dnx = fabs((double)host.normal.x - (double)gpu_nx);
                double dny = fabs((double)host.normal.y - (double)gpu_ny);
                double nd = fmax(dnx, dny);
                if (dn > worst_d)
                    worst_d = dn;
                if (nd > worst_n)
                    worst_n = nd;
                compared++;

                /* Distance is in shape units; 1e-3 of a unit is far below one
                 * 1/255 pixel step at any sane scale, and above float32 noise at
                 * these magnitudes. */
                if (dn > 1e-3) {
                    fprintf(stderr,
                            "test_sdf_parity[%s/%u]: distance drift at %u: host=%.7f gpu=%.7f\n",
                            cs->name, mode, i, (double)host.distance, (double)gpu_d);
                    EXPECT(false);
                }
                /* Unit vectors: normal_tol is derived above from the crease ramp's
                 * conditioning, so this bounds arithmetic order noise rather than
                 * visible shading error. */
                if (nd > normal_tol) {
                    fprintf(stderr,
                            "test_sdf_parity[%s/%u]: normal drift at %u: host=(%.7f,%.7f) "
                            "gpu=(%.7f,%.7f) tol=%.2e\n",
                            cs->name, mode, i, (double)host.normal.x, (double)host.normal.y,
                            (double)gpu_nx, (double)gpu_ny, normal_tol);
                    EXPECT(false);
                }
            }

            /* Every sample must be either compared or explicitly excused. A run
             * that skipped everything would otherwise pass vacuously. */
            EXPECT(compared > 0u);
            EXPECT(compared + skipped_discontinuity == SAMPLE_COUNT);
            fprintf(stderr,
                    "test_sdf_parity[%s/%u]: %u compared (%u at the crease, skipped), "
                    "max|dd|=%.3e max|dn|=%.3e\n",
                    cs->name, mode, compared, skipped_discontinuity, worst_d, worst_n);
        }
    }

    flux_compute_pipeline_release(pipeline);
    vkUnmapMemory(vk, memory);
    vkDestroyBuffer(vk, buffer, nullptr);
    vkFreeMemory(vk, memory, nullptr);
    flux_device_release(d);

    TEST_SUMMARY();
    return 0;
}
