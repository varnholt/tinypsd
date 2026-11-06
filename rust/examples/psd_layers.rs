//! Lists the layers of a psd file and writes each one, its mask and the composite as a png.
//!
//!     cargo run --example psd_layers -- file.psd [output directory]

use std::path::Path;
use std::process::ExitCode;

use tinypsd::{Image, Layer, LayerMask, LoadOptions};

fn crc32(bytes: &[u8]) -> u32 {
    !bytes
        .iter()
        .fold(!0u32, |crc, &byte| (0..8).fold(crc ^ u32::from(byte), |crc, _| (crc >> 1) ^ (0xedb8_8320 & (crc & 1).wrapping_neg())))
}

fn chunk(png: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    png.extend_from_slice(&(data.len() as u32).to_be_bytes());
    let body: Vec<u8> = kind.iter().chain(data).copied().collect();
    png.extend_from_slice(&body);
    png.extend_from_slice(&crc32(&body).to_be_bytes());
}

/// An uncompressed rgba png (stored deflate blocks), so the example needs no dependencies.
fn write_png(path: &Path, image: &Image) -> std::io::Result<()> {
    let mut raw = Vec::new();
    for y in 0..image.height {
        raw.push(0); // no filter
        raw.extend_from_slice(image.row(y).unwrap_or_default());
    }
    let mut zlib = vec![0x78, 0x01];
    let blocks: Vec<&[u8]> = if raw.is_empty() { vec![&[]] } else { raw.chunks(65535).collect() };
    for (index, block) in blocks.iter().enumerate() {
        let length = block.len() as u16;
        zlib.push(u8::from(index + 1 == blocks.len()));
        zlib.extend_from_slice(&length.to_le_bytes());
        zlib.extend_from_slice(&(!length).to_le_bytes());
        zlib.extend_from_slice(block);
    }
    let (a, b) = raw.iter().fold((1u32, 0u32), |(a, b), &value| {
        let a = (a + u32::from(value)) % 65521;
        (a, (b + a) % 65521)
    });
    zlib.extend_from_slice(&((b << 16) | a).to_be_bytes());

    let mut header = Vec::new();
    header.extend_from_slice(&(image.width as u32).to_be_bytes());
    header.extend_from_slice(&(image.height as u32).to_be_bytes());
    header.extend_from_slice(&[8, 6, 0, 0, 0]); // 8 bit rgba, deflate, no filter, no interlace
    let mut png = vec![0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1a, b'\n'];
    chunk(&mut png, b"IHDR", &header);
    chunk(&mut png, b"IDAT", &zlib);
    chunk(&mut png, b"IEND", &[]);
    std::fs::write(path, png)
}

/// Keeps layer names usable as file names.
fn file_name(index: usize, name: &str, suffix: &str) -> String {
    let safe: String = name.chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '_' }).collect();
    format!("layer_{index:03}_{safe}{suffix}.png")
}

/// A raster mask as an opaque grayscale image: white shows the layer, black hides it.
fn mask_image(mask: &LayerMask) -> Image {
    let pixels = mask.values.iter().flat_map(|&value| [value, value, value, 255]).collect();
    Image { width: mask.bounds.width(), height: mask.bounds.height(), pixels }
}

fn print_masks(layer: &Layer) {
    if let Some(mask) = &layer.mask {
        println!(
            "        mask          {:4} x {:<4} at ({}, {}) default {:3}{}{}",
            mask.bounds.width(),
            mask.bounds.height(),
            mask.bounds.left,
            mask.bounds.top,
            mask.default_color,
            if mask.disabled { " disabled" } else { "" },
            mask.density.map(|density| format!(" density {density}")).unwrap_or_default()
        );
    }
    if let Some(mask) = &layer.vector_mask {
        let knots: usize = mask.subpaths.iter().map(|subpath| subpath.knots.len()).sum();
        println!(
            "        vector mask   {} subpaths, {} knots{}{}",
            mask.subpaths.len(),
            knots,
            if mask.inverted { " inverted" } else { "" },
            if mask.disabled { " disabled" } else { "" }
        );
    }
}

fn main() -> ExitCode {
    let arguments: Vec<String> = std::env::args().collect();
    if arguments.len() < 2 {
        println!("usage: psd_layers <file.psd> [output directory]");
        return ExitCode::from(2);
    }
    let document = match tinypsd::load_file(&arguments[1], &LoadOptions::default()) {
        Ok(document) => document,
        Err(error) => {
            eprintln!("{}: {error}", arguments[1]);
            return ExitCode::FAILURE;
        }
    };

    let header = &document.header;
    println!("{} x {}, {} bit, {} channels, {} layers", header.width, header.height, header.depth, header.channels, document.layers.len());

    // photoshop stores the layers bottom to top; show them like its layers panel, top first
    for (index, layer) in document.layers.iter().enumerate().rev() {
        let kind = if layer.is_section_divider() {
            "group"
        } else if layer.is_image_layer() {
            "pixels"
        } else {
            "empty"
        };
        println!(
            "  [{index:3}] {kind:<8} {:<30} {:4} x {:<4} at ({}, {}) opacity {:3}{}",
            layer.name,
            layer.bounds.width(),
            layer.bounds.height(),
            layer.bounds.left,
            layer.bounds.top,
            layer.opacity,
            if layer.visible { "" } else { " hidden" }
        );
        print_masks(layer);
    }

    let Some(output) = arguments.get(2).map(Path::new) else {
        return ExitCode::SUCCESS;
    };
    let write = || -> std::io::Result<()> {
        std::fs::create_dir_all(output)?;
        for (index, layer) in document.layers.iter().enumerate() {
            if !layer.image.is_empty() {
                write_png(&output.join(file_name(index, &layer.name, "")), &layer.image)?;
            }
            if let Some(mask) = layer.mask.as_ref().filter(|mask| !mask.values.is_empty()) {
                write_png(&output.join(file_name(index, &layer.name, "_mask")), &mask_image(mask))?;
            }
        }
        if !document.composite.is_empty() {
            write_png(&output.join("composite.png"), &document.composite)?;
        }
        Ok(())
    };
    match write() {
        Ok(()) => {
            println!("wrote the layers to {}", output.display());
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("{}: {error}", output.display());
            ExitCode::FAILURE
        }
    }
}
