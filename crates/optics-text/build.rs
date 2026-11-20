//! Relay rpaths for `optics-text` and republish via `links = "optics_text"`.

fn main() {
    optics_build::relay_rpaths(
        &["DEP_OPTICS_TEXT_RPATHS", "DEP_OPTICS_GFX_RS_RPATHS"],
        true,
    );
}
