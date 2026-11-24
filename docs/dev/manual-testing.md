# Manual Testing and Interactive Verification Guide

This document defines manual verification protocols, interactive inspection procedures, and platform-specific test checklists for the Optics monorepo. It complements the automated test suites defined in [Testing](testing.md).

---

## 1. Scope and Verification Model

Optics enforces deterministic, headless testing across the C and Rust codebases for mechanical correctness, mathematical regressions, and memory safety. However, graphics and windowing systems include properties that cannot be verified through headless unit tests alone:

1. **Live Compositor Presentation**: Frame pacing, swapchain resizing, vsync synchronization, and zero-flicker buffer swapping on live display servers.
2. **Visual & Optical Fidelity**: Real-time evaluation of refractive distortion, chromatic aberration, dual-Kawase blur, and procedural noise dithering across diverse GPU hardware and driver implementations.
3. **Interactive Human Input**: Mouse cursor precision, focus navigation, caret positioning, IME composition windows, clipboard synchronization, and cross-application drag-and-drop.
4. **Desktop Shell Integration**: Native window decorations, fractional DPI scaling, system theme transitions (dark/light mode), and accessibility tree emission.

### Testing Surface Responsibility

| Verification Channel | Primary Scope | Execution Mode | Governing Document |
| :--- | :--- | :--- | :--- |
| **Mechanical Unit / Integration** | Math, layout algorithms, CPU tessellation, memory budgets, leak tracking | Automated (`meson test`) | [Testing](testing.md) |
| **Tolerance Goldens** | Raster stability, Lavapipe visual regression gates | Automated (`test_material_golden`) | [Testing](testing.md#golden-references) |
| **Interactive Examples** | Materials, 2D/3D pipelines, font shaping, window shells, drag-and-drop | Manual human inspection | This document |
| **Pre-Release Smoke Matrix** | Clean build, compositor matrix, window lifecycle, OS packaging | Manual release gate | This document & [Release](release.md) |

---

## 2. Environment Preparation and Build

### 2.1 Build Configuration

Manual verification requires compiling the interactive example targets across `gfx`, `material`, `window`, `ui`, `scene`, and `text`:

```bash
# Clean setup with examples, tests, and debugging symbols enabled
meson setup build -Dexamples=true -Dtests=true --buildtype=debugoptimized

# Compile all binaries
meson compile -C build
```

### 2.2 Enabling Vulkan Validation Layers

Run interactive binaries with Vulkan validation layers active to intercept synchronization hazards, image layout mismatches, and descriptor pool leaks during manual operations:

```bash
# Linux / macOS
export VK_LAYER_KHR_VALIDATION=1
./build/examples/showcase/material_gallery

# Windows (PowerShell)
$env:VK_LAYER_KHR_VALIDATION = "1"
.\build\examples\showcase\material_gallery.exe
```

---

## 3. Visual Effects and Material Inspection

The primary visual test bed is the modern material showcase (`examples/showcase/material_gallery`), consolidating `libs/material`, `libs/gfx`, and `libs/ui`.

### 3.1 Execution and Controls

```bash
./build/examples/showcase/material_gallery
```

| Key Binding | Target View / Action | Verification Focus |
| :--- | :--- | :--- |
| `1` | **4-Up Comparison Grid** | View all 4 core materials simultaneously under identical lighting. |
| `2` | **Liquid Glass Fullscreen** | Inspect SDF refraction, chromatic dispersion, and specular rim. |
| `3` | **Frosted Glass Fullscreen** | Inspect dual-Kawase blur smoothness and vibrancy saturation boost. |
| `4` | **Acrylic Fullscreen** | Inspect luminance plate balancing and blue-noise grain dither. |
| `5` | **Mica & Mica Alt Fullscreen**| Inspect screen-anchored sampling and wallpaper blend. |
| `D` | **Toggle Dark / Light** | Verify instant theme plate polarity adaptation without visual popping. |
| `F` | **Toggle Inactive Fallback** | Verify Mica degrades cleanly to an opaque fallback tint when inactive. |
| `Space` | **Pause / Resume Motion** | Freeze background animation to inspect static edge sharpness and dither. |
| `Esc` | **Quit** | Clean teardown without Vulkan validation warnings on stderr. |

### 3.2 Visual Defect Inspection Checklist

When inspecting materials, check for the following visual artifacts:

- [ ] **Liquid Glass Refraction**: Moving background patterns (radial stripes, geometry) bend smoothly around rounded rectangle silhouettes. No pixelated stair-stepping or discontinuous tear lines.
- [ ] **Chromatic Dispersion**: A subtle RGB split appears near high-curvature boundaries. Colors must fringe naturally without severe color clamping or blowout.
- [ ] **Dual-Kawase Blur**: Background remains soft and diffuse; no blocky 4x4 or 8x8 downsampling grid artifacts appear during rapid movement.
- [ ] **Procedural Noise Dither (Acrylic / Mica)**: Noise grain is fine, uniform, and prevents banding in smooth luminance gradients. Dither must not crawl or vibrate when the window is stationary.
- [ ] **Boundary Lighting**: 1px inner SDF borders and specular rim highlights remain razor-sharp under high-DPI scaling factors (1.0x, 1.5x, 2.0x).

---

## 4. 2D Canvas & 3D Core Graphics Verification

Run the core rendering pipelines to verify path tessellation, font shaping, compute barriers, and 3D scene execution.

### 4.1 2D Canvas Pipeline (`examples/getting_started/canvas_window`)

```bash
./build/examples/getting_started/canvas_window
```

**Verification Steps:**
1. Observe the animated ring of 12 coloured circles and the rounded-rectangle pointer proxy on the dark background.
2. Confirm antialiased geometry boundaries are smooth and free of dark fringe halos.
3. Move the pointer: the rounded-rectangle proxy tracks it without visible lag while the circle ring keeps animating.
4. Resize the window repeatedly: verify the canvas viewport and animation adjust dynamically without stretching or aspect-ratio distortion.

### 4.2 3D Lighting & Transform Pipeline (`examples/showcase/scene_studio`)

```bash
./build/examples/showcase/scene_studio
```

**Verification Steps:**
1. Confirm the model auto-orbits continuously with correct depth-buffer occlusion (back faces never draw over front faces).
2. Inspect Phong specular highlights across the model: light reflectance calculates smoothly without vertex-clamping jumps.
3. Press `Space` or use the toolbar button to pause/resume orbit, then drag the **Speed** slider in the inspector: motion responds immediately.
4. Adjust **Pitch**, **FOV**, and **Ambient** in the inspector: the camera and lighting update live without stutter or GPU memory pressure warnings.

### 4.3 glTF Asset Loading (`examples/showcase/scene_studio`)

```bash
./build/examples/showcase/scene_studio
./build/examples/showcase/scene_studio --file path/to/model.glb
```

**Verification Steps:**
1. Verify the bundled `Duck.glb` asset loads, parses, and displays immediately upon startup.
2. Confirm the asset name appears in the floating toolbar badge.
3. Load a custom `.glb` with `--file`; an unreadable path must exit with a clear error instead of a blank window.
4. Rename or hide the bundled asset and relaunch: the viewer falls back to the procedurally generated cube.

### 4.4 Typography and Text Shaping (`examples/showcase/widget_gallery`)

```bash
./build/examples/showcase/widget_gallery
```

**Verification Steps:**
1. Verify shaped text lines with mixed punctuation, Latin, and extended Unicode characters render crisply.
2. Check glyph edge rendering: subpixel positioning and dual-layer R8 atlas blitting must not introduce blurry halos or clipped ascenders/descenders.
3. Toggle display DPI or move between displays: text re-shapes cleanly without font rasterization fuzziness.

### 4.5 Compute Pipeline Synchronization (`examples/getting_started/compute_pass`)

```bash
./build/examples/getting_started/compute_pass
```

**Verification Steps:**
1. Confirm offscreen compute dispatch executes to completion and writes data through synchronization memory barriers.
2. Verify terminal output logs byte-identical confirmation for compute-generated buffers.

---

## 5. UI Widgets, Window Shell, and Interaction Protocols

Verify desktop user interface controls, input handling, window lifecycle, and system capabilities hosted within `libs/window` and `libs/ui`.

### 5.1 Retained UI Controls (`examples/showcase/widget_gallery`)

```bash
./build/examples/showcase/widget_gallery
```

| Control Under Test | Action to Perform | Expected Observable Outcome |
| :--- | :--- | :--- |
| **Push Button** | Hover cursor over a button; press left mouse button; release. | Hover state transitions smoothly; click feedback triggers; text remains centered. |
| **Checkbox** | Click a checkbox on the **Controls** page. | Check mark state flips reliably; hit target covers both label and icon. |
| **Segmented Control** | Switch the density option between the three segments. | The selected segment highlights and only one segment stays active. |
| **Slider** | Drag the **Exposure** or **Contrast** slider thumb across the track. | Numeric value tracks the pointer linearly; thumb does not detach from the cursor. |
| **Sidebar Split Handle** | Drag the vertical divider between the sidebar and the main area. | Panes resize smoothly and the sidebar respects its minimum width. |
| **Tab Strip** | On the **Layout & Data** page, switch tabs and close a closable tab. | Active tab highlight switches cleanly; the last remaining tab cannot be closed. |
| **Tree View** | Expand/collapse a branch and select a leaf. | Disclosure state toggles and exactly one leaf shows the selected highlight. |
| **Data Grid** | Click a sortable header and drag a column divider. | Rows reorder by the chosen column; the column width follows the pointer. |

### 5.2 Text Field, Caret, and Clipboard (`examples/showcase/widget_gallery`)

Verify retained text editing (`ADR-0036`, `ADR-0057`, `ADR-0064`):

1. **Focus & Selection**:
   - Click inside a text field: blinking caret appears at the clicked character index.
   - Click and drag across characters: text selection highlight wraps the selected glyph range accurately.
2. **Keyboard Editing**:
   - Type alphanumeric characters: text inserts at the caret position.
   - Press `Backspace` / `Delete`: characters remove correctly.
   - Press `Left` / `Right` arrows: caret moves one glyph forward/backward.
   - Press `Home` / `End`: caret jumps to the beginning/end of line.
3. **Clipboard Interaction**:
   - Select text and press `Ctrl+C` (or `Cmd+C` on macOS): selection copies to system clipboard.
   - Paste clipboard text into an external text editor (e.g., terminal or VS Code): text matches.
   - Copy text from an external application and press `Ctrl+V`: text pastes into the text field at the caret position.

### 5.3 Overlays, Modals, and Z-Bands (`examples/showcase/widget_gallery`)

```bash
./build/examples/showcase/widget_gallery
```

Select the **Overlays & DnD** page (or press `4`) before running these checks.

**Verification Steps:**
1. Click button to trigger a **Modal Dialog**:
   - A dimmed modal backdrop covers the parent window.
   - Clicks outside the dialog must not interact with underlying controls.
   - Press `Escape` or click the "Close" button: modal dismisses cleanly and restores parent focus.
2. Trigger **Dropdown Menus & Context Flyouts**:
   - Menu opens in the high-priority overlay Z-band (`ADR-0060`).
   - Open a menu near the screen edge: menu repositioning ensures it does not clip outside window boundaries.
   - Click anywhere outside the menu: menu closes immediately.

### 5.4 Cross-Platform Drag and Drop (`examples/showcase/widget_gallery`)

```bash
./build/examples/showcase/widget_gallery
```

**Verification Steps (`ADR-0086`):**
1. **Internal Drag & Drop**:
   - Click and hold the draggable card in the left column.
   - Drag across the window: visual drag proxy tracks cursor motion.
   - Hover over the right drop target: target borders highlight in active hover state.
   - Release mouse: payload drops successfully and status label updates with payload size.
2. **External Drag-Out**:
   - Drag from the window drag source and release into an external text editor or terminal window.
   - Payload string must paste into the destination application.
3. **External Drop-In**:
   - Select text or a file in your file manager / browser, drag it over the window target, and drop.
   - window drop target receives payload data without dropping packets or crashing.

---

## 6. Cross-Platform and Compositor Matrix

Execute manual checks across the target desktop environments:

| Operating System | Compositor / Display Server | Driver Stack | Required Verification Checks |
| :--- | :--- | :--- | :--- |
| **Linux** | **GNOME Shell (Wayland)** | Mesa RADV / ANV | `wp_fractional_scale_v1` scaling, `xdg_shell` resizing, zero-lag window drag. |
| **Linux** | **KDE KWin (Wayland)** | Mesa / NVIDIA 550+ | Server-side decoration (`zxdg_decoration_manager_v1`), dark/light theme D-Bus sync. |
| **Linux** | **Sway / wlroots** | Mesa | Tiling window resize events, absence of SSD decor crashes, keyboard focus. |
| **Linux (Fallback)**| **X11 / Xwayland** | Any Vulkan 1.3 ICD | Fallback window creation, basic event loop pumping. |
| **Windows 10/11** | **DWM** | NVIDIA / AMD / Intel | Win32 message loop, per-monitor DPI v2 scaling, taskbar minimize/restore. |
| **macOS** | **Quartz / WindowServer** | MoltenVK >= 1.3 | Metal swapchain, Cocoa `NSWindow` resize, Retina 2.0x display scaling. |

---

## 7. Pre-Release Smoke Verification Checklist

Before tagging or distributing a release, execute the following smoke protocol from a clean clone:

```bash
# 1. Fresh build configuration
git clean -fdx
meson setup build-release --buildtype=release -Dexamples=true -Dtests=true
meson compile -C build-release

# 2. Automated test baseline
meson test -C build-release --no-suite bench --no-suite fuzz

# 3. Interactive smoke pass
./build-release/examples/showcase/material_gallery   # Run 30s; cycle keys 1-5, D, F
./build-release/examples/showcase/widget_gallery                # Test button, slider, text input, clipboard
./build-release/examples/showcase/scene_studio           # Verify 3D rendering and camera orbit
./build-release/examples/showcase/widget_gallery               # Verify drag-and-drop interaction
```

### Manual Release Acceptance Sign-Off

- [ ] All four smoke applications start within 1 second on a cold launch.
- [ ] No GPU validation layer warnings or memory leak assertions are emitted to stderr.
- [ ] Window resize operations do not produce black frame flashing or tearing.
- [ ] Keyboard navigation, mouse clicks, and clipboard shortcuts function reliably.
- [ ] Application closes cleanly upon clicking the window close button or pressing `Esc`.

---

## 8. Defect Reporting Protocol

When reporting a manual testing defect, include the following environment header:

```text
[Manual Testing Defect Report]
Platform: Linux (Fedora 40) / Windows 11 23H2 / macOS 14.5
Compositor / Window Manager: GNOME 46 (Wayland) / KWin / DWM / Cocoa
GPU & Driver: AMD Radeon RX 6800 XT (RADV 24.1.2) / NVIDIA RTX 4080 (555.58)
Binary Executed: ./build/examples/showcase/material_gallery
Steps to Reproduce:
  1. Launch binary with VK_LAYER_KHR_VALIDATION=1
  2. Press key '2' to enter Liquid Glass Fullscreen
  3. Drag window across monitors with different DPI scale factors
Observed Behavior: [Describe visual artifact, input stall, or crash]
Expected Behavior: [Describe expected correct presentation]
Stderr / Log Trace: [Attach terminal output and validation layer logs]
```

---

## See Also

- [Testing](testing.md) — automated unit, integration, benchmark, and fuzz test suites.
- [Setup](setup.md) — development environment bootstrap and dependencies.
- [Cross-platform](cross-platform.md) — multi-platform build requirements and invariants.
- [Release](release.md) — source release procedure and quality gates.
- [How to run and explore examples](../how-to/run-and-explore-examples.md) — catalog of monorepo examples.
