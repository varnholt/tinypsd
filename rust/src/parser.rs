//! The file sections, in the order they are stored: header, color mode data, image resources, layer and mask
//! information, image data.

use crate::decode::{SampleEncoding, expand_bits, samples_to_8bit, undo_prediction, unpack_bits};
use crate::inflate::inflate;
use crate::reader::Reader;
use crate::{
    ColorMode, Document, Error, ErrorCode, Image, Knot, Layer, LayerMask, LoadOptions, Path, PixelFormat, Point, Rect, SectionDivider,
    Subpath, VectorMask,
};

/// The psd limit; psb allows 300,000.
const MAX_DIMENSION: i64 = 30_000;
const MAX_CHANNELS: u16 = 56;

/// Channel ids: -2 is the layer mask, -3 the "real" user mask photoshop writes when a layer has both a user mask and a
/// rendered one; 0 means the layer has no raster mask to decode.
const NO_MASK_CHANNEL: i32 = 0;

const fn four_cc(text: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*text)
}

type Result<T> = std::result::Result<T, Error>;

fn fail<T>(code: ErrorCode, offset: usize, detail: impl Into<String>) -> Result<T> {
    Err(Error { code, offset, detail: detail.into() })
}

/// A Truncated error if the reader ran out of data.
fn check_read(reader: &Reader, what: &str) -> Result<()> {
    if reader.failed() {
        return fail(ErrorCode::Truncated, reader.offset(), what);
    }
    Ok(())
}

/// Legacy pascal strings are in the system code page; latin-1 is the closest portable guess.
fn latin1_to_utf8(bytes: &[u8]) -> String {
    bytes.iter().map(|&byte| char::from(byte)).collect()
}

/// UTF-16 code units to UTF-8; surrogate pairs become one code point, unpaired ones U+FFFD, trailing nulls are dropped.
fn utf16_to_utf8(units: &[u16]) -> String {
    let end = units.iter().rposition(|&unit| unit != 0).map_or(0, |last| last + 1);
    char::decode_utf16(units[..end].iter().copied()).map(|c| c.unwrap_or(char::REPLACEMENT_CHARACTER)).collect()
}

struct ChannelInfo {
    id: i32,
    length: u32, // including the 2 byte compression field
}

/// The layer mask / adjustment layer data of a layer record.
#[derive(Default)]
struct MaskBlock {
    present: bool,
    bounds: Rect,
    default_color: u8,
    flags: u8,
    has_real: bool,
    real_bounds: Rect,
    real_default_color: u8,
    real_flags: u8,
    user_density: Option<u8>,
    user_feather: Option<f64>,
    vector_density: Option<u8>,
    vector_feather: Option<f64>,
}

fn read_rect(reader: &mut Reader) -> Rect {
    let top = reader.i32();
    let left = reader.i32();
    let bottom = reader.i32();
    let right = reader.i32();
    Rect { top, left, bottom, right }
}

fn plausible_rect(rect: &Rect) -> bool {
    let width = i64::from(rect.right) - i64::from(rect.left);
    let height = i64::from(rect.bottom) - i64::from(rect.top);
    (0..=MAX_DIMENSION).contains(&width) && (0..=MAX_DIMENSION).contains(&height)
}

/// The size of the mask parameters: their flag byte plus a density byte or a feather double for each bit.
fn mask_parameters_size(parameters: u8) -> usize {
    1 + if parameters & 0x01 != 0 { 1 } else { 0 }
        + if parameters & 0x02 != 0 { 8 } else { 0 }
        + if parameters & 0x04 != 0 { 1 } else { 0 }
        + if parameters & 0x08 != 0 { 8 } else { 0 }
}

