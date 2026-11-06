//! A loader for Adobe Photoshop (`.psd`) files: layers with their names, bounds, opacity, visibility, blend mode and
//! groups, decoded to 8 bit RGBA, plus raster and vector masks, the merged composite and saved paths.
//!
//! The Rust port of the C++23 header [tinypsd](https://github.com/varnholt/tinypsd). Both decode the same files to
//! the same bytes; CI checks that against one digest of every test file.
//!
//! - no dependencies and no `unsafe`
//! - loads from a file or from memory, e.g. out of an asset archive
//! - errors say what went wrong and at which byte; nothing panics and nothing is printed
//! - built for untrusted files: every read is bounds checked and sizes are validated before anything is allocated
//!
//! ```no_run
//! let document = tinypsd::load_file("character.psd", &tinypsd::LoadOptions::default())?;
//! for layer in &document.layers {
//!     // bottom to top, like photoshop stores them
//!     if layer.is_image_layer() && layer.visible {
//!         // layer.image.pixels: width * height * 4 bytes, rgba, top row first
//!         println!("{} at {}, {}", layer.name, layer.bounds.left, layer.bounds.top);
//!     }
//! }
//! # Ok::<(), tinypsd::Error>(())
//! ```
//!
//! Format specification: <https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/>

#![forbid(unsafe_code)]
#![warn(missing_docs)]

mod decode;
mod inflate;
mod parser;
mod reader;

use std::fmt;

/// The color mode from the file header.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ColorMode {
    /// 1 bit black and white.
    Bitmap = 0,
    /// One gray channel.
    Grayscale = 1,
    /// One channel of indices into a 256 color palette.
    Indexed = 2,
    /// Red, green and blue.
    Rgb = 3,
    /// Cyan, magenta, yellow and black, stored inverted (255 is no ink).
    Cmyk = 4,
    /// Not supported: loading fails with [`ErrorCode::UnsupportedColorMode`].
    Multichannel = 7,
    /// Read as its gray ramp.
    Duotone = 8,
    /// Not supported: loading fails with [`ErrorCode::UnsupportedColorMode`].
    Lab = 9,
}

/// The byte order of the decoded pixels in memory, 4 bytes per pixel.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum PixelFormat {
    /// r, g, b, a; what OpenGL, Vulkan, wgpu and most image crates expect.
    #[default]
    Rgba8,
    /// b, g, r, a; what Direct3D and Windows bitmaps expect.
    Bgra8,
}

/// What went wrong.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ErrorCode {
    /// The file does not exist or cannot be read.
    CannotOpenFile = 0,
    /// The signature is not `8BPS`.
    NotAPsdFile = 1,
    /// Version 2 is the large document format (`.psb`), which is not supported.
    UnsupportedVersion = 2,
    /// Lab and multichannel documents.
    UnsupportedColorMode = 3,
    /// A bit depth the color mode does not allow.
    UnsupportedDepth = 4,
    /// More pixels than [`LoadOptions::max_pixels`].
    ImageTooLarge = 5,
    /// The data ends before a section does.
    Truncated = 6,
    /// A value is out of its valid range.
    Corrupt = 7,
}

impl fmt::Display for ErrorCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            ErrorCode::CannotOpenFile => "cannot open file",
            ErrorCode::NotAPsdFile => "not a psd file",
            ErrorCode::UnsupportedVersion => "unsupported version (psb is not supported)",
            ErrorCode::UnsupportedColorMode => "unsupported color mode",
            ErrorCode::UnsupportedDepth => "unsupported bit depth",
            ErrorCode::ImageTooLarge => "image too large",
            ErrorCode::Truncated => "file is truncated",
            ErrorCode::Corrupt => "file is corrupt",
        })
    }
}

/// A load error: what, where and the details. Displays as e.g. "file is truncated at byte 1234: layer channel data".
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    /// What went wrong.
    pub code: ErrorCode,
    /// The byte offset in the file where the problem was found.
    pub offset: usize,
    /// What exactly was wrong.
    pub detail: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.code)?;
        if self.code != ErrorCode::CannotOpenFile {
            write!(f, " at byte {}", self.offset)?;
        }
        if !self.detail.is_empty() {
            write!(f, ": {}", self.detail)?;
        }
        Ok(())
    }
}

impl std::error::Error for Error {}

