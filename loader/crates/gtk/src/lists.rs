// SPDX-License-Identifier: GPL-3.0-or-later
//! Virtualised lists over the core's windowed data sources.
//!
//! [`WindowModel`] is a `GListModel` whose item count is the source's total;
//! an item is just its position, and the row content is read from the core's
//! [`ListWindow`] when GTK binds it (a placeholder until the block arrives).
//! Splices from the core become exact `items-changed` calls, so a refreshing
//! server list updates rows in place and the view keeps its scroll anchor.
//! GtkColumnView (tables) and GtkGridView (cards) recycle their row widgets;
//! a card's picture is requested when it is bound and dropped when unbound.

use gtk::glib;
use gtk::prelude::*;
use gtk::subclass::prelude::*;

mod imp {
    use super::*;
    use gtk::gio;
    use std::cell::Cell;

    #[derive(Default)]
    pub struct WindowModel {
        pub count: Cell<u32>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for WindowModel {
        const NAME: &'static str = "GnWindowModel";
        type Type = super::WindowModel;
        type Interfaces = (gio::ListModel,);
    }

    impl ObjectImpl for WindowModel {}

    impl ListModelImpl for WindowModel {
        fn item_type(&self) -> glib::Type {
            glib::BoxedAnyObject::static_type()
        }
        fn n_items(&self) -> u32 {
            self.count.get()
        }
        fn item(&self, position: u32) -> Option<glib::Object> {
            (position < self.count.get()).then(|| glib::BoxedAnyObject::new(position as usize).upcast())
        }
    }
}

glib::wrapper! {
    pub struct WindowModel(ObjectSubclass<imp::WindowModel>) @implements gtk::gio::ListModel;
}

impl Default for WindowModel {
    fn default() -> Self {
        glib::Object::new()
    }
}

impl WindowModel {
    /// Applies a splice: the count changes first, then the view is told exactly what moved.
    pub fn splice(&self, s: &godnotic_core::bridge::Splice) {
        let imp = self.imp();
        let c = imp.count.get() as usize;
        let new = c - s.removed.min(c.saturating_sub(s.position)) + s.added;
        imp.count.set(new as u32);
        self.items_changed(s.position as u32, s.removed as u32, s.added as u32);
    }

    /// Every row changed (narrow/wide switch, re-sort): rebind all.
    pub fn refresh_all(&self) {
        let n = self.imp().count.get();
        if n > 0 {
            self.items_changed(0, n, n);
        }
    }

    pub fn count(&self) -> u32 {
        self.imp().count.get()
    }
}

/// The position an item stands for.
pub fn index_of(item: &glib::Object) -> Option<usize> {
    item.downcast_ref::<glib::BoxedAnyObject>().map(|b| *b.borrow::<usize>())
}
