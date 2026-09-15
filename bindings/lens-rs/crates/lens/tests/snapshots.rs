use lens::{Input, Ui};

#[test]
fn snapshots_outlive_ui_and_validate_presentation_identity() {
    let input = Input::new((200.0, 100.0), 0.016);
    let mut ui = Ui::headless().unwrap();
    assert!(ui.snapshot().is_err());
    ui.frame(&input, |frame| frame.label("first"));
    let first = ui.snapshot().unwrap();
    ui.activate(&first).unwrap();
    ui.frame(&input, |frame| frame.label("second"));
    let second = ui.snapshot().unwrap();
    assert!(second.generation() > first.generation());
    ui.activate(&second).unwrap();
    assert!(ui.activate(&first).is_err());
    drop(ui);
    assert!(first.generation() > 0);
    let mut foreign = Ui::headless().unwrap();
    foreign.frame(&input, |frame| frame.label("foreign"));
    assert!(foreign.activate(&first).is_err());
}
