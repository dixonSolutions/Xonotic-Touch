// SPDX-License-Identifier: GPL-3.0-or-later
//! CRC-32 (IEEE, the zip one), table-driven.

const fn table() -> [u32; 256] {
    let mut t = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

static TABLE: [u32; 256] = table();

#[derive(Clone)]
pub struct Crc32(u32);

impl Default for Crc32 {
    fn default() -> Self {
        Self::new()
    }
}

impl Crc32 {
    pub fn new() -> Self {
        Self(0xFFFF_FFFF)
    }
    pub fn update(&mut self, b: &[u8]) {
        let mut c = self.0;
        for &x in b {
            c = TABLE[((c ^ x as u32) & 0xFF) as usize] ^ (c >> 8);
        }
        self.0 = c;
    }
    pub fn value(&self) -> u32 {
        self.0 ^ 0xFFFF_FFFF
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn known_values() {
        let mut c = super::Crc32::new();
        c.update(b"123456789");
        assert_eq!(c.value(), 0xCBF4_3926);
        assert_eq!(super::Crc32::new().value(), 0);
    }
}
