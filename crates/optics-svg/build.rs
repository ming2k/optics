//! Relay rpaths for `optics-svg` and republish for downstream crates.

fn main() {
    optics_build::relay_rpaths(&["DEP_OPTICS_SVG_RPATHS", "DEP_OPTICS_GFX_RS_RPATHS"], true);
}
