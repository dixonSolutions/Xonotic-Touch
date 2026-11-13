// SPDX-License-Identifier: GPL-3.0-or-later
//! Pictures by image ref (docs/UI_BRIDGE.md 5.2). A picture asks once for its
//! ref at the box it draws, shows a pulsing placeholder until the engine
//! answers, and drops its texture when its row or card is unbound. Textures
//! are cached per ref for the session (bounded).

use gtk::prelude::*;
use gtk::{gdk, glib};
use std::collections::{HashMap, VecDeque};

#[derive(Default)]
pub struct Images {
    textures: HashMap<String, gdk::Texture>,
    order: VecDeque<String>,
    waiting: HashMap<String, Vec<glib::WeakRef<gtk::Picture>>>,
}

const MAX_TEXTURES: usize = 300;

impl Images {
    /// Shows the cached texture or registers the picture to receive it; true when the engine must be asked.
    pub fn attach(&mut self, r: &str, pic: &gtk::Picture) -> bool {
        if let Some(t) = self.textures.get(r) {
            pic.set_paintable(Some(t));
            pic.remove_css_class("gn-placeholder");
            return false;
        }
        pic.set_paintable(gdk::Paintable::NONE);
        pic.add_css_class("gn-placeholder");
        let list = self.waiting.entry(r.to_string()).or_default();
        let first = list.is_empty();
        list.push(pic.downgrade());
        first
    }

    fn put(&mut self, r: &str, t: gdk::Texture) {
        if !self.textures.contains_key(r) {
            self.order.push_back(r.to_string());
            while self.order.len() > MAX_TEXTURES {
                if let Some(old) = self.order.pop_front() {
                    self.textures.remove(&old);
                }
            }
        }
        self.textures.insert(r.to_string(), t.clone());
        for w in self.waiting.remove(r).unwrap_or_default() {
            if let Some(p) = w.upgrade() {
                p.set_paintable(Some(&t));
                p.remove_css_class("gn-placeholder");
            }
        }
    }

    pub fn arrived(&mut self, r: &str, path: Option<&str>, data: Option<&[u8]>) {
        let t = match (data, path) {
            (Some(d), _) => gdk::Texture::from_bytes(&glib::Bytes::from(d)).ok(),
            (None, Some(p)) => gdk::Texture::from_filename(p).ok(),
            _ => None,
        };
        match t {
            Some(t) => self.put(r, t),
            None => self.missing(r),
        }
    }

    /// No such image: the placeholder stays, without the pulse.
    pub fn missing(&mut self, r: &str) {
        for w in self.waiting.remove(r).unwrap_or_default() {
            if let Some(p) = w.upgrade() {
                p.remove_css_class("gn-placeholder");
                p.add_css_class("gn-missing");
            }
        }
    }

    /// The loader's own brand assets, available before any engine runs.
    pub fn preload_brand(&mut self) {
        let banner = glib::Bytes::from_static(include_bytes!("../data/banner-800.png"));
        if let Ok(t) = gdk::Texture::from_bytes(&banner) {
            self.put("asset:banner", t);
        }
    }
}
