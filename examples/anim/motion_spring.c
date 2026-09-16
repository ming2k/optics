/*
 * motion_spring.c — canonical starter for the anim motion vocabulary (ADR-0077).
 *
 * Demonstrates closed-form spring advance, non-divergence over large dt,
 * and adaptive smoothing without clock, timeline, or heap allocation.
 */

#include <anim/anim.h>
#include <stdio.h>

static void print_bar(const char *label, float value, float max_val) {
    int width = 36;
    int filled = (int)((value / max_val) * (float)width);
    if (filled < 0)
        filled = 0;
    if (filled > width)
        filled = width;

    printf("%-20s [%6.2f] |", label, value);
    for (int i = 0; i < width; ++i)
        putchar(i < filled ? '=' : ' ');
    printf("|\n");
}

int main(void) {
    printf("=== Optics anim motion vocabulary starter ===\n\n");

    /* 1. Closed-form analytic spring */
    anim_spring spring = anim_spring_at(0.0f);
    anim_spring_params params = anim_spring_snappy();
    float target = 100.0f;

    printf("1. Simulating snappy spring advance (0.0 -> 100.0):\n");
    float dt = 1.0f / 60.0f; /* 60 FPS step */
    for (int step = 0; step <= 20; ++step) {
        anim_spring_advance(&spring, target, params, dt, false);
        if (step % 2 == 0) {
            char tag[32];
            snprintf(tag, sizeof(tag), "Step %2d (t=%.2fs)", step, (float)step * dt);
            print_bar(tag, spring.value, 110.0f);
        }
    }

    /* 2. Resilience under massive frame stall (dt = 0.50s) */
    printf("\n2. Resilience test: massive frame stall (dt = 0.50s):\n");
    printf("   Traditional Euler integrators diverge to infinity on large dt.\n");
    printf("   Anim uses closed-form analytic solutions that never diverge:\n");
    spring = anim_spring_at(0.0f);
    anim_spring_advance(&spring, target, params, 0.50f, false);
    print_bar("After 0.5s stall", spring.value, 110.0f);

    /* 3. Motion-adaptive smoother for noisy/lagged telemetry */
    printf("\n3. Motion-adaptive smoother (de-jittering noisy signals):\n");
    anim_smoother smoother = anim_smoother_init(80.0f);
    for (int step = 0; step < 6; ++step) {
        float noisy_input = 80.0f + (float)(step % 2 == 0 ? 8 : -8);
        anim_smoother_step(&smoother, noisy_input, 0.10f, 2.5f, 0.5f, dt);
        char tag[32];
        snprintf(tag, sizeof(tag), "Raw %4.1f -> Smooth", noisy_input);
        print_bar(tag, smoother.value, 100.0f);
    }

    printf("\nStarter complete: provable math, zero allocation, caller-owned state.\n");
    return 0;
}
