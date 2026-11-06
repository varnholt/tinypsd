//! A zlib (RFC 1950/1951) decoder that follows zlib's puff.c: canonical huffman codes decoded bit by bit, small and
//! with every read and write bounds checked. Photoshop compresses 16 and 32 bit layers with it.

const MAX_BITS: usize = 15;

struct Huffman {
    counts: [u16; MAX_BITS + 1], // number of codes of each length
    symbols: [u16; 288],         // symbols ordered by code
}

impl Huffman {
    fn new() -> Self {
        Self { counts: [0; MAX_BITS + 1], symbols: [0; 288] }
    }

    /// Returns 0 for a complete code, a positive number for an incomplete one and a negative for an over-subscribed one.
    fn build(&mut self, lengths: &[u16]) -> i32 {
        self.counts = [0; MAX_BITS + 1];
        for &length in lengths {
            self.counts[usize::from(length)] += 1;
        }
        if usize::from(self.counts[0]) == lengths.len() {
            return 0; // no codes, complete but useless
        }
        let mut left: i32 = 1;
        for length in 1..=MAX_BITS {
            left <<= 1;
            left -= i32::from(self.counts[length]);
            if left < 0 {
                return left;
            }
        }
        let mut offsets = [0u16; MAX_BITS + 1];
        for length in 1..MAX_BITS {
            offsets[length + 1] = offsets[length] + self.counts[length];
        }
        for (symbol, &length) in lengths.iter().enumerate() {
            if length != 0 {
                let slot = &mut offsets[usize::from(length)];
                self.symbols[usize::from(*slot)] = symbol as u16;
                *slot += 1;
            }
        }
        left
    }
}

struct Inflater<'a, 'b> {
    source: &'a [u8],
    destination: &'b mut [u8],
    position: usize,
    written: usize,
    bit_buffer: u32,
    bit_count: u32,
    failed: bool,
}

/// Inflates a zlib stream into exactly `destination.len()` bytes, checksum included. False for anything else.
pub(crate) fn inflate(source: &[u8], destination: &mut [u8]) -> bool {
    let mut inflater = Inflater { source, destination, position: 0, written: 0, bit_buffer: 0, bit_count: 0, failed: false };
    inflater.run()
}

const LENGTH_BASE: [u16; 29] =
    [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LENGTH_EXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DISTANCE_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
    24577,
];
const DISTANCE_EXTRA: [u8; 30] = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];

impl Inflater<'_, '_> {
    fn run(&mut self) -> bool {
        // zlib header: deflate, 32k window at most, no preset dictionary, header checksum
        let method = self.byte_at(0);
        let flags = self.byte_at(1);
        if self.source.len() < 6 || (method & 0x0f) != 8 || (method >> 4) > 7 || (flags & 0x20) != 0 || ((method << 8) | flags) % 31 != 0 {
            return false;
        }
        self.position = 2;

        let mut last = false;
        while !last {
            last = self.bits(1) == 1;
            let ok = match self.bits(2) {
                0 => self.stored(),
                1 => self.fixed(),
                2 => self.dynamic(),
                _ => false,
            };
            if !ok || self.failed {
                return false;
            }
        }

        // adler-32 of the output follows, byte aligned
        if self.written != self.destination.len() || self.position + 4 > self.source.len() {
            return false;
        }
        let (mut a, mut b) = (1u32, 0u32);
        for &value in self.destination.iter() {
            a = (a + u32::from(value)) % 65521;
            b = (b + a) % 65521;
        }
        let expected = (self.byte_at(self.position) << 24)
            | (self.byte_at(self.position + 1) << 16)
            | (self.byte_at(self.position + 2) << 8)
            | self.byte_at(self.position + 3);
        expected == ((b << 16) | a)
    }

    fn byte_at(&self, index: usize) -> u32 {
        self.source.get(index).map_or(0, |&byte| u32::from(byte))
    }

    /// Least significant bit first.
    fn bits(&mut self, count: u32) -> u32 {
        let mut value = self.bit_buffer;
        while self.bit_count < count {
            if self.position >= self.source.len() {
                self.failed = true;
                return 0;
            }
            value |= self.byte_at(self.position) << self.bit_count;
            self.position += 1;
            self.bit_count += 8;
        }
        self.bit_buffer = value >> count;
        self.bit_count -= count;
        value & ((1u32 << count) - 1)
    }

