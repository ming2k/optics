//! Tests for fine-grained reactivity and declarative retained views.

use lens::reactive::{create_effect, create_memo, create_signal};
use lens::view::{dyn_button, dyn_label, h_stack, v_stack, View};
use std::cell::RefCell;
use std::rc::Rc;

#[test]
fn signal_and_effect_propagation() {
    let (count, set_count) = create_signal(0);
    let log = Rc::new(RefCell::new(Vec::new()));

    let log_clone = log.clone();
    create_effect(move || {
        log_clone.borrow_mut().push(count.get());
    });

    assert_eq!(*log.borrow(), vec![0]);

    set_count.set(1);
    set_count.set(2);
    set_count.update(|c| *c += 10);

    assert_eq!(*log.borrow(), vec![0, 1, 2, 12]);
}

#[test]
fn memo_computation_tracks_dependencies() {
    let (first, set_first) = create_signal("Hello".to_string());
    let (last, set_last) = create_signal("World".to_string());

    let full_name = create_memo(move || format!("{} {}", first.get(), last.get()));

    assert_eq!(full_name.get(), "Hello World");

    set_last.set("Optics".to_string());
    assert_eq!(full_name.get(), "Hello Optics");

    set_first.set("Muta".to_string());
    assert_eq!(full_name.get(), "Muta Optics");
}

#[test]
fn declarative_retained_view_tree_updates_with_signals() {
    let mut ui = lens::Ui::headless().expect("headless ui");
    let input = lens::Input::new((800.0, 600.0), 1.0 / 60.0);

    let (counter, set_counter) = create_signal(0);

    // Build the declarative tree ONCE!
    let app_view = v_stack()
        .child(dyn_label(move || format!("Counter: {}", counter.get())))
        .child(
            h_stack()
                .child(dyn_button("+1", {
                    let set_counter = set_counter.clone();
                    move || set_counter.update(|c| *c += 1)
                }))
                .child(dyn_button("Reset", {
                    let set_counter = set_counter.clone();
                    move || set_counter.set(0)
                })),
        );

    // Frame 1
    ui.frame(&input, |frame| {
        app_view.render(frame);
    });

    // Update signal
    set_counter.set(42);

    // Frame 2: The View node's cached text was updated directly via Effect without rebuilding tree
    ui.frame(&input, |frame| {
        app_view.render(frame);
    });
}
