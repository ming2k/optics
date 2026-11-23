//! Relay rpaths for `optics-text` and republish for downstream crates.

fn main() {
    optics_build::relay_rpaths(
        &["DEP_OPTICS_TEXT_RPATHS", "DEP_OPTICS_GFX_RS_RPATHS"],
        true,
    );
}