    fn stored(&mut self) -> bool {
        self.bit_buffer = 0;
        self.bit_count = 0;
        if self.position + 4 > self.source.len() {
            return false;
        }
        let length = (self.byte_at(self.position) | (self.byte_at(self.position + 1) << 8)) as usize;
        let complement = (self.byte_at(self.position + 2) | (self.byte_at(self.position + 3) << 8)) as usize;
        self.position += 4;
        if length != (!complement & 0xffff) || self.position + length > self.source.len() || self.written + length > self.destination.len()
        {
            return false;
        }
        self.destination[self.written..self.written + length].copy_from_slice(&self.source[self.position..self.position + length]);
        self.written += length;
        self.position += length;
        true
    }

    /// The decoded symbol, or None.
    fn decode(&mut self, huffman: &Huffman) -> Option<usize> {
        let (mut code, mut first, mut index) = (0i32, 0i32, 0i32);
        for length in 1..=MAX_BITS {
            code |= self.bits(1) as i32;
            if self.failed {
                return None;
            }
            let count = i32::from(huffman.counts[length]);
            if code - count < first {
                return Some(usize::from(huffman.symbols[(index + (code - first)) as usize]));
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        None
    }

    fn codes(&mut self, lengths: &Huffman, distances: &Huffman) -> bool {
        loop {
            let Some(symbol) = self.decode(lengths) else {
                return false;
            };
            if symbol < 256 {
                if self.written >= self.destination.len() {
                    return false;
                }
                self.destination[self.written] = symbol as u8;
                self.written += 1;
                continue;
            }
            if symbol == 256 {
                return true;
            }

            let length_index = symbol - 257;
            if length_index >= LENGTH_BASE.len() {
                return false;
            }
            let length = u32::from(LENGTH_BASE[length_index]) + self.bits(u32::from(LENGTH_EXTRA[length_index]));
            let distance_symbol = match self.decode(distances) {
                Some(value) if value < 30 => value,
                _ => return false,
            };
            let distance = u32::from(DISTANCE_BASE[distance_symbol]) + self.bits(u32::from(DISTANCE_EXTRA[distance_symbol]));
            let (length, distance) = (length as usize, distance as usize);
            if self.failed || distance > self.written || self.written + length > self.destination.len() {
                return false;
            }
            for _ in 0..length {
                self.destination[self.written] = self.destination[self.written - distance];
                self.written += 1;
            }
        }
    }

    fn fixed(&mut self) -> bool {
        let mut code_lengths = [0u16; 288 + 30];
        code_lengths[..144].fill(8);
        code_lengths[144..256].fill(9);
        code_lengths[256..280].fill(7);
        code_lengths[280..288].fill(8);
        code_lengths[288..].fill(5);
        let mut lengths = Huffman::new();
        let mut distances = Huffman::new();
        lengths.build(&code_lengths[..288]);
        distances.build(&code_lengths[288..]);
        self.codes(&lengths, &distances)
    }

    fn dynamic(&mut self) -> bool {
        const ORDER: [usize; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
        let length_count = self.bits(5) as usize + 257;
        let distance_count = self.bits(5) as usize + 1;
        let code_count = self.bits(4) as usize + 4;
        if self.failed || length_count > 286 || distance_count > 30 {
            return false;
        }

        let mut code_lengths = [0u16; 320];
        for &slot in &ORDER[..code_count] {
            code_lengths[slot] = self.bits(3) as u16;
        }
        let mut code_length_code = Huffman::new();
        if code_length_code.build(&code_lengths[..19]) != 0 {
            return false;
        }

        code_lengths = [0u16; 320];
        let mut index = 0;
        while index < length_count + distance_count {
            let Some(symbol) = self.decode(&code_length_code) else {
                return false;
            };
            if symbol < 16 {
                code_lengths[index] = symbol as u16;
                index += 1;
                continue;
            }
            let (repeated, repeat) = match symbol {
                16 => {
                    if index == 0 {
                        return false;
                    }
                    (code_lengths[index - 1], 3 + self.bits(2) as usize)
                }
                17 => (0, 3 + self.bits(3) as usize),
                _ => (0, 11 + self.bits(7) as usize),
            };
            if self.failed || index + repeat > length_count + distance_count {
                return false;
            }
            code_lengths[index..index + repeat].fill(repeated);
            index += repeat;
        }
        if code_lengths[256] == 0 {
            return false; // no end of block code
        }

        let mut lengths = Huffman::new();
        let mut distances = Huffman::new();
        let length_result = lengths.build(&code_lengths[..length_count]);
        if length_result < 0 || (length_result > 0 && length_count - usize::from(lengths.counts[0]) != 1) {
            return false;
        }
        let distance_result = distances.build(&code_lengths[length_count..length_count + distance_count]);
        if distance_result < 0 || (distance_result > 0 && distance_count - usize::from(distances.counts[0]) != 1) {
            return false;
        }
        self.codes(&lengths, &distances)
    }
}
