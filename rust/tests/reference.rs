//! Real files from photoshop and gimp, taken from psd-tools (MIT, see tests/data/psd-tools/LICENSE), the same checks as
//! the C++ library's tests/reference_test.cpp: the same image saved at different depths and color modes has to decode
//! to the same pixels, and names, groups, flags and masks have to match what photoshop shows.

use std::path::PathBuf;

use tinypsd::{Document, ErrorCode, Image, Knot, LayerMask, LoadOptions, Rect, SectionDivider};

fn path(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../tests/data/psd-tools").join(name)
}

fn load(name: &str) -> Document {
    tinypsd::load_file(path(name), &LoadOptions::default()).unwrap_or_else(|error| panic!("{name}: {error}"))
}

fn load_error(name: &str) -> ErrorCode {
    tinypsd::load_file(path(name), &LoadOptions::default()).expect_err(name).code
}

/// The largest difference between two images of the same size, 256 if the sizes differ.
fn max_difference(lhs: &Image, rhs: &Image) -> i32 {
    if lhs.width != rhs.width || lhs.height != rhs.height || lhs.pixels.len() != rhs.pixels.len() || lhs.is_empty() {
        return 256;
    }
    lhs.pixels.iter().zip(&rhs.pixels).map(|(&a, &b)| (i32::from(a) - i32::from(b)).abs()).max().unwrap_or(0)
}

fn names(document: &Document) -> Vec<&str> {
    document.layers.iter().map(|layer| layer.name.as_str()).collect()
}

#[test]
fn one_image_at_every_depth_and_color_mode_decodes_the_same() {
    let rgb8 = load("4x4_8bit_rgb.psd");
    for other in ["4x4_16bit_rgb.psd", "4x4_8bit_rgba.psd", "4x4_8bit_index_color.psd"] {
        let difference = max_difference(&rgb8.composite, &load(other).composite);
        assert!(difference <= 1, "{other} differs by {difference}");
    }

    let gray8 = load("4x4_8bit_grayscale.psd");
    for other in ["4x4_16bit_grayscale.psd", "4x4_8bit_duotone.psd"] {
        assert!(max_difference(&gray8.composite, &load(other).composite) <= 1, "{other}");
    }

    // photoshop's 32 bit documents are linear; the 16 bit save of the same document is the reference
    let deep16 = load("16bit5x5.psd");
    let deep32 = load("32bit5x5.psd");
    assert!(max_difference(&deep16.composite, &deep32.composite) <= 1);
    assert_eq!(deep16.layers.len(), 3);
    assert_eq!(deep32.layers.len(), 3);
    for (layer16, layer32) in deep16.layers.iter().zip(&deep32.layers) {
        // zip with prediction, 16 bit deltas versus 32 bit byte planes
        assert!(max_difference(&layer16.image, &layer32.image) <= 1, "{}", layer16.name);
    }
}

#[test]
fn bitmap_documents_are_black_and_white() {
    for name in ["4x4_1bit_bitmap.psd", "100x20_1bit_bitmap_rle.psd"] {
        let pixels = load(name).composite.pixels;
        assert!(!pixels.is_empty());
        assert!(pixels.iter().all(|&value| value == 0 || value == 255));
        assert!(pixels.contains(&0));
    }
}

#[test]
fn names_groups_and_flags() {
    // russian photoshop: the unicode names, not the legacy code page bytes
    let two = load("2layers.psd");
    assert_eq!(names(&two), ["Фон", "Слой"]);
    assert_eq!((two.layers[1].bounds.width(), two.layers[1].bounds.height()), (85, 46));

    let emoji = load("layer-name-emoji.psd");
    assert_eq!(emoji.layers.len(), 1);
    assert_eq!(emoji.layers[0].name, "👽");
    assert_eq!(emoji.layers[0].opacity, 128);

    let group = load("group.psd");
    assert_eq!(names(&group), ["Background", "</Layer group>", "Shape 1", "Group 1"]);
    assert_eq!(group.layers[1].section_divider, SectionDivider::BoundingSectionDivider);
    assert_eq!(group.layers[3].section_divider, SectionDivider::OpenFolder);
    assert!(group.layers[2].is_image_layer() && !group.layers[2].image.is_empty());

    let empty_layer = load("empty-layer.psd");
    let empty = empty_layer.find_layer("empty").expect("a layer called empty");
    assert!(!empty.is_image_layer() && empty.image.is_empty());

    let hidden = load("hidden-layer.psd");
    assert_eq!(hidden.layers.len(), 3);
    assert!(hidden.layers[1].visible && !hidden.layers[2].visible);
}

#[test]
fn a_gimp_file_with_transparency() {
    let document = load("transparentbg-gimp.psd");
    assert_eq!(document.layers.len(), 1);
    assert!(document.layers[0].image.pixels.chunks_exact(4).any(|pixel| pixel[3] < 255));
}

// the expected values below are what psd-tools 1.10.9 reads from the same files

fn mask_sum(mask: &LayerMask) -> u64 {
    mask.values.iter().map(|&value| u64::from(value)).sum()
}

fn bounds(left: i32, top: i32, right: i32, bottom: i32) -> Rect {
    Rect { top, left, bottom, right }
}

fn anchor_is(knot: &Knot, x: f32, y: f32) -> bool {
    (knot.anchor.x - x).abs() < 0.01 && (knot.anchor.y - y).abs() < 0.01
}