/// 0 bytes = no mask; otherwise rectangle, default color, flags, then optionally the "real" flags, default color and
/// rectangle, then the mask parameters if flag bit 4 is set, or 2 bytes of padding. Photoshop writes the real fields
/// before the parameters, against the specification (psd-tools found the same). Whether real fields are there follows
/// from which layout adds up to the block size; the size alone is ambiguous, two feathers make a block without real
/// fields as long as one with them.
fn parse_mask_block(mut block: Reader) -> MaskBlock {
    let mut mask = MaskBlock::default();
    if block.remaining() < 18 {
        return mask;
    }
    mask.bounds = read_rect(&mut block);
    mask.default_color = block.u8();
    mask.flags = block.u8();
    let has_parameters = mask.flags & 0x10 != 0;

    // the parameter flag byte at the position each layout would put it
    let parameters_at = |skip: usize| {
        let mut probe = block.clone();
        probe.skip(skip);
        probe.u8()
    };
    let rest = block.remaining();
    let fits = |used: usize| used <= rest && rest - used < 4; // a little padding is fine
    let mut has_real = rest >= 18;
    if has_parameters {
        // set parameter flags are part of each candidate: bit 4 promises at least one parameter
        let real_parameters = if rest >= 18 { parameters_at(18) } else { 0 };
        let real_used = 18 + mask_parameters_size(real_parameters);
        let plain_used = mask_parameters_size(parameters_at(0));
        let real_fits = rest >= 18 && real_parameters & 0x0f != 0 && fits(real_used);
        let plain_fits = fits(plain_used);
        has_real = real_fits && (!plain_fits || real_used == rest || plain_used != rest);
    }
    if has_real {
        mask.real_flags = block.u8();
        mask.real_default_color = block.u8();
        mask.real_bounds = read_rect(&mut block);
        mask.has_real = true;
    }
    if has_parameters {
        let parameters = block.u8();
        if parameters & 0x01 != 0 {
            mask.user_density = Some(block.u8());
        }
        if parameters & 0x02 != 0 {
            mask.user_feather = Some(block.f64());
        }
        if parameters & 0x04 != 0 {
            mask.vector_density = Some(block.u8());
        }
        if parameters & 0x08 != 0 {
            mask.vector_feather = Some(block.f64());
        }
    }
    mask.present = !block.failed();
    mask
}

/// A raster mask exists if its channel does: -3 together with the "real" fields, else -2. A layer with only a vector
/// mask has the mask fields but no mask channel. Returns the channel to decode it from.
fn apply_mask_block(block: &MaskBlock, layer: &mut Layer, channels: &[ChannelInfo]) -> i32 {
    let has_channel = |id: i32| channels.iter().any(|channel| channel.id == id);
    if !block.present {
        return NO_MASK_CHANNEL;
    }
    let use_real = block.has_real && has_channel(-3);
    if !use_real && !has_channel(-2) {
        return NO_MASK_CHANNEL;
    }
    let flags = if use_real { block.real_flags } else { block.flags };
    let mask = LayerMask {
        bounds: if use_real { block.real_bounds } else { block.bounds },
        default_color: if use_real { block.real_default_color } else { block.default_color },
        disabled: flags & 0x02 != 0,
        relative_to_layer: flags & 0x01 != 0,
        density: block.user_density,
        feather: block.user_feather,
        values: Vec::new(),
    };
    if !plausible_rect(&mask.bounds) {
        return NO_MASK_CHANNEL;
    }
    layer.mask = Some(mask);
    if use_real { -3 } else { -2 }
}

pub(crate) struct Parser<'a> {
    reader: Reader<'a>,
    options: LoadOptions,
    document: Document,
    palette: [u8; 768],
    composite_has_alpha: bool,
}

impl<'a> Parser<'a> {
    pub(crate) fn new(data: &'a [u8], options: LoadOptions) -> Self {
        Self { reader: Reader::new(data, 0), options, document: Document::default(), palette: [0; 768], composite_has_alpha: false }
    }

    pub(crate) fn parse(mut self) -> Result<Document> {
        self.parse_header()?;
        self.parse_color_mode_data()?;
        self.parse_image_resources()?;
        self.parse_layer_and_mask_information()?;
        self.parse_image_data()?;
        Ok(self.document)
    }

    fn check_pixels(&self, width: i64, height: i64, offset: usize, what: &str) -> Result<()> {
        if (width as u64).saturating_mul(height as u64) > self.options.max_pixels as u64 {
            return fail(ErrorCode::ImageTooLarge, offset, format!("{what}: {width} x {height}"));
        }
        Ok(())
    }

    fn depth(&self) -> i32 {
        self.document.header.depth
    }

    // ---------------------------------------------------------------------------------------------------------------
    // file header section
    // ---------------------------------------------------------------------------------------------------------------

