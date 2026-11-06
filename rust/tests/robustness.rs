//! This parses untrusted files: no input may panic. Truncations of every test file and thousands of random
//! corruptions must either load or return an error, and what loads has to be consistent. Run in a debug build, integer
//! overflow panics too, so this also finds arithmetic that would silently wrap.

use std::path::PathBuf;

use tinypsd::{Document, LoadOptions};

fn files() -> Vec<(String, Vec<u8>)> {
    let directory = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../tests/data/psd-tools");
    let mut files: Vec<(String, Vec<u8>)> = std::fs::read_dir(&directory)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| path.extension().is_some_and(|extension| extension == "psd" || extension == "psb"))
        .map(|path| (path.file_name().unwrap().to_string_lossy().into_owned(), std::fs::read(&path).unwrap()))
        .collect();
    files.sort();
    files
}

fn check_consistent(document: &Document) {
    for layer in &document.layers {
        let image = &layer.image;
        assert_eq!(image.pixels.len(), image.width.max(0) as usize * image.height.max(0) as usize * 4);
        if let Some(mask) = &layer.mask {
            if !mask.values.is_empty() {
                assert_eq!(mask.values.len(), mask.bounds.width() as usize * mask.bounds.height() as usize);
            }
            mask.value_at(mask.bounds.right - 1, mask.bounds.bottom - 1);
            mask.value_at(i32::MIN, i32::MAX);
        }
        layer.image.row(layer.image.height - 1);
        layer.image.row(-1);
    }
}

#[test]
fn truncation_is_an_error_or_a_smaller_result() {
    let options = LoadOptions { max_pixels: 1 << 20, ..LoadOptions::default() };
    for (name, data) in files() {
        let full = tinypsd::load(&data, &options);
        // every size through the headers, then every 13th byte through the pixel data
        for size in (0..data.len()).filter(|&size| size < 4096 || size % 13 == 0) {
            if let Ok(document) = tinypsd::load(&data[..size], &options) {
                // only the composite at the very end may go missing
                check_consistent(&document);
                assert!(document.composite.is_empty(), "{name} cut to {size} bytes kept its composite");
                assert_eq!(document.layers.len(), full.as_ref().map_or(0, |full| full.layers.len()), "{name} cut to {size} bytes");
            }
        }
    }
}

#[test]
fn random_corruption_never_panics() {
    // a small generator, so the test needs no dependencies
    let mut state = 20_261_106u64;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    let options = LoadOptions { max_pixels: 1 << 20, ..LoadOptions::default() };
    let mut loaded = 0;
    let files = files();
    for round in 0..6000 {
        let (_, data) = &files[round % files.len()];
        let mut corrupted = data.clone();
        for _ in 0..1 + round % 8 {
            let position = (next() % corrupted.len() as u64) as usize;
            corrupted[position] = next() as u8;
        }
        if let Ok(document) = tinypsd::load(&corrupted, &options) {
            check_consistent(&document);
            loaded += 1;
        }
    }
    println!("{loaded} of 6000 corrupted files still loaded");
}