/// Group markers: photoshop stores a group as an open or closed folder layer above its content and a bounding
/// divider below it, all in the same flat list of layers.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum SectionDivider {
    /// An ordinary layer.
    #[default]
    None = -1,
    /// Any other layer type that carries the section key.
    Other = 0,
    /// The folder layer of an expanded group.
    OpenFolder = 1,
    /// The folder layer of a collapsed group.
    ClosedFolder = 2,
    /// The end of a group, below its content.
    BoundingSectionDivider = 3,
}

/// Decoded 8 bit pixels, 4 bytes per pixel in the requested [`PixelFormat`], top row first, no padding.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Image {
    /// Width in pixels.
    pub width: i32,
    /// Height in pixels.
    pub height: i32,
    /// `width * height * 4` bytes.
    pub pixels: Vec<u8>,
}

impl Image {
    /// True if there are no pixels.
    pub fn is_empty(&self) -> bool {
        self.pixels.is_empty()
    }

    /// The pixels of row `y`, or None outside the image.
    pub fn row(&self, y: i32) -> Option<&[u8]> {
        let row_size = usize::try_from(self.width).ok()? * 4;
        let start = usize::try_from(y).ok()?.checked_mul(row_size)?;
        self.pixels.get(start..start + row_size)
    }
}

/// A rectangle in document pixels; right and bottom are exclusive.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Rect {
    /// The first row.
    pub top: i32,
    /// The first column.
    pub left: i32,
    /// One past the last row.
    pub bottom: i32,
    /// One past the last column.
    pub right: i32,
}

impl Rect {
    /// `right - left`.
    pub fn width(&self) -> i32 {
        self.right.wrapping_sub(self.left)
    }

    /// `bottom - top`.
    pub fn height(&self) -> i32 {
        self.bottom.wrapping_sub(self.top)
    }
}

/// A point in document pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Point {
    /// Horizontal, in document pixels.
    pub x: f32,
    /// Vertical, in document pixels, downwards.
    pub y: f32,
}

/// A bezier knot: the anchor and its two control points.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Knot {
    /// The control point of the curve arriving at the anchor.
    pub incoming: Point,
    /// The point the path goes through.
    pub anchor: Point,
    /// The control point of the curve leaving the anchor.
    pub outgoing: Point,
    /// True if the control points move together.
    pub linked: bool,
}

/// One connected piece of a path.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Subpath {
    /// True if the last knot connects back to the first.
    pub closed: bool,
    /// The knots in order.
    pub knots: Vec<Knot>,
}

/// A raster layer mask: 8 bit coverage that hides the layer where it is dark.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LayerMask {
    /// In document pixels; independent of the layer bounds.
    pub bounds: Rect,
    /// The value outside `bounds`: 0 hides the layer there, 255 shows it.
    pub default_color: u8,
    /// Switched off in photoshop; the layer shows unmasked.
    pub disabled: bool,
    /// Photoshop's "position relative to layer" flag.
    pub relative_to_layer: bool,
    /// Masks panel density, 255 = full effect; None if never changed.
    pub density: Option<u8>,
    /// Masks panel feather radius in pixels; None if never changed.
    pub feather: Option<f64>,
    /// `bounds` sized, top row first, 255 = shown; empty if pixels were not loaded.
    pub values: Vec<u8>,
}

impl LayerMask {
    /// The mask value at a document pixel, the default color outside the bounds.
    pub fn value_at(&self, x: i32, y: i32) -> u8 {
        let b = &self.bounds;
        if self.values.is_empty() || x < b.left || x >= b.right || y < b.top || y >= b.bottom {
            return self.default_color;
        }
        let index = (i64::from(y) - i64::from(b.top)) * i64::from(b.width()) + (i64::from(x) - i64::from(b.left));
        usize::try_from(index).ok().and_then(|index| self.values.get(index)).copied().unwrap_or(self.default_color)
    }
}

/// A vector mask: bezier shapes that clip the layer, e.g. drawn with the pen tool or from a shape layer.
#[derive(Clone, Debug, PartialEq)]
pub struct VectorMask {
    /// In document pixels.
    pub subpaths: Vec<Subpath>,
    /// Shows the outside of the shapes instead of the inside.
    pub inverted: bool,
    /// Moves with the layer.
    pub linked: bool,
    /// Switched off in photoshop.
    pub disabled: bool,
    /// Photoshop's initial fill record: the shapes start from a filled canvas.
    pub starts_filled: bool,
    /// Masks panel density, 255 = full effect; None if never changed.
    pub density: Option<u8>,
    /// Masks panel feather radius in pixels; None if never changed.
    pub feather: Option<f64>,
}

