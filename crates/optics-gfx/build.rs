//! Relay rpaths from `optics-gfx-sys` and republish for downstream crates.

fn main() {
    optics_build::relay_rpaths(&["DEP_OPTICS_GFX_RPATHS"], true);
}
