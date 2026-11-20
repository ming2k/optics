//! Relay rpaths published by enabled Optics crates for `optics` test and example binaries.

fn main() {
    optics_build::relay_rpaths(
        &[
            "DEP_OPTICS_APP_RS_RPATHS",
            "DEP_OPTICS_WINDOW_RS_RPATHS",
            "DEP_OPTICS_UI_RS_RPATHS",
            "DEP_OPTICS_MATERIAL_RS_RPATHS",
            "DEP_OPTICS_SVG_RS_RPATHS",
            "DEP_OPTICS_SCENE_RS_RPATHS",
            "DEP_OPTICS_TEXT_RS_RPATHS",
            "DEP_OPTICS_GFX_RS_RPATHS",
        ],
        false,
    );
}
