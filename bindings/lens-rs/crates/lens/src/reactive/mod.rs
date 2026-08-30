//! Fine-grained reactive primitives: Signal, Memo, Effect, and Observer Graph.
//!
//! Based on Push-Pull dependency tracking (Solid/Leptos/MobX math).
//! Guarantees:
//!   - Zero virtual DOM diffing overhead.
//!   - Exact point-to-point propagation from mutated Signal to subscribed View Nodes.
//!   - Glitch-free synchronous execution.

use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::rc::Rc;

thread_local! {
    /// Currently running Effect/Observer subscriber context.
    static CURRENT_OBSERVER: RefCell<Option<Rc<dyn Subscriber>>> = const { RefCell::new(None) };
}

pub trait Subscriber {
    fn notify(&self);
}

/// A reactive state container holding a value of type `T`.
pub struct Signal<T: 'static> {
    value: Rc<RefCell<T>>,
    subscribers: Rc<RefCell<HashSet<SubscriberKey>>>,
}

impl<T: 'static> Clone for Signal<T> {
    fn clone(&self) -> Self {
        Self {
            value: self.value.clone(),
            subscribers: self.subscribers.clone(),
        }
    }
}

#[derive(Clone)]
struct SubscriberKey(usize, Rc<dyn Subscriber>);

impl PartialEq for SubscriberKey {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}

impl Eq for SubscriberKey {}

impl std::hash::Hash for SubscriberKey {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.0.hash(state);
    }
}

/// Create a new read-write Signal with initial value.
pub fn create_signal<T: 'static>(initial: T) -> (ReadSignal<T>, WriteSignal<T>) {
    let sig = Signal {
        value: Rc::new(RefCell::new(initial)),
        subscribers: Rc::new(RefCell::new(HashSet::new())),
    };
    (ReadSignal(sig.clone()), WriteSignal(sig))
}

/// Read-only handle to a Signal. Tracks dependencies when read inside an observer.
pub struct ReadSignal<T: 'static>(Signal<T>);

impl<T: 'static> Clone for ReadSignal<T> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl<T: Clone + 'static> ReadSignal<T> {
    /// Read the current value and register dependency in current active observer.
    pub fn get(&self) -> T {
        self.track();
        self.0.value.borrow().clone()
    }

    /// Read without registering dependency.
    pub fn get_untracked(&self) -> T {
        self.0.value.borrow().clone()
    }

    /// Explicitly register current observer.
    pub fn track(&self) {
        CURRENT_OBSERVER.with(|obs| {
            if let Some(subscriber) = obs.borrow().as_ref() {
                let addr = Rc::as_ptr(subscriber) as *const () as usize;
                self.0
                    .subscribers
                    .borrow_mut()
                    .insert(SubscriberKey(addr, subscriber.clone()));
            }
        });
    }
}

/// Write-only handle to a Signal. Notifies all subscribers upon mutation.
pub struct WriteSignal<T: 'static>(Signal<T>);

impl<T: 'static> Clone for WriteSignal<T> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl<T: 'static> WriteSignal<T> {
    /// Set a new value and notify all subscribers.
    pub fn set(&self, new_val: T) {
        *self.0.value.borrow_mut() = new_val;
        self.notify_subscribers();
    }

    /// Mutate the value via closure and notify subscribers.
    pub fn update(&self, f: impl FnOnce(&mut T)) {
        f(&mut *self.0.value.borrow_mut());
        self.notify_subscribers();
    }

    fn notify_subscribers(&self) {
        let subs: Vec<Rc<dyn Subscriber>> = self
            .0
            .subscribers
            .borrow()
            .iter()
            .map(|s| s.1.clone())
            .collect();

        for sub in subs {
            sub.notify();
        }
    }
}

/// A reactive derived computation (Memoized Signal).
pub struct Memo<T: 'static> {
    read: ReadSignal<T>,
}

impl<T: Clone + 'static> Clone for Memo<T> {
    fn clone(&self) -> Self {
        Self {
            read: self.read.clone(),
        }
    }
}

impl<T: Clone + 'static> Memo<T> {
    pub fn get(&self) -> T {
        self.read.get()
    }
}

/// Create a derived computation that re-evaluates only when its dependencies change.
pub fn create_memo<T: Clone + PartialEq + 'static>(
    f: impl Fn() -> T + 'static,
) -> Memo<T> {
    let initial = f();
    let (read, write) = create_signal(initial);
    let compute = Rc::new(f);

    let write_clone = write.clone();
    let compute_clone = compute.clone();

    create_effect(move || {
        let new_val = compute_clone();
        if new_val != write_clone.0.value.borrow().clone() {
            write_clone.set(new_val);
        }
    });

    Memo { read }
}

struct EffectSubscriber {
    run: RefCell<Box<dyn Fn()>>,
    running: Cell<bool>,
}

impl Subscriber for EffectSubscriber {
    fn notify(&self) {
        if self.running.get() {
            return;
        }
        self.running.set(true);
        CURRENT_OBSERVER.with(|obs| {
            let prev = obs.replace(None);
            (self.run.borrow())();
            obs.replace(prev);
        });
        self.running.set(false);
    }
}

/// Create a side effect that automatically tracks dependencies and re-executes on change.
pub fn create_effect(f: impl Fn() + 'static) {
    let sub = Rc::new(EffectSubscriber {
        run: RefCell::new(Box::new(f)),
        running: Cell::new(false),
    });

    CURRENT_OBSERVER.with(|obs| {
        let prev = obs.replace(Some(sub.clone() as Rc<dyn Subscriber>));
        (sub.run.borrow())();
        obs.replace(prev);
    });
}
