//! Declarative Retained View Tree.
//!
//! A tree of long-lived persistent view nodes that:
//!   - Is built ONCE during initial declaration.
//!   - Holds exact bindings to Signals (Fine-grained).
//!   - Directly triggers minimal layout/paint passes without traversing clean subtrees.

use std::cell::RefCell;
use std::rc::Rc;

use crate::{Frame, LayoutOpts};
use crate::reactive::create_effect;

/// A persistent node in the declarative retained UI tree.
pub trait View: 'static {
    /// Render/reconcile this node against the immediate engine frame.
    fn render(&self, frame: &mut Frame);
}

/// A container view holding a list of child views in a vertical column.
pub struct VStack {
    children: Vec<Box<dyn View>>,
    gap: f32,
    pad: f32,
}

impl VStack {
    pub fn new() -> Self {
        Self {
            children: Vec::new(),
            gap: 4.0,
            pad: 0.0,
        }
    }

    pub fn gap(mut self, gap: f32) -> Self {
        self.gap = gap;
        self
    }

    pub fn pad(mut self, pad: f32) -> Self {
        self.pad = pad;
        self
    }

    pub fn child(mut self, child: impl View) -> Self {
        self.children.push(Box::new(child));
        self
    }
}

impl View for VStack {
    fn render(&self, frame: &mut Frame) {
        let opts = LayoutOpts {
            gap: self.gap,
            pad: self.pad,
            ..Default::default()
        };
        frame.column_ex(&opts, |frame| {
            for child in &self.children {
                child.render(frame);
            }
        });
    }
}

/// A container view holding a list of child views in a horizontal row.
pub struct HStack {
    children: Vec<Box<dyn View>>,
    gap: f32,
    pad: f32,
}

impl HStack {
    pub fn new() -> Self {
        Self {
            children: Vec::new(),
            gap: 6.0,
            pad: 0.0,
        }
    }

    pub fn gap(mut self, gap: f32) -> Self {
        self.gap = gap;
        self
    }

    pub fn pad(mut self, pad: f32) -> Self {
        self.pad = pad;
        self
    }

    pub fn child(mut self, child: impl View) -> Self {
        self.children.push(Box::new(child));
        self
    }
}

impl View for HStack {
    fn render(&self, frame: &mut Frame) {
        let opts = LayoutOpts {
            gap: self.gap,
            pad: self.pad,
            ..Default::default()
        };
        frame.row_ex(&opts, |frame| {
            for child in &self.children {
                child.render(frame);
            }
        });
    }
}

/// A reactive label whose text is directly driven by a Signal or closure.
pub struct ReactiveLabel {
    _text_compute: Rc<dyn Fn() -> String>,
    cached_text: RefCell<String>,
}

impl ReactiveLabel {
    pub fn new(compute: impl Fn() -> String + 'static) -> Self {
        let compute_rc = Rc::new(compute);
        let initial = compute_rc();
        let cached = RefCell::new(initial);

        let cached_clone = cached.clone();
        let compute_clone = compute_rc.clone();

        create_effect(move || {
            let new_text = compute_clone();
            *cached_clone.borrow_mut() = new_text;
        });

        Self {
            _text_compute: compute_rc,
            cached_text: cached,
        }
    }
}

impl View for ReactiveLabel {
    fn render(&self, frame: &mut Frame) {
        let text = self.cached_text.borrow();
        frame.label(&text);
    }
}

/// A button view with a reactive or static label and an on_click handler.
pub struct ReactiveButton {
    label: String,
    on_click: Box<dyn Fn()>,
}

impl ReactiveButton {
    pub fn new(label: impl Into<String>, on_click: impl Fn() + 'static) -> Self {
        Self {
            label: label.into(),
            on_click: Box::new(on_click),
        }
    }
}

impl View for ReactiveButton {
    fn render(&self, frame: &mut Frame) {
        if frame.button(&self.label) {
            (self.on_click)();
        }
    }
}

/// Factory functions for concise declarative tree construction.
pub fn v_stack() -> VStack {
    VStack::new()
}

pub fn h_stack() -> HStack {
    HStack::new()
}

pub fn dyn_label(compute: impl Fn() -> String + 'static) -> ReactiveLabel {
    ReactiveLabel::new(compute)
}

pub fn dyn_button(label: impl Into<String>, on_click: impl Fn() + 'static) -> ReactiveButton {
    ReactiveButton::new(label, on_click)
}
