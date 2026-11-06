//! Decodes the test files, truncated and corrupted versions of them included, and compares everything against
//! tests/data/digest.txt, which the C++ library's tests/digest.cpp checks the same way. A difference means the two
//! implementations no longer decode the same bytes. The format is described in tests/digest.cpp.

use std::fmt::Write;
use std::path::PathBuf;

use tinypsd::{Document, Error, Image, LoadOptions, PixelFormat, Subpath};

fn repository() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..")
}

fn fnv1a(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325u64, |hash, &byte| (hash ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3))
}

fn flag(value: bool) -> &'static str {
    if value { "1" } else { "0" }
}

fn optional_byte(value: Option<u8>) -> String {
    value.map_or_else(|| "-".to_string(), |value| value.to_string())
}

fn optional_double(value: Option<f64>) -> String {
    value.map_or_else(|| "-".to_string(), |value| format!("{:016x}", value.to_bits()))
}

fn image(image: &Image) -> String {
    format!("{} {} {} {:016x}", image.width, image.height, image.pixels.len(), fnv1a(&image.pixels))
}

fn subpaths(out: &mut String, list: &[Subpath]) {
    for subpath in list {
        writeln!(out, "subpath {} {}", flag(subpath.closed), subpath.knots.len()).unwrap();
        for knot in &subpath.knots {
            let points = [knot.incoming.x, knot.incoming.y, knot.anchor.x, knot.anchor.y, knot.outgoing.x, knot.outgoing.y];
            let hex: Vec<String> = points.iter().map(|value| format!("{:08x}", value.to_bits())).collect();
            writeln!(out, "knot {} {}", hex.join(" "), flag(knot.linked)).unwrap();
        }
    }
}

/// The detailed digest of a load result, without the "file" line.
fn digest(result: &Result<Document, Error>) -> String {
    let document = match result {
        Err(error) => return format!("error {} {}\n", error.code as i32, error.offset),
        Ok(document) => document,
    };
    let header = &document.header;
    let mut out = format!("header {} {} {} {} {}\n", header.width, header.height, header.channels, header.depth, header.color_mode as i32);
    for (index, layer) in document.layers.iter().enumerate() {
        let b = &layer.bounds;
        writeln!(out, "layer {index} {}", layer.name).unwrap();
        writeln!(
            out,
            "bounds {} {} {} {} opacity {} visible {} clipping {} blend {:08x} section {}",
            b.top,
            b.left,
            b.bottom,
            b.right,
            layer.opacity,
            flag(layer.visible),
            flag(layer.clipping),
            u32::from_be_bytes(layer.blend_mode),
            layer.section_divider as i32
        )
        .unwrap();
        writeln!(out, "image {}", image(&layer.image)).unwrap();
        if let Some(mask) = &layer.mask {
            let m = &mask.bounds;
            writeln!(
                out,
                "mask {} {} {} {} default {} disabled {} relative {} density {} feather {} values {} {:016x}",
                m.top,
                m.left,
                m.bottom,
                m.right,
                mask.default_color,
                flag(mask.disabled),
                flag(mask.relative_to_layer),
                optional_byte(mask.density),
                optional_double(mask.feather),
                mask.values.len(),
                fnv1a(&mask.values)
            )
            .unwrap();
        }
        if let Some(mask) = &layer.vector_mask {
            writeln!(
                out,
                "vector inverted {} linked {} disabled {} filled {} density {} feather {} subpaths {}",
                flag(mask.inverted),
                flag(mask.linked),
                flag(mask.disabled),
                flag(mask.starts_filled),
                optional_byte(mask.density),
                optional_double(mask.feather),
                mask.subpaths.len()
            )
            .unwrap();
            subpaths(&mut out, &mask.subpaths);
        }
    }
    writeln!(out, "composite {}", image(&document.composite)).unwrap();
    for path in &document.paths {
        writeln!(out, "path {} {} subpaths {}", path.id, path.name, path.subpaths.len()).unwrap();
        subpaths(&mut out, &path.subpaths);
    }
    out
}

fn summary(text: String) -> String {
    if text.starts_with("error") { text } else { format!("ok {:016x}\n", fnv1a(text.as_bytes())) }
}

fn digest_all() -> String {
    let directory = repository().join("tests/data/psd-tools");
    let mut names: Vec<String> = std::fs::read_dir(&directory)
        .unwrap()
        .map(|entry| entry.unwrap().file_name().into_string().unwrap())
        .filter(|name| name.ends_with(".psd") || name.ends_with(".psb"))
        .collect();
    names.sort();

    let mut out = String::new();
    for name in &names {
        let data = std::fs::read(directory.join(name)).unwrap();

        // the real file with each option set
        let bgra = LoadOptions { pixel_format: PixelFormat::Bgra8, ..LoadOptions::default() };
        let structure = LoadOptions { load_layer_pixels: false, load_composite: false, ..LoadOptions::default() };
        out += &format!("file {name} default\n{}", digest(&tinypsd::load(&data, &LoadOptions::default())));
        out += &format!("file {name} bgra\n{}", digest(&tinypsd::load(&data, &bgra)));
        out += &format!("file {name} structure\n{}", digest(&tinypsd::load(&data, &structure)));

        // cut off at 15 points
        for part in 1..16 {
            let size = data.len() * part / 16;
            out += &format!("file {name} truncate{size} {}", summary(digest(&tinypsd::load(&data[..size], &LoadOptions::default()))));
        }

        // 24 corrupted copies, from a generator seeded with the name
        let mut state = fnv1a(name.as_bytes());
        let mut next = || {
            state = state.wrapping_mul(6_364_136_223_846_793_005).wrapping_add(1_442_695_040_888_963_407);
            state >> 33
        };
        let small = LoadOptions { max_pixels: 1 << 20, ..LoadOptions::default() };
        for round in 0..24 {
            let mut corrupted = data.clone();
            for _ in 0..1 + round % 4 {
                let position = (next() % corrupted.len() as u64) as usize;
                corrupted[position] = (next() & 0xff) as u8;
            }
            out += &format!("file {name} corrupt{round} {}", summary(digest(&tinypsd::load(&corrupted, &small))));
        }
    }
    out
}

#[test]
fn decodes_the_same_as_the_cpp_library() {
    let actual = digest_all();
    let expected_path = repository().join("tests/data/digest.txt");
    let expected = std::fs::read_to_string(&expected_path).unwrap().replace("\r\n", "\n");
    if actual == expected {
        return;
    }
    let written = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("digest.txt");
    std::fs::write(&written, &actual).unwrap();
    for (line, (left, right)) in actual.lines().zip(expected.lines()).enumerate() {
        assert_eq!(left, right, "line {} of {} differs; the whole digest is in {}", line + 1, expected_path.display(), written.display());
    }
    panic!("{} lines, {} expected; the whole digest is in {}", actual.lines().count(), expected.lines().count(), written.display());
}
