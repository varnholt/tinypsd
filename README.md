# tinypsd

[![CI](https://github.com/varnholt/tinypsd/actions/workflows/ci.yml/badge.svg)](https://github.com/varnholt/tinypsd/actions/workflows/ci.yml)

A loader for Adobe Photoshop (`.psd`) files in a single C++23 header: layers with their names, bounds, opacity,
visibility, blend mode and groups, decoded to 8 bit RGBA, plus the merged composite and saved paths.

- one header, no dependencies beyond the standard library: copy `include/tinypsd/tinypsd.hpp` into your project
- or use it as a CMake library: `add_subdirectory`, `FetchContent` or `find_package`
- loads from a file or from memory, e.g. out of an asset archive
- errors come back as `std::expected`, with what went wrong and where; no exceptions, nothing printed
- built for untrusted files: every read is bounds checked, sizes are validated before anything is allocated, and
  CI runs the tests under address and undefined behavior sanitizers and fuzzes the loader

## Usage

```cpp
#include <tinypsd/tinypsd.hpp>

const auto document = tinypsd::loadFile("character.psd");
if (!document)
{
   std::println("{}", tinypsd::describe(document.error()));  // e.g. "file is truncated at byte 1234: layer channel data"
   return;
}

for (const auto& layer : document->_layers)  // bottom to top, like photoshop stores them
{
   if (!layer.isImageLayer() || !layer._visible)
   {
      continue;  // group markers and empty layers have no pixels
   }

   // layer._image._pixels: width * height * 4 bytes, rgba, top row first
   upload(layer._name, layer._bounds._left, layer._bounds._top, layer._image);
}
```

From memory, with options:

```cpp
const std::vector<std::byte> bytes = archive.read("ui/inventory.psd");
const auto document = tinypsd::load(bytes, {._pixel_format = tinypsd::PixelFormat::Bgra8, ._load_composite = false});
```

### API

Everything lives in namespace `tinypsd`.

| | |
|---|---|
| `load(std::span<const std::byte>, LoadOptions = {})` | parses a file in memory; returns `std::expected<Document, Error>` |
| `loadFile(path, LoadOptions = {})` | reads and parses a file |
| `Document` | `_header` (size, depth, color mode), `_layers`, `_composite`, `_paths`, `findLayer(name)` |
| `Layer` | `_name` (utf-8), `_bounds`, `_opacity`, `_visible`, `_clipping`, `_blend_mode`, `_section_divider`, `_image`, `_mask`, `_vector_mask`, `isImageLayer()`, `isSectionDivider()` |
| `Image` | `_width`, `_height`, `_pixels`, `row(y)` |
| `Path` | the paths panel: subpaths of bezier knots in document pixels |
| `LayerMask` | a raster mask: `_bounds`, `_default_color` outside them, `_disabled`, `_density`, `_feather`, 8 bit `_values`, `valueAt(x, y)` |
| `VectorMask` | bezier subpaths in document pixels, `_inverted`, `_linked`, `_disabled`, `_starts_filled`, `_density`, `_feather` |
| `LoadOptions` | `_pixel_format` (`Rgba8`, `Bgra8`), `_load_layer_pixels`, `_load_composite`, `_max_pixels` (64 megapixels by default) |
| `Error` | `_code`, `_offset` in the file, `_detail`; `describe(error)` turns it into a sentence |

Groups: photoshop stores a group as a folder layer (`OpenFolder` or `ClosedFolder`) above its content and a
`BoundingSectionDivider` below it, all in the same flat list.

Masks: a layer with both a painted mask and a vector mask also carries the vector mask rendered to pixels.
`_mask` is the painted one, which the specification calls the real user mask (psd-tools returns the rendered
one instead); the shapes are in `_vector_mask`. tinypsd does not apply masks to `_image`.

## What is supported

| | |
|---|---|
| color modes | RGB, grayscale, CMYK, indexed, duotone (as its gray ramp), bitmap; all become 8 bit RGBA |
| bit depths | 1 (bitmap), 8, 16 and 32 bit. photoshop's 32 bit documents are linear and get the sRGB curve, so they match the 16 bit save of the same image |
| compression | raw, PackBits (RLE), zip and zip with prediction, which photoshop uses for 16 and 32 bit layers |
| layers | names (unicode), bounds, opacity, visibility, clipping, blend mode key, groups (nested too), transparency |
| masks | raster layer masks (bounds, default color, flags, density, feather, 8 bit coverage) and vector masks (bezier subpaths, flags) |
| also | the merged composite, saved paths |

Not supported, and reported or skipped rather than misread:

- the large document format (`.psb`) and Lab or multichannel documents: an error
- adjustment layers, text and smart object contents, layer effects, shape fills and strokes: skipped
- blending: tinypsd decodes the layers, it does not composite them. `_composite` is the merged image photoshop saved
- color management: ICC profiles are ignored, CMYK is converted with a simple formula

## Example

`examples/psd_layers.cpp` lists the layers of a file and writes each one, and the composite, as a PNG:

```
> psd_layers tests/data/psd-tools/group.psd layers
100 x 200, 8 bit, 3 channels, 4 layers
  [  3] group    Group 1                           0 x 0    at (0, 0) opacity 255
  [  2] pixels   Shape 1                          41 x 74   at (25, 24) opacity 255
  [  1] group    </Layer group>                    0 x 0    at (0, 0) opacity 255
  [  0] pixels   Background                      100 x 200  at (0, 0) opacity 255
wrote the layers to layers
```

## Build

Requirements: CMake 3.25+ and a C++23 compiler. CI builds and tests with MSVC (Visual Studio 2022), GCC 14,
Clang 20 and AppleClang (macOS 15).

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

| Option | Default | |
|--------|---------|-|
| `TINYPSD_BUILD_TESTS` | top level only | tests against generated files and real photoshop and gimp files |
| `TINYPSD_BUILD_EXAMPLES` | top level only | `psd_layers` |
| `TINYPSD_BUILD_FUZZER` | `OFF` | libFuzzer target, clang only |
| `TINYPSD_INSTALL` | top level only | install rules and the CMake package |

## Tests

- `tests/tinypsd_test.cpp` builds files with a small writer (`tests/psd_writer.h`) that follows the specification:
  every depth with every compression, the color modes, groups, unicode names, masks and paths, plus every
  truncation and 3000 random corruptions of a file that uses all of it.
- `tests/reference_test.cpp` loads real files from photoshop and gimp, taken from
  [psd-tools](https://github.com/psd-tools/psd-tools): the same image saved at 8, 16 and 32 bit and in other color
  modes has to decode to the same pixels.
- CI runs both under ASan and UBSan, and fuzzes `load()` with libFuzzer for a minute.

## Background

tinypsd started as the psd loader of [deceptus](https://github.com/varnholt/deceptus_engine), a platformer whose
levels and sprites are layered photoshop files. It is a rewrite of the earlier `psd` repository with the fixes the
game needed, plus zip compression, 16 and 32 bit and unicode names.

## License

MIT, see `LICENSE`. The files in `tests/data/psd-tools` are from psd-tools, MIT licensed, see the `LICENSE` there.