    fn parse_header(&mut self) -> Result<()> {
        // 4 signature '8BPS', 2 version, 6 reserved, 2 channels, 4 height, 4 width, 2 depth, 2 color mode
        let reader = &mut self.reader;
        let signature = reader.u32();
        let file_version = reader.u16();
        reader.skip(6);
        let channels = reader.u16();
        let height = reader.u32();
        let width = reader.u32();
        let depth = reader.u16();
        let mode = reader.u16();
        if reader.failed() {
            // a short file that does not even start like a psd is not truncated, it is something else
            let code = if signature == four_cc(b"8BPS") { ErrorCode::Truncated } else { ErrorCode::NotAPsdFile };
            return fail(code, reader.offset(), "file header");
        }
        if signature != four_cc(b"8BPS") {
            return fail(ErrorCode::NotAPsdFile, 0, "signature is not 8BPS");
        }
        if file_version == 2 {
            return fail(ErrorCode::UnsupportedVersion, 4, "large document format (psb)");
        }
        if file_version != 1 {
            return fail(ErrorCode::Corrupt, 4, format!("version {file_version}"));
        }
        if !(1..=MAX_CHANNELS).contains(&channels) {
            return fail(ErrorCode::Corrupt, 12, format!("{channels} channels"));
        }
        if width < 1 || height < 1 || i64::from(width) > MAX_DIMENSION || i64::from(height) > MAX_DIMENSION {
            return fail(ErrorCode::Corrupt, 14, format!("size {width} x {height}"));
        }

        let color_mode = match mode {
            0 => ColorMode::Bitmap,
            1 => ColorMode::Grayscale,
            2 => ColorMode::Indexed,
            3 => ColorMode::Rgb,
            4 => ColorMode::Cmyk,
            8 => ColorMode::Duotone,
            7 | 9 => return fail(ErrorCode::UnsupportedColorMode, 24, format!("color mode {mode}")),
            _ => return fail(ErrorCode::Corrupt, 24, format!("color mode {mode}")),
        };
        match color_mode {
            ColorMode::Bitmap if depth != 1 => return fail(ErrorCode::UnsupportedDepth, 22, format!("bitmap with depth {depth}")),
            ColorMode::Indexed if depth != 8 => return fail(ErrorCode::UnsupportedDepth, 22, format!("indexed color with depth {depth}")),
            ColorMode::Grayscale | ColorMode::Duotone | ColorMode::Rgb | ColorMode::Cmyk if !matches!(depth, 8 | 16 | 32) => {
                return fail(ErrorCode::UnsupportedDepth, 22, format!("depth {depth}"));
            }
            _ => {}
        }

        let header = &mut self.document.header;
        header.width = width as i32;
        header.height = height as i32;
        header.channels = i32::from(channels);
        header.depth = i32::from(depth);
        header.color_mode = color_mode;

        if self.color_channel_count() > i32::from(channels) {
            return fail(ErrorCode::Corrupt, 12, format!("{channels} channels for color mode {mode}"));
        }
        self.check_pixels(i64::from(width), i64::from(height), 14, "document")
    }

    fn color_channel_count(&self) -> i32 {
        match self.document.header.color_mode {
            ColorMode::Rgb => 3,
            ColorMode::Cmyk => 4,
            _ => 1,
        }
    }

