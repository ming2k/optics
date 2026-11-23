//! Relay rpaths from `optics-app-sys` and republish for downstream crates.

fn main() {
    optics_build::relay_rpaths(
        &[
            "DEP_OPTICS_APP_RPATHS",
            "DEP_OPTICS_WINDOW_RS_RPATHS",
            "DEP_OPTICS_UI_RS_RPATHS",
            "DEP_OPTICS_GFX_RS_RPATHS",
        ],
        true,
    );
}