impl Default for VectorMask {
    fn default() -> Self {
        Self { subpaths: Vec::new(), inverted: false, linked: true, disabled: false, starts_filled: false, density: None, feather: None }
    }
}

/// One layer, a group marker included.
#[derive(Clone, Debug, PartialEq)]
pub struct Layer {
    /// UTF-8; the unicode name if the file stores one, else the legacy name.
    pub name: String,
    /// Where the layer is, in document pixels.
    pub bounds: Rect,
    /// 0 to 255.
    pub opacity: u8,
    /// The eye in the layers panel.
    pub visible: bool,
    /// Clipped to the layer below.
    pub clipping: bool,
    /// Photoshop's key, e.g. `b"norm"`, `b"mul "`, `b"scrn"`; see [`Layer::blend_mode_str`].
    pub blend_mode: [u8; 4],
    /// Whether this is a group marker.
    pub section_divider: SectionDivider,
    /// The layer pixels, `bounds` sized; empty for group markers, empty layers or if not loaded.
    pub image: Image,
    /// The raster mask, if the layer has one.
    pub mask: Option<LayerMask>,
    /// The vector mask, if the layer has one.
    pub vector_mask: Option<VectorMask>,
}

impl Default for Layer {
    fn default() -> Self {
        Self {
            name: String::new(),
            bounds: Rect::default(),
            opacity: 255,
            visible: true,
            clipping: false,
            blend_mode: *b"norm",
            section_divider: SectionDivider::None,
            image: Image::default(),
            mask: None,
            vector_mask: None,
        }
    }
}

impl Layer {
    /// A group marker (folder or bounding divider) rather than content.
    pub fn is_section_divider(&self) -> bool {
        self.section_divider != SectionDivider::None
    }

    /// An ordinary layer with at least one pixel.
    pub fn is_image_layer(&self) -> bool {
        !self.is_section_divider() && self.bounds.width() > 0 && self.bounds.height() > 0
    }

    /// The blend mode key as text, e.g. "norm".
    pub fn blend_mode_str(&self) -> String {
        String::from_utf8_lossy(&self.blend_mode).into_owned()
    }
}

/// A saved path from the image resources (the paths panel), e.g. a clipping path.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Path {
    /// The image resource id, 2000 to 2997.
    pub id: i32,
    /// The name in the paths panel.
    pub name: String,
    /// In document pixels.
    pub subpaths: Vec<Subpath>,
}

/// The file header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    /// Document width in pixels.
    pub width: i32,
    /// Document height in pixels.
    pub height: i32,
    /// Channels of the composite, color and extra ones.
    pub channels: i32,
    /// Bits per channel: 1, 8, 16 or 32.
    pub depth: i32,
    /// The color mode.
    pub color_mode: ColorMode,
}

impl Default for Header {
    fn default() -> Self {
        Self { width: 0, height: 0, channels: 0, depth: 0, color_mode: ColorMode::Rgb }
    }
}

/// What to load and how.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LoadOptions {
    /// The byte order of the decoded pixels.
    pub pixel_format: PixelFormat,
    /// False reads only names, bounds and flags, which is much faster.
    pub load_layer_pixels: bool,
    /// The merged image photoshop stores at the end of the file.
    pub load_composite: bool,
    /// Per image; guards against huge allocations. 64 megapixels by default.
    pub max_pixels: usize,
}

impl Default for LoadOptions {
    fn default() -> Self {
        Self { pixel_format: PixelFormat::Rgba8, load_layer_pixels: true, load_composite: true, max_pixels: 1 << 26 }
    }
}

/// A loaded psd file.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Document {
    /// Size, depth, color mode.
    pub header: Header,
    /// Bottom to top, the order photoshop stores them in.
    pub layers: Vec<Layer>,
    /// The merged image; empty if the file has none or it was not loaded.
    pub composite: Image,
    /// The paths panel.
    pub paths: Vec<Path>,
}

impl Document {
    /// Document width in pixels.
    pub fn width(&self) -> i32 {
        self.header.width
    }

    /// Document height in pixels.
    pub fn height(&self) -> i32 {
        self.header.height
    }

    /// The first layer with this name.
    pub fn find_layer(&self, name: &str) -> Option<&Layer> {
        self.layers.iter().find(|layer| layer.name == name)
    }
}

/// The version of the format support, the same as the C++ library's.
pub const VERSION: (u32, u32, u32) = (1, 0, 0);