    fn bytes_per_row(&self, width: i32) -> usize {
        let width = width.max(0) as usize;
        match self.depth() {
            1 => width.div_ceil(8),
            depth => width * depth.max(0) as usize / 8,
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // color mode data section: the palette of indexed documents
    // ---------------------------------------------------------------------------------------------------------------

    fn parse_color_mode_data(&mut self) -> Result<()> {
        let length = self.reader.u32() as usize;
        let mut section = self.reader.sub(length);
        check_read(&self.reader, "color mode data")?;
        if self.document.header.color_mode == ColorMode::Indexed {
            // 256 red values, then 256 green, then 256 blue
            let table = section.take(768);
            if table.is_empty() {
                return fail(ErrorCode::Corrupt, section.offset(), "indexed color without a 768 byte palette");
            }
            self.palette.copy_from_slice(table);
        }
        Ok(())
    }

    // ---------------------------------------------------------------------------------------------------------------
    // image resources section: only the saved paths are read
    // ---------------------------------------------------------------------------------------------------------------

    /// The resources are optional, so a malformed block ends the section instead of the load. A section that runs
    /// past the end of the file is still an error.
    fn parse_image_resources(&mut self) -> Result<()> {
        let length = self.reader.u32() as usize;
        let mut section = self.reader.sub(length);
        check_read(&self.reader, "image resources")?;

        // 4 signature, 2 id, pascal name padded to even, 4 size, data padded to even
        while section.remaining() >= 12 {
            let signature = section.u32();
            if ![b"8BIM", b"MeSa", b"PHUT", b"AgHg", b"DCSR"].iter().any(|key| signature == four_cc(key)) {
                break;
            }
            let id = section.u16();
            let name_length = usize::from(section.u8());
            let name = latin1_to_utf8(section.take(name_length));
            section.align(2);
            let size = section.u32() as usize;
            let block = section.sub(size);
            section.align(2);
            if section.failed() {
                break;
            }
            if (2000..=2997).contains(&id) {
                let (subpaths, _) = self.parse_path_records(block);
                self.document.paths.push(Path { id: i32::from(id), name, subpaths });
            }
        }
        Ok(())
    }

    /// The 26 byte path records of a saved path or a vector mask, and whether the initial fill record says the shapes
    /// start from a filled canvas.
    fn parse_path_records(&self, mut block: Reader) -> (Vec<Subpath>, bool) {
        // points are 8.24 fixed point fractions of the document size, vertical first
        let width = self.document.header.width as f32;
        let height = self.document.header.height as f32;
        let read_point = |record: &mut Reader| {
            let y = record.i32() as f32 / 16_777_216.0;
            let x = record.i32() as f32 / 16_777_216.0;
            Point { x: x * width, y: y * height }
        };

        let mut subpaths: Vec<Subpath> = Vec::new();
        let mut starts_filled = false;
        while block.remaining() >= 26 {
            let mut record = block.sub(26);
            let kind = record.u16();
            match kind {
                // closed and open subpath length records
                0 | 3 => subpaths.push(Subpath { closed: kind == 0, knots: Vec::new() }),
                // closed and open knots, linked and unlinked; knots before any length record are malformed and ignored
                1 | 2 | 4 | 5 => {
                    if let Some(subpath) = subpaths.last_mut() {
                        let incoming = read_point(&mut record);
                        let anchor = read_point(&mut record);
                        let outgoing = read_point(&mut record);
                        subpath.knots.push(Knot { incoming, anchor, outgoing, linked: kind == 1 || kind == 4 });
                    }
                }
                // initial fill record: 1 = start with all pixels filled
                8 => starts_filled = record.u16() == 1,
                _ => {} // fill rule and clipboard records
            }
        }
        (subpaths, starts_filled)
    }

    // ---------------------------------------------------------------------------------------------------------------
    // layer and mask information section
    // ---------------------------------------------------------------------------------------------------------------

    fn parse_layer_and_mask_information(&mut self) -> Result<()> {
        let length = self.reader.u32() as usize;
        let mut section = self.reader.sub(length);
        check_read(&self.reader, "layer and mask information")?;
        if length == 0 {
            return Ok(());
        }

        // layer info, with its own length
        let layer_info_length = section.u32() as usize;
        let mut layer_info = section.sub(layer_info_length);
        check_read(&section, "layer info")?;
        if layer_info_length > 0 {
            self.parse_layer_info(&mut layer_info)?;
        }

        // global layer mask info, ignored
        let global_mask_length = section.u32() as usize;
        section.skip(global_mask_length);
        if section.failed() {
            return Ok(()); // nothing after it is required
        }

        // tagged blocks. 16 and 32 bit documents keep their layers here ('Lr16', 'Lr32') and leave the layer info
        // above empty
        while section.remaining() >= 12 {
            let signature = section.u32();
            if signature != four_cc(b"8BIM") && signature != four_cc(b"8B64") {
                break;
            }
            let key = section.u32();
            let block_length = section.u32() as usize;
            let mut block = section.sub(block_length);
            if section.failed() {
                return fail(ErrorCode::Truncated, block.offset(), "tagged block in the layer and mask information");
            }
            section.align(2);
            if [b"Lr16", b"Lr32", b"Layr"].iter().any(|k| key == four_cc(k)) && self.document.layers.is_empty() {
                self.parse_layer_info(&mut block)?;
            }
        }
        Ok(())
    }

    fn parse_layer_info(&mut self, info: &mut Reader) -> Result<()> {
        // a negative count means the first alpha channel of the composite is its transparency
        let count = info.i16();
        check_read(info, "layer count")?;
        self.composite_has_alpha = count < 0;
        let layer_count = i32::from(count).unsigned_abs() as usize;

        // a layer record is at least 34 bytes, so the count cannot exceed what the data could hold
        if layer_count > info.remaining() / 34 {
            return fail(ErrorCode::Corrupt, info.offset(), format!("{layer_count} layers in {} bytes", info.remaining()));
        }

        let mut layers = vec![Layer::default(); layer_count];
        let mut channels: Vec<Vec<ChannelInfo>> = Vec::with_capacity(layer_count);
        let mut mask_channels = vec![NO_MASK_CHANNEL; layer_count];
        for (layer, mask_channel) in layers.iter_mut().zip(mask_channels.iter_mut()) {
            channels.push(self.parse_layer_record(info, layer, mask_channel)?);
        }

        // the channel image data of all layers follows the records, in the same order
        for ((layer, layer_channels), &mask_channel) in layers.iter_mut().zip(&channels).zip(&mask_channels) {
            self.parse_layer_pixels(info, layer, layer_channels, mask_channel)?;
        }
        self.document.layers = layers;
        Ok(())
    }

    fn parse_layer_record(&self, info: &mut Reader, layer: &mut Layer, mask_channel: &mut i32) -> Result<Vec<ChannelInfo>> {
        let record_offset = info.offset();
        layer.bounds = read_rect(info);
        let channel_count = info.u16();
        check_read(info, "layer record")?;

        // 64 bit, so corrupt coordinates like top = -2^31 cannot overflow
        let width = i64::from(layer.bounds.right) - i64::from(layer.bounds.left);
        let height = i64::from(layer.bounds.bottom) - i64::from(layer.bounds.top);
        if width < 0 || height < 0 || width > MAX_DIMENSION || height > MAX_DIMENSION {
            return fail(ErrorCode::Corrupt, record_offset, format!("layer size {width} x {height}"));
        }
        if channel_count > MAX_CHANNELS {
            return fail(ErrorCode::Corrupt, record_offset + 16, format!("{channel_count} layer channels"));
        }

        let channels: Vec<ChannelInfo> =
            (0..channel_count).map(|_| ChannelInfo { id: i32::from(info.i16()), length: info.u32() }).collect();

        let blend_signature = info.u32();
        for character in &mut layer.blend_mode {
            *character = info.u8();
        }
        layer.opacity = info.u8();
        layer.clipping = info.u8() != 0;
        let flags = info.u8();
        info.skip(1); // filler
        let extra_length = info.u32() as usize;
        let mut extra = info.sub(extra_length);
        check_read(info, "layer record")?;
        if blend_signature != four_cc(b"8BIM") {
            return fail(ErrorCode::Corrupt, record_offset, "layer blend mode signature is not 8BIM");
        }
        layer.visible = flags & 0x02 == 0;

        // layers without extra data have no name and no section key
        if extra_length > 0 {
            *mask_channel = self.parse_layer_extra_data(&mut extra, layer, &channels);
        }
        Ok(channels)
    }

    /// Mask data, blending ranges, the name, then tagged blocks. All optional, so a malformed part ends the parsing of
    /// the extra data instead of the load. Returns the raster mask channel.
    fn parse_layer_extra_data(&self, extra: &mut Reader, layer: &mut Layer, channels: &[ChannelInfo]) -> i32 {
        let mask_length = extra.u32() as usize;
        let mask_block = parse_mask_block(extra.sub(mask_length));
        let blending_ranges = extra.u32() as usize;
        extra.skip(blending_ranges);

        // pascal string padded to a multiple of 4 bytes, including the length byte
        let name_length = usize::from(extra.u8());
        let name = extra.take(name_length);
        extra.align(4);
        if extra.failed() {
            return NO_MASK_CHANNEL;
        }
        layer.name = latin1_to_utf8(name);
        let mask_channel = apply_mask_block(&mask_block, layer, channels);

        while extra.remaining() >= 12 {
            let signature = extra.u32();
            if signature != four_cc(b"8BIM") && signature != four_cc(b"8B64") {
                break;
            }
            let key = extra.u32();
            let length = extra.u32() as usize;
            let mut block = extra.sub(length);
            extra.align(2);
            if extra.failed() {
                break;
            }

            // 'lsct' is the section divider; nested groups use 'lsdk' with the same layout
            if key == four_cc(b"lsct") || key == four_cc(b"lsdk") {
                let kind = block.u32();
                if !block.failed() {
                    layer.section_divider = match kind {
                        0 => SectionDivider::Other,
                        1 => SectionDivider::OpenFolder,
                        2 => SectionDivider::ClosedFolder,
                        3 => SectionDivider::BoundingSectionDivider,
                        _ => layer.section_divider,
                    };
                }
            }

            // vector mask: version, flags (invert, not linked, disabled), then path records. 'vsms' is the newer key
            if (key == four_cc(b"vmsk") || key == four_cc(b"vsms")) && layer.vector_mask.is_none() {
                block.u32(); // version, 3
                let flags = block.u32();
                if !block.failed() {
                    let (subpaths, starts_filled) = self.parse_path_records(block.clone());
                    layer.vector_mask = Some(VectorMask {
                        subpaths,
                        inverted: flags & 0x01 != 0,
                        linked: flags & 0x02 == 0,
                        disabled: flags & 0x04 != 0,
                        starts_filled,
                        density: mask_block.vector_density,
                        feather: mask_block.vector_feather,
                    });
                }
            }

            // unicode name: 4 byte count of utf-16 code units, then the code units
            if key == four_cc(b"luni") {
                let unit_count = block.u32() as usize;
                if !block.failed() && unit_count <= block.remaining() / 2 {
                    let units: Vec<u16> = (0..unit_count).map(|_| block.u16()).collect();
                    layer.name = utf16_to_utf8(&units);
                }
            }
        }
        mask_channel
    }

    fn parse_layer_pixels(&self, info: &mut Reader, layer: &mut Layer, channels: &[ChannelInfo], mask_channel: i32) -> Result<()> {
        let width = layer.bounds.width();
        let height = layer.bounds.height();
        let has_pixels = width > 0 && height > 0;
        let load_pixels = self.options.load_layer_pixels;
        if has_pixels && load_pixels {
            self.check_pixels(i64::from(width), i64::from(height), info.offset(), "layer")?;
        }

        let mut planes: Vec<(i32, Vec<u8>)> = Vec::new();
        for channel in channels {
            // the declared length bounds each channel, so a bad channel cannot shift the ones after it
            let mut data = info.sub(channel.length as usize);
            check_read(info, "layer channel data")?;

            // the raster mask has its own rectangle, and holds coverage rather than color
            if channel.id == mask_channel && load_pixels {
                if let Some(mask) = layer.mask.as_mut() {
                    let (mask_width, mask_height) = (mask.bounds.width(), mask.bounds.height());
                    if mask_width > 0 && mask_height > 0 {
                        self.check_pixels(i64::from(mask_width), i64::from(mask_height), data.offset(), "layer mask")?;
                        mask.values = self.decode_channel(&mut data, mask_width, mask_height, SampleEncoding::Linear)?;
                    }
                    continue;
                }
            }

            // the other mask channel (-2 next to a "real" -3) and channels of other color modes are skipped
            let wanted = channel.id >= -1 && channel.id < self.color_channel_count();
            if !has_pixels || !load_pixels || !wanted {
                continue;
            }
            // transparency is coverage, so it stays linear in 32 bit documents like the masks
            let encoding = if channel.id == -1 { SampleEncoding::Linear } else { SampleEncoding::Color };
            planes.push((channel.id, self.decode_channel(&mut data, width, height, encoding)?));
        }

        if has_pixels && load_pixels && !layer.is_section_divider() {
            let find = |id: i32| planes.iter().find(|(plane_id, _)| *plane_id == id).map(|(_, values)| values.as_slice());
            let mut color: [Option<&[u8]>; 4] = [None; 4];
            for (index, slot) in color.iter_mut().enumerate().take(self.color_channel_count() as usize) {
                *slot = find(index as i32);
            }
            layer.image = self.compose(width, height, &color, find(-1));
        }
        Ok(())
    }

    /// Decodes one layer channel, compression field included, into `width * height` 8 bit values.
    fn decode_channel(&self, data: &mut Reader, width: i32, height: i32, encoding: SampleEncoding) -> Result<Vec<u8>> {
        let compression = data.u16();
        check_read(data, "channel compression")?;
        let row_bytes = self.bytes_per_row(width);
        self.check_plausible_size(data, compression, row_bytes, height, 1)?;
        let (columns, rows) = (width as usize, height as usize);
        let mut values = vec![0u8; columns * rows];
        let depth = self.depth();

        match compression {
            0 => {
                let raw = data.take(row_bytes * rows);
                check_read(data, "raw channel data")?;
                for (row, output) in raw.chunks_exact(row_bytes.max(1)).zip(values.chunks_exact_mut(columns.max(1))) {
                    samples_to_8bit(row, depth, output, encoding);
                }
            }
            1 => self.decode_rle_rows(data, width, height, 1, std::slice::from_mut(&mut values), encoding, -1)?,
            2 | 3 => {
                let offset = data.offset();
                let mut raw = vec![0u8; row_bytes * rows];
                if !inflate(data.take(data.remaining()), &mut raw) {
                    return fail(ErrorCode::Corrupt, offset, "zip data does not inflate to the channel size");
                }
                for (row, output) in raw.chunks_exact_mut(row_bytes.max(1)).zip(values.chunks_exact_mut(columns.max(1))) {
                    if compression == 3 {
                        undo_prediction(row, depth, columns);
                    }
                    samples_to_8bit(row, depth, output, encoding);
                }
            }
            _ => return fail(ErrorCode::Corrupt, data.offset() - 2, format!("compression {compression}")),
        }
        Ok(values)
    }

    /// Rejects data that cannot hold the rows it claims, before the output is allocated. Raw rows need their full
    /// size, packbits rows a 2 byte size each plus at least 2 bytes per 128 output bytes.
    fn check_plausible_size(&self, data: &Reader, compression: u16, row_bytes: usize, height: i32, channel_count: i32) -> Result<()> {
        let rows = height.max(0) as u64 * channel_count.max(0) as u64;
        let row_bytes = row_bytes as u64;
        let minimum = match compression {
            0 => rows * row_bytes,
            1 => rows * (2 + 2 * row_bytes.div_ceil(128)),
            // deflate expands at most about 1032 to 1, plus the zlib header and checksum
            2 | 3 => rows * row_bytes / 1032 + 6,
            _ => 0,
        };
        let remaining = data.remaining() as u64;
        if minimum > remaining {
            let code = if compression == 0 { ErrorCode::Truncated } else { ErrorCode::Corrupt };
            return fail(code, data.offset(), format!("{rows} rows need at least {minimum} bytes, {remaining} left"));
        }
        Ok(())
    }

    /// Packbits rows: a table of compressed row sizes for all rows of all channels, then the rows. Channel `c` goes
    /// into `planes[c]`; channels past the planes are decoded and dropped. `linear_channel` holds coverage (the
    /// composite's transparency) and stays linear, -1 for none.
    #[allow(clippy::too_many_arguments)]
    fn decode_rle_rows(
        &self,
        data: &mut Reader,
        width: i32,
        height: i32,
        channel_count: i32,
        planes: &mut [Vec<u8>],
        encoding: SampleEncoding,
        linear_channel: i32,
    ) -> Result<()> {
        let (columns, rows) = (width as usize, height as usize);
        let row_count = rows * channel_count as usize;
        let row_sizes: Vec<usize> = (0..row_count).map(|_| usize::from(data.u16())).collect();
        let total: usize = row_sizes.iter().sum();
        check_read(data, "packbits row sizes")?;

        // packbits needs at least 2 bytes per 128 output bytes; this also bounds the allocation by the file size
        let row_bytes = self.bytes_per_row(width);
        let minimum_row_size = 2 * row_bytes.div_ceil(128);
        if total > data.remaining() || total < minimum_row_size * row_count {
            return fail(ErrorCode::Corrupt, data.offset(), format!("packbits sizes {total} for {} bytes of data", data.remaining()));
        }

        let depth = self.depth();
        let mut samples = vec![0u8; row_bytes];
        let mut discarded = vec![0u8; columns];
        for channel in 0..channel_count as usize {
            for y in 0..rows {
                let offset = data.offset();
                let source = data.take(row_sizes[channel * rows + y]);
                if !unpack_bits(source, &mut samples) {
                    return fail(ErrorCode::Corrupt, offset, "packbits row does not decode to the row size");
                }
                let destination = match planes.get_mut(channel) {
                    Some(plane) => &mut plane[y * columns..(y + 1) * columns],
                    None => &mut discarded[..],
                };
                if depth == 1 {
                    expand_bits(&samples, destination);
                } else {
                    let row_encoding = if channel as i32 == linear_channel { SampleEncoding::Linear } else { encoding };
                    samples_to_8bit(&samples, depth, destination, row_encoding);
                }
            }
        }
        Ok(())
    }

    /// Combines 8 bit planes into 4 byte pixels. Missing color planes count as 0, a missing alpha as opaque.
    fn compose(&self, width: i32, height: i32, color: &[Option<&[u8]>; 4], alpha: Option<&[u8]>) -> Image {
        let pixel_count = width as usize * height as usize;
        let value = |plane: Option<&[u8]>, index: usize| u32::from(plane.map_or(0, |values| values[index]));
        let bgra = self.options.pixel_format == PixelFormat::Bgra8;
        let mut pixels = vec![0u8; pixel_count * 4];

        for (index, pixel) in pixels.chunks_exact_mut(4).enumerate() {
            let (red, green, blue) = match self.document.header.color_mode {
                ColorMode::Rgb => (value(color[0], index), value(color[1], index), value(color[2], index)),
                ColorMode::Cmyk => {
                    // photoshop stores cmyk inverted: 255 is no ink
                    let black = value(color[3], index);
                    let ink = |channel: usize| (value(color[channel], index) * black + 127) / 255;
                    (ink(0), ink(1), ink(2))
                }
                ColorMode::Indexed => {
                    let entry = value(color[0], index) as usize;
                    (u32::from(self.palette[entry]), u32::from(self.palette[256 + entry]), u32::from(self.palette[512 + entry]))
                }
                _ => {
                    // grayscale, duotone (shown as its gray ramp), bitmap
                    let gray = value(color[0], index);
                    (gray, gray, gray)
                }
            };
            pixel[0] = (if bgra { blue } else { red }) as u8;
            pixel[1] = green as u8;
            pixel[2] = (if bgra { red } else { blue }) as u8;
            pixel[3] = alpha.map_or(255, |values| values[index]);
        }
        Image { width, height, pixels }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // image data section: the merged composite
    // ---------------------------------------------------------------------------------------------------------------

    fn parse_image_data(&mut self) -> Result<()> {
        if !self.options.load_composite || self.reader.remaining() == 0 {
            return Ok(()); // some writers leave the composite out
        }

        let header = self.document.header;
        let mut reader = self.reader.clone();
        let compression = reader.u16();
        check_read(&reader, "image data compression")?;

        // the composite has all document channels; we keep the color ones and the transparency, if any
        let row_bytes = self.bytes_per_row(header.width);
        self.check_plausible_size(&reader, compression, row_bytes, header.height, header.channels)?;
        let color_count = self.color_channel_count();
        let alpha_index = if self.composite_has_alpha && header.channels > color_count { color_count } else { -1 };
        let kept = if alpha_index >= 0 { color_count + 1 } else { color_count } as usize;
        let (columns, rows) = (header.width as usize, header.height as usize);
        let mut planes = vec![vec![0u8; columns * rows]; kept];
        let encoding_of = |channel: i32| if channel == alpha_index { SampleEncoding::Linear } else { SampleEncoding::Color };
        let depth = header.depth;

        // one row of the composite into its plane, or nowhere for channels that are not kept
        let store = |planes: &mut Vec<Vec<u8>>, channel: usize, y: usize, row: &[u8]| {
            if let Some(plane) = planes.get_mut(channel) {
                let destination = &mut plane[y * columns..(y + 1) * columns];
                if depth == 1 {
                    expand_bits(row, destination);
                } else {
                    samples_to_8bit(row, depth, destination, encoding_of(channel as i32));
                }
            }
        };

        match compression {
            0 => {
                for channel in 0..header.channels as usize {
                    for y in 0..rows {
                        let row = reader.take(row_bytes);
                        check_read(&reader, "raw image data")?;
                        store(&mut planes, channel, y, row);
                    }
                }
            }
            1 => self.decode_rle_rows(
                &mut reader,
                header.width,
                header.height,
                header.channels,
                &mut planes,
                SampleEncoding::Color,
                alpha_index,
            )?,
            2 | 3 => {
                let offset = reader.offset();
                let mut raw = vec![0u8; row_bytes * rows * header.channels as usize];
                if !inflate(reader.take(reader.remaining()), &mut raw) {
                    return fail(ErrorCode::Corrupt, offset, "zip data does not inflate to the image size");
                }
                for (row_index, row) in raw.chunks_exact_mut(row_bytes.max(1)).enumerate() {
                    if compression == 3 {
                        undo_prediction(row, depth, columns);
                    }
                    store(&mut planes, row_index / rows, row_index % rows, row);
                }
            }
            _ => return fail(ErrorCode::Corrupt, reader.offset() - 2, format!("compression {compression}")),
        }

        let mut color: [Option<&[u8]>; 4] = [None; 4];
        for (slot, plane) in color.iter_mut().zip(&planes).take(color_count as usize) {
            *slot = Some(plane.as_slice());
        }
        let alpha = if alpha_index >= 0 { planes.last().map(Vec::as_slice) } else { None };
        let composite = self.compose(header.width, header.height, &color, alpha);
        self.document.composite = composite;
        self.reader = reader;
        Ok(())
    }
}
