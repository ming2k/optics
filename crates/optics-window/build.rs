//! Relay rpaths from `optics-window-sys` and republish for downstream crates.

fn main() {
    optics_build::relay_rpaths(&["DEP_OPTICS_WINDOW_RPATHS"], true);
}
