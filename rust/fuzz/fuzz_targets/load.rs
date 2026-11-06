//! Every input must load or return an error, never panic.
//!
//!     cargo +nightly fuzz run load ../tests/data/psd-tools -- -max_total_time=60

#![no_main]

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    // a small pixel cap keeps each run fast; the size checks are what is being fuzzed, not big allocations
    let options = tinypsd::LoadOptions { max_pixels: 1 << 22, ..Default::default() };
    if let Ok(document) = tinypsd::load(data, &options) {
        for layer in &document.layers {
            assert_eq!(layer.image.pixels.len(), layer.image.width as usize * layer.image.height as usize * 4);
        }
    }
});
