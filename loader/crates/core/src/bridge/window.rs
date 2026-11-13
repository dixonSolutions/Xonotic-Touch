// SPDX-License-Identifier: GPL-3.0-or-later
//! A long list the engine sends in windows (the server browser, the map grid).
//!
//! The renderer's list model always has `total` items; unloaded ones are
//! placeholders. It asks for the rows around what is realised, keeps a bounded
//! number of blocks, and gets back the exact `items-changed` ranges, so a
//! refreshing list updates in place and keeps its scroll anchor.

use serde_json::{Map, Value};
use std::collections::{HashSet, VecDeque};

/// One `items-changed(position, removed, added)` for the platform list model.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Splice {
    pub position: usize,
    pub removed: usize,
    pub added: usize,
}

pub struct ListWindow {
    pub block: usize,
    pub max_blocks: usize,
    total: usize,
    blocks: std::collections::HashMap<usize, Vec<Option<Map<String, Value>>>>,
    /// Least recently used first.
    lru: VecDeque<usize>,
    in_flight: HashSet<usize>,
    /// Loaded blocks whose rows are from an older source revision: shown, but asked again.
    stale: HashSet<usize>,
    /// Bumped on every change.
    pub version: u64,
}

impl Default for ListWindow {
    fn default() -> Self {
        Self::new(50, 60)
    }
}

impl ListWindow {
    pub fn new(block: usize, max_blocks: usize) -> Self {
        Self { block: block.max(1), max_blocks: max_blocks.max(1), total: 0, blocks: Default::default(), lru: VecDeque::new(), in_flight: HashSet::new(), stale: HashSet::new(), version: 0 }
    }

    pub fn total(&self) -> usize {
        self.total
    }

    pub fn row(&self, i: usize) -> Option<&Map<String, Value>> {
        if i >= self.total {
            return None;
        }
        self.blocks.get(&(i / self.block))?.get(i % self.block)?.as_ref()
    }

    pub fn loaded_blocks(&self) -> Vec<usize> {
        let mut v: Vec<usize> = self.blocks.keys().copied().collect();
        v.sort_unstable();
        v
    }

    fn touch(&mut self, b: usize) {
        self.lru.retain(|&x| x != b);
        self.lru.push_back(b);
    }

    /// Stores rows from `offset`; `total` (when given) resizes the list. Returns the
    /// splices the list model must emit, in order: a resize first, then the rows
    /// that changed content (as removed = added = n, which views treat as an update).
    pub fn put(&mut self, offset: usize, rows: Vec<Map<String, Value>>, total: Option<usize>) -> Vec<Splice> {
        let mut out = Vec::new();
        let old_total = self.total;
        let new_total = total.unwrap_or(old_total).max(if rows.is_empty() { 0 } else { offset + rows.len() }.min(total.unwrap_or(usize::MAX)));
        if new_total != old_total {
            if new_total > old_total {
                out.push(Splice { position: old_total, removed: 0, added: new_total - old_total });
            } else {
                out.push(Splice { position: new_total, removed: old_total - new_total, added: 0 });
                let last = if new_total == 0 { None } else { Some((new_total - 1) / self.block) };
                let drop: Vec<usize> = self.blocks.keys().copied().filter(|&b| last.is_none_or(|l| b > l)).collect();
                for b in drop {
                    self.blocks.remove(&b);
                    self.lru.retain(|&x| x != b);
                }
            }
            self.total = new_total;
        }
        let n = rows.len().min(self.total.saturating_sub(offset));
        for (k, r) in rows.into_iter().take(n).enumerate() {
            let i = offset + k;
            let b = i / self.block;
            let block = self.block;
            self.blocks.entry(b).or_insert_with(|| vec![None; block])[i % block] = Some(r);
            self.in_flight.remove(&b);
            self.stale.remove(&b);
            self.touch(b);
        }
        if n > 0 {
            // Rows that already existed as items are updated in place.
            let existing_end = (offset + n).min(old_total);
            if offset < existing_end {
                out.push(Splice { position: offset, removed: existing_end - offset, added: existing_end - offset });
            }
        }
        while self.blocks.len() > self.max_blocks {
            match self.lru.pop_front() {
                Some(b) => {
                    self.blocks.remove(&b);
                }
                None => break,
            }
        }
        self.version += 1;
        out
    }

