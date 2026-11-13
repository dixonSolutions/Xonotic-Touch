// SPDX-License-Identifier: GPL-3.0-or-later
//! What a platform UI implements to show the engine's screens. The GTK app
//! (crates/gtk) and the Android Kotlin views (through crates/android) are two
//! implementations of this one interface; [`dispatch`] drives either from the
//! [`Store`]'s changes. Requests the renderer needs to make (windows, images,
//! sync) go out through the [`crate::bridge::Client`] it holds.

use crate::bridge::{Change, Splice, Store};

pub trait Renderer {
    /// The tree was replaced: show `app.stack`'s top screen and `app.modal`, reusing widgets by id.
    fn rebuild(&mut self, store: &Store);
    /// These nodes were inserted or replaced: update their widgets in place (and the screen stack if `app` is among them).
    fn update(&mut self, store: &Store, ids: &[String]);
    /// These nodes are gone.
    fn remove(&mut self, store: &Store, ids: &[String]);
    /// Rows of a data source changed: emit these `items-changed` splices on every model over it.
    fn rows(&mut self, store: &Store, source: &str, splices: &[Splice]);
    /// A data source has a new revision: ask again for the visible rows.
    fn source_changed(&mut self, store: &Store, source: &str);
    fn image(&mut self, reference: &str, path: Option<&str>, data: Option<&[u8]>);
    fn image_missing(&mut self, reference: &str);
    /// The store is out of step: send `sync`.
    fn need_sync(&mut self, store: &Store);
    fn error(&mut self, code: &str, msg: &str);
    /// The engine closed (quit, crash, the match ended in a session that exits).
    fn bye(&mut self, reason: &str);
}

/// Hands every change to the renderer, in order.
pub fn dispatch(store: &Store, changes: &[Change], r: &mut dyn Renderer) {
    for c in changes {
        match c {
            Change::Hello { .. } => {}
            Change::Snapshot => r.rebuild(store),
            Change::Set(ids) => r.update(store, ids),
            Change::Deleted(ids) => r.remove(store, ids),
            Change::NeedSync => r.need_sync(store),
            Change::Rows { source, splices } => r.rows(store, source, splices),
            Change::Source { source, .. } => r.source_changed(store, source),
            Change::Image { reference, path, data } => r.image(reference, path.as_deref(), data.as_deref()),
            Change::ImageMissing(reference) => r.image_missing(reference),
            Change::Error { code, msg } => r.error(code, msg),
            Change::Bye(reason) => r.bye(reason),
        }
    }
}
