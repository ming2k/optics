//! Relay rpaths from `optics-gfx-sys` and republish via `links = "optics_gfx"`.

fn main() {
    optics_build::relay_rpaths(&["DEP_OPTICS_GFX_RPATHS"], true);
}