#[test]
fn a_layer_mask_and_a_shape_layers_vector_mask() {
    for name in ["mask.psd", "mask-disabled.psd"] {
        let document = load(name);
        assert_eq!(names(&document), ["Background", "Background copy", "Shape 1"]);
        assert!(document.layers[0].mask.is_none() && document.layers[0].vector_mask.is_none());

        let masked = &document.layers[1];
        let mask = masked.mask.as_ref().expect("a layer mask");
        assert_eq!(mask.bounds, bounds(23, 10, 94, 67));
        assert_eq!(mask.default_color, 0);
        assert_eq!(mask.disabled, name == "mask-disabled.psd");
        assert_eq!(mask_sum(mask), 874_692, "{name}");
        assert!(masked.vector_mask.is_none());

        let shape = &document.layers[2];
        assert!(shape.mask.is_none());
        let vector_mask = shape.vector_mask.as_ref().expect("a vector mask");
        assert!(!vector_mask.inverted && vector_mask.linked && !vector_mask.disabled && !vector_mask.starts_filled);
        assert_eq!(vector_mask.subpaths.len(), 1);
        let subpath = &vector_mask.subpaths[0];
        assert!(subpath.closed && subpath.knots.len() == 4);
        assert!(anchor_is(&subpath.knots[0], 17.0, 38.0));
        assert!(anchor_is(&subpath.knots[1], 82.0, 38.0));
    }
}

#[test]
fn layer_and_vector_mask_on_one_layer_with_density() {
    // each layer has a painted mask and a vector mask, so photoshop writes two mask channels: -2 is the vector mask
    // rendered to pixels and -3 the painted mask. the specification calls -3 the real user mask, and that is what
    // `mask` holds. psd-tools 1.10.9 returns -2 instead; bounds and sums are those of channel -3 as psd-tools decodes it
    let document = load("mask-density-layervectormask.psd");
    assert_eq!(document.layers.len(), 5);
    let expected: [(i32, i32, u64, Option<u8>); 4] =
        [(0, 32, 43104, Some(64)), (8, 32, 43107, Some(128)), (16, 32, 43102, Some(191)), (24, 31, 41072, None)];
    for (index, &(top, right, sum, density)) in expected.iter().enumerate() {
        let layer = &document.layers[index + 1];
        let mask = layer.mask.as_ref().expect("a layer mask");
        let vector_mask = layer.vector_mask.as_ref().expect("a vector mask");
        assert_eq!(mask.bounds, bounds(0, top, right, top + 8), "{}", layer.name);
        assert_eq!(mask.default_color, 255);
        assert_eq!(mask_sum(mask), sum, "{}", layer.name);
        assert_eq!(mask.density, density);
        assert_eq!(vector_mask.density, density);

        let row = index as f32 * 8.0;
        let knots = &vector_mask.subpaths[0].knots;
        assert!(vector_mask.subpaths.len() == 1 && knots.len() == 4, "{}", layer.name);
        assert!(anchor_is(&knots[0], 16.0, row) && anchor_is(&knots[1], 32.0, row), "{}", layer.name);
    }
}

#[test]
fn mask_parameters_without_a_real_mask_channel() {
    // psd-tools 1.10.9 fails on this file; it has to load here, with consistent masks
    let document = load("mask-parameters-no-real-channel.psd");
    assert!(!document.layers.is_empty());
    for mask in document.layers.iter().filter_map(|layer| layer.mask.as_ref()) {
        if !mask.values.is_empty() {
            assert_eq!(mask.values.len(), (mask.bounds.width() * mask.bounds.height()) as usize);
        }
    }
}

#[test]
fn unsupported_files_are_reported_not_misread() {
    assert_eq!(load_error("1layer.psb"), ErrorCode::UnsupportedVersion);
    assert_eq!(load_error("4x4_8bit_lab.psd"), ErrorCode::UnsupportedColorMode);
}

#[test]
fn options_skip_work_and_cap_the_size() {
    let bytes = std::fs::read(path("group.psd")).unwrap();
    let structure =
        tinypsd::load(&bytes, &LoadOptions { load_layer_pixels: false, load_composite: false, ..LoadOptions::default() }).unwrap();
    assert_eq!(structure.layers.len(), 4);
    assert!(structure.layers.iter().all(|layer| layer.image.is_empty()) && structure.composite.is_empty());

    let rgba = load("group.psd");
    let bgra = tinypsd::load(&bytes, &LoadOptions { pixel_format: tinypsd::PixelFormat::Bgra8, ..LoadOptions::default() }).unwrap();
    for (a, b) in rgba.composite.pixels.chunks_exact(4).zip(bgra.composite.pixels.chunks_exact(4)) {
        assert_eq!([a[2], a[1], a[0], a[3]], [b[0], b[1], b[2], b[3]]);
    }
    assert_eq!(rgba.composite.row(1).map(<[u8]>::len), Some(rgba.composite.width as usize * 4));
    assert_eq!(rgba.composite.row(rgba.composite.height), None);

    let capped = tinypsd::load(&bytes, &LoadOptions { max_pixels: 100, ..LoadOptions::default() }).unwrap_err();
    assert_eq!(capped.code, ErrorCode::ImageTooLarge);
}