    /// The (offset, count) requests needed to show rows `first..=last`, with one
    /// block of margin either side; each block is asked once until it arrives.
    pub fn need(&mut self, first: usize, last: usize) -> Vec<(usize, usize)> {
        if self.total == 0 {
            if self.in_flight.contains(&0) || self.blocks.contains_key(&0) {
                return vec![];
            }
            self.in_flight.insert(0);
            return vec![(0, self.block)];
        }
        let last_block = (self.total - 1) / self.block;
        let lo = (first / self.block).saturating_sub(1);
        let hi = ((last.min(self.total - 1)) / self.block + 1).min(last_block);
        let mut out = Vec::new();
        for b in lo..=hi {
            if self.blocks.contains_key(&b) && !self.stale.contains(&b) {
                self.touch(b);
                continue;
            }
            if self.in_flight.insert(b) {
                out.push((b * self.block, self.block));
            }
        }
        out
    }

    /// The source has a new revision: keep the rows on screen, but ask for every block again.
    pub fn invalidate(&mut self) {
        self.stale.extend(self.blocks.keys().copied());
        self.in_flight.clear();
        self.version += 1;
    }

    /// The engine started a new listing: everything goes. Returns the splice.
    pub fn reset(&mut self) -> Option<Splice> {
        let old = self.total;
        self.blocks.clear();
        self.lru.clear();
        self.in_flight.clear();
        self.stale.clear();
        self.total = 0;
        self.version += 1;
        (old > 0).then_some(Splice { position: 0, removed: old, added: 0 })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn rows(from: usize, n: usize) -> Vec<Map<String, Value>> {
        (from..from + n).map(|i| json!({"i": i}).as_object().unwrap().clone()).collect()
    }

    #[test]
    fn asks_once_per_block_and_stays_bounded() {
        let mut w = ListWindow::new(10, 3);
        assert_eq!(w.need(0, 5), vec![(0, 10)]);
        assert_eq!(w.need(0, 5), vec![], "in flight: not asked twice");
        assert_eq!(w.put(0, rows(0, 10), Some(95)), vec![Splice { position: 0, removed: 0, added: 95 }]);
        assert_eq!(w.total(), 95);
        assert_eq!(w.row(3).unwrap()["i"], 3);
        assert!(w.row(50).is_none(), "a placeholder until its block arrives");
        assert_eq!(w.need(40, 49), vec![(30, 10), (40, 10), (50, 10)]);
        for b in [30, 40, 50] {
            assert_eq!(w.put(b, rows(b, 10), None), vec![Splice { position: b, removed: 10, added: 10 }], "an in-place update");
        }
        assert_eq!(w.loaded_blocks(), vec![3, 4, 5], "bounded: the least recently used block went");
        assert_eq!(w.need(90, 500), vec![(80, 10), (90, 10)], "never past the end");
        // The listing shrinks: one splice, and blocks past the end are dropped.
        assert_eq!(w.put(0, vec![], Some(20)), vec![Splice { position: 20, removed: 75, added: 0 }]);
        assert!(w.loaded_blocks().is_empty());
        assert_eq!(w.reset(), Some(Splice { position: 0, removed: 20, added: 0 }));
        assert_eq!(w.reset(), None);
    }

    #[test]
    fn a_refresh_updates_in_place_so_the_view_keeps_its_anchor() {
        let mut w = ListWindow::new(50, 100);
        w.put(0, rows(0, 50), Some(2000));
        // The engine re-sends rows 0..50 with new pings: only an update, no insert or removal.
        assert_eq!(w.put(0, rows(0, 50), Some(2000)), vec![Splice { position: 0, removed: 50, added: 50 }]);
        // Rows past the known total grow the list first, then nothing else changes.
        assert_eq!(w.put(2000, rows(2000, 10), None), vec![Splice { position: 2000, removed: 0, added: 10 }]);
        assert_eq!(w.total(), 2010);
    }
}
