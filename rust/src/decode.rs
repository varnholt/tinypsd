//! Sample level decoding: packbits, photoshop's zip prediction, depth conversion and bitmap expansion.

/// Photoshop's packbits flavor. Writes exactly `destination.len()` bytes when the input is valid; false if the
/// input ends early or would write past the destination.
pub(crate) fn unpack_bits(source: &[u8], destination: &mut [u8]) -> bool {
    let mut read = 0;
    let mut written = 0;
    while written < destination.len() {
        let Some(&header) = source.get(read) else {
            return false;
        };
        read += 1;
        if header < 0x80 {
            // header + 1 literal bytes
            let count = usize::from(header) + 1;
            if read + count > source.len() || written + count > destination.len() {
                return false;
            }
            destination[written..written + count].copy_from_slice(&source[read..read + count]);
            read += count;
            written += count;
        } else if header > 0x80 {
            // the next byte repeated 257 - header times
            let count = 257 - usize::from(header);
            let Some(&value) = source.get(read) else {
                return false;
            };
            if written + count > destination.len() {
                return false;
            }
            read += 1;
            destination[written..written + count].fill(value);
            written += count;
        }
        // 0x80 is a no-op
    }
    true
}

/// Undoes photoshop's "zip with prediction": each row holds differences to the previous sample. 32 bit rows
/// additionally store the bytes of all floats as four planes (all high bytes first).
pub(crate) fn undo_prediction(row: &mut [u8], depth: i32, width: usize) {
    match depth {
        8 => {
            for index in 1..row.len() {
                row[index] = row[index].wrapping_add(row[index - 1]);
            }
        }
        16 => {
            let mut index = 2;
            while index + 1 < row.len() {
                let previous = u16::from_be_bytes([row[index - 2], row[index - 1]]);
                let value = u16::from_be_bytes([row[index], row[index + 1]]).wrapping_add(previous);
                row[index..index + 2].copy_from_slice(&value.to_be_bytes());
                index += 2;
            }
        }
        32 => {
            for index in 1..row.len() {
                row[index] = row[index].wrapping_add(row[index - 1]);
            }
            let planes = row.to_vec();
            for x in 0..width {
                for byte in 0..4 {
                    row[x * 4 + byte] = planes[byte * width + x];
                }
            }
        }
        _ => {}
    }
}

/// How 32 bit float samples map to 8 bit: colors get the srgb curve, coverage like masks and transparency stays linear.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum SampleEncoding {
    Color,
    Linear,
}

/// Photoshop's 32 bit documents are linear light; colors are encoded with the srgb curve like photoshop shows them.
/// On photoshop files this matches the 16 bit version of the same document within one level.
fn float_to_8(value: f32) -> u8 {
    if value.is_nan() || value <= 0.0 {
        return 0; // also catches nan
    }
    if value >= 1.0 {
        return 255;
    }
    let encoded = if value <= 0.003_130_8 { value * 12.92 } else { 1.055 * value.powf(1.0 / 2.4) - 0.055 };
    (encoded * 255.0).round() as u8
}

fn linear_float_to_8(value: f32) -> u8 {
    if value.is_nan() || value <= 0.0 {
        return 0; // also catches nan
    }
    if value >= 1.0 { 255 } else { (value * 255.0).round() as u8 }
}

/// Converts one row of big endian samples to 8 bit.
pub(crate) fn samples_to_8bit(samples: &[u8], depth: i32, output: &mut [u8], encoding: SampleEncoding) {
    match depth {
        8 => {
            let count = output.len().min(samples.len());
            output[..count].copy_from_slice(&samples[..count]);
        }
        16 => {
            for (value, sample) in output.iter_mut().zip(samples.chunks_exact(2)) {
                let wide = u32::from(u16::from_be_bytes([sample[0], sample[1]]));
                *value = ((wide * 255 + 32767) / 65535) as u8;
            }
        }
        32 => {
            for (value, sample) in output.iter_mut().zip(samples.chunks_exact(4)) {
                let float = f32::from_bits(u32::from_be_bytes([sample[0], sample[1], sample[2], sample[3]]));
                *value = match encoding {
                    SampleEncoding::Color => float_to_8(float),
                    SampleEncoding::Linear => linear_float_to_8(float),
                };
            }
        }
        _ => {}
    }
}

/// 1 bit per pixel, 1 = black.
pub(crate) fn expand_bits(bits: &[u8], output: &mut [u8]) {
    for (x, value) in output.iter_mut().enumerate() {
        let bit = bits.get(x / 8).map_or(0, |byte| (byte >> (7 - x % 8)) & 1);
        *value = if bit != 0 { 0 } else { 255 };
    }
}