/// Parses a psd file that is already in memory, e.g. from an archive.
pub fn load(data: &[u8], options: &LoadOptions) -> Result<Document, Error> {
    parser::Parser::new(data, options.clone()).parse()
}

/// Reads and parses a psd file.
pub fn load_file(path: impl AsRef<std::path::Path>, options: &LoadOptions) -> Result<Document, Error> {
    let path = path.as_ref();
    let data = std::fs::read(path).map_err(|_| Error { code: ErrorCode::CannotOpenFile, offset: 0, detail: path.display().to_string() })?;
    load(&data, options)
}

#[cfg(test)]
mod tests {
    use super::*;

    mod vectors {
        include!("../tests/zlib_vectors.rs");
    }

    #[test]
    fn packbits_noop_and_overruns() {
        let mut row = [0u8; 4];
        let mut unpack = |bytes: &[u8]| {
            row = [0; 4];
            decode::unpack_bits(bytes, &mut row)
        };
        assert!(unpack(&[0x80, 0xfd, 0x07]));
        assert_eq!(row, [7, 7, 7, 7]);
        let mut row2 = [0u8; 4];
        assert!(decode::unpack_bits(&[0x01, 1, 2, 0xff, 3], &mut row2));
        assert_eq!(row2, [1, 2, 3, 3]);
        assert!(!decode::unpack_bits(&[0xfc, 9], &mut row2)); // 5 bytes into a row of 4
        assert!(!decode::unpack_bits(&[0x04, 1, 2], &mut row2)); // literal run longer than the input
        assert!(!decode::unpack_bits(&[0x00, 1], &mut row2)); // the input ends before the row is full
    }

    #[test]
    fn inflate_fixed_and_dynamic_blocks() {
        let fixed_text = b"tinypsd tinypsd tinypsd!";
        let mut fixed = vec![0u8; fixed_text.len()];
        assert!(inflate::inflate(&vectors::ZLIB_FIXED, &mut fixed));
        assert_eq!(&fixed, fixed_text);

        let mut expected: Vec<u8> = (0..3000).map(|index: u32| ((index * 7 + (index / 13) * 31) % 251) as u8).collect();
        for _ in 0..200 {
            expected.extend_from_slice(b"abcabcabc");
        }
        let mut dynamic = vec![0u8; expected.len()];
        assert!(inflate::inflate(&vectors::ZLIB_DYNAMIC, &mut dynamic));
        assert_eq!(dynamic, expected);

        // wrong sizes and a broken checksum are errors, not partial results
        assert!(!inflate::inflate(&vectors::ZLIB_DYNAMIC, &mut vec![0u8; expected.len() - 1]));
        assert!(!inflate::inflate(&vectors::ZLIB_DYNAMIC, &mut vec![0u8; expected.len() + 1]));
        let mut broken = vectors::ZLIB_FIXED.to_vec();
        *broken.last_mut().unwrap() ^= 0x01;
        assert!(!inflate::inflate(&broken, &mut fixed));
        assert!(!inflate::inflate(&vectors::ZLIB_FIXED[..10], &mut fixed));
    }

    #[test]
    fn errors_describe_themselves() {
        let error = Error { code: ErrorCode::Truncated, offset: 1234, detail: "layer channel data".into() };
        assert_eq!(error.to_string(), "file is truncated at byte 1234: layer channel data");
        let missing = load_file("does/not/exist.psd", &LoadOptions::default()).unwrap_err();
        assert_eq!(missing.code, ErrorCode::CannotOpenFile);
        assert!(missing.to_string().starts_with("cannot open file: "));
    }

    #[test]
    fn headers_are_checked() {
        assert_eq!(load(b"GIF8", &LoadOptions::default()).unwrap_err().code, ErrorCode::NotAPsdFile);
        assert_eq!(load(b"8BPS\0\x01", &LoadOptions::default()).unwrap_err().code, ErrorCode::Truncated);
        let mut header = b"8BPS\0\x02\0\0\0\0\0\0\0\x03\0\0\0\x04\0\0\0\x04\0\x08\0\x03".to_vec();
        assert_eq!(load(&header, &LoadOptions::default()).unwrap_err().code, ErrorCode::UnsupportedVersion);
        header[5] = 1;
        header[25] = 9; // lab
        assert_eq!(load(&header, &LoadOptions::default()).unwrap_err().code, ErrorCode::UnsupportedColorMode);
        header[25] = 3;
        header[23] = 7; // depth 7
        assert_eq!(load(&header, &LoadOptions::default()).unwrap_err().code, ErrorCode::UnsupportedDepth);
    }
}
