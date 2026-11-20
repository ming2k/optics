//! Relay rpaths for `optics-ui` and republish via `links = "optics_ui"`.

fn main() {
    optics_build::relay_rpaths(&["DEP_OPTICS_UI_RPATHS", "DEP_OPTICS_GFX_RS_RPATHS"], true);
}
