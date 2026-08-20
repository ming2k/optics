fn main() {
    let mut ui = lens::Ui::headless().unwrap();
    let input = lens::Input::new((100.0, 100.0), 1.0/60.0);
    ui.frame(&input, |f| { f.button("x"); });
    println!("overflowed = {}", ui.overflowed());
}
