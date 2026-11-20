//! Relay rpaths from `optics-window-sys` and republish via `links = "optics_window"`.

fn main() {
    optics_build::relay_rpaths(&["DEP_OPTICS_WINDOW_RPATHS"], true);
}
