# tinypsd for Rust

[![CI](https://github.com/varnholt/tinypsd/actions/workflows/ci.yml/badge.svg)](https://github.com/varnholt/tinypsd/actions/workflows/ci.yml)

A loader for Adobe Photoshop (`.psd`) files: layers with their names, bounds, opacity, visibility, blend mode and
groups, decoded to 8 bit RGBA, plus raster and vector masks, the merged composite and saved paths.

This is the Rust port of the C++23 header [tinypsd](https://github.com/varnholt/tinypsd). Both decode the same files to
the same bytes, and CI holds them to that: they are checked against one digest of every test file, including truncated
and corrupted versions of them.

- no dependencies and no `unsafe`
- 8, 16 and 32 bit; RGB, grayscale, CMYK, indexed, duotone and bitmap; raw, RLE, zip and zip with prediction
- loads from a file or from memory, e.g. out of an asset archive
- errors say what went wrong and at which byte; nothing panics, nothing is printed
- built for untrusted files: every read is bounds checked, sizes are validated before anything is allocated, and the
  loader is fuzzed

## Usage

```toml
[dependencies]
tinypsd = "0.1"
```

```rust
use tinypsd::{load_file, LoadOptions};

let document = load_file("character.psd", &LoadOptions::default())?;
for layer in &document.layers {
    // bottom to top, like photoshop stores them
    if !layer.is_image_layer() || !layer.visible {
        continue; // group markers and empty layers have no pixels
    }
    // layer.image.pixels: width * height * 4 bytes, rgba, top row first
    upload(&layer.name, layer.bounds.left, layer.bounds.top, &layer.image);
}
```

From memory, with options:

```rust
let bytes = archive.read("ui/inventory.psd");
let options = LoadOptions { pixel_format: PixelFormat::Bgra8, load_composite: false, ..LoadOptions::default() };
let document = tinypsd::load(&bytes, &options)?;
```

| | |
|---|---|
| `load(&[u8], &LoadOptions)` | parses a file in memory |
| `load_file(path, &LoadOptions)` | reads and parses a file |
| `Document` | `header` (size, depth, color mode), `layers`, `composite`, `paths`, `find_layer(name)` |
| `Layer` | `name` (UTF-8), `bounds`, `opacity`, `visible`, `clipping`, `blend_mode`, `section_divider`, `image`, `mask`, `vector_mask`, `is_image_layer()`, `is_section_divider()` |
| `Image` | `width`, `height`, `pixels`, `row(y)` |
| `LayerMask` | `bounds`, `default_color` outside them, `disabled`, `density`, `feather`, 8 bit `values`, `value_at(x, y)` |
| `VectorMask` | bezier subpaths in document pixels, `inverted`, `linked`, `disabled`, `starts_filled`, `density`, `feather` |
| `LoadOptions` | `pixel_format` (`Rgba8`, `Bgra8`), `load_layer_pixels`, `load_composite`, `max_pixels` (64 megapixels by default) |
| `Error` | `code`, `offset` in the file, `detail`; implements `Display` and `std::error::Error` |

What is supported and how groups and masks are stored is described in the
[main README](https://github.com/varnholt/tinypsd#what-is-supported).

## Example

`examples/psd_layers.rs` lists the layers of a file and writes each one, its mask and the composite as a PNG:

```
> cargo run --example psd_layers -- ../tests/data/psd-tools/group.psd layers
100 x 200, 8 bit, 3 channels, 4 layers
  [  3] group    Group 1                           0 x 0    at (0, 0) opacity 255
  [  2] pixels   Shape 1                          41 x 74   at (25, 24) opacity 255
        vector mask   1 subpaths, 4 knots
  [  1] group    </Layer group>                    0 x 0    at (0, 0) opacity 255
  [  0] pixels   Background                      100 x 200  at (0, 0) opacity 255
wrote the layers to layers
```

## Tests

- `tests/parity.rs` decodes every file in `../tests/data/psd-tools` three ways (RGBA, BGRA, structure only), 15
  truncations and 24 corruptions of each, and compares all of it with `../tests/data/digest.txt`. The C++ library's
  `tests/digest.cpp` checks the same file, so the two implementations cannot drift apart.
- `tests/reference.rs` has the reference checks of the C++ library: depths and color modes decode to the same pixels,
  unicode names, groups and masks match what photoshop and psd-tools read.
- `tests/robustness.rs` loads truncations of every test file and 6000 random corruptions; with debug assertions,
  integer overflow would panic too.
- `fuzz/` is a cargo-fuzz target, run for a minute in CI.

Minimum supported Rust version: 1.85.

## License

MIT, see `LICENSE`. The test files in `../tests/data/psd-tools` are from psd-tools, MIT licensed.
