//! Relay rpaths for `optics-material` and republish via `links = "optics_material"`.

fn main() {
    optics_build::relay_rpaths(
        &["DEP_OPTICS_MATERIAL_RPATHS", "DEP_OPTICS_GFX_RPATHS"],
        true,
    );
}
