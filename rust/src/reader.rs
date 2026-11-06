//! A big endian reader where every read is bounds checked.

/// Reads big endian values from a byte range. A read past the end sets a sticky failure and returns zero, so parsing
/// code can read a group of fields and check once.
#[derive(Clone)]
pub(crate) struct Reader<'a> {
    data: &'a [u8],
    base_offset: usize,
    position: usize,
    failed: bool,
}

impl<'a> Reader<'a> {
    pub(crate) fn new(data: &'a [u8], base_offset: usize) -> Self {
        Self { data, base_offset, position: 0, failed: false }
    }

    /// The position in the file, for error messages.
    pub(crate) fn offset(&self) -> usize {
        self.base_offset + self.position
    }

    pub(crate) fn remaining(&self) -> usize {
        self.data.len() - self.position
    }

    pub(crate) fn failed(&self) -> bool {
        self.failed
    }

    /// The next `count` bytes, or an empty slice (and the failure is set) if there are fewer left.
    pub(crate) fn take(&mut self, count: usize) -> &'a [u8] {
        if self.failed || count > self.remaining() {
            self.failed = true;
            self.position = self.data.len();
            return &[];
        }
        let bytes = &self.data[self.position..self.position + count];
        self.position += count;
        bytes
    }

    pub(crate) fn skip(&mut self, count: usize) {
        self.take(count);
    }

    pub(crate) fn u8(&mut self) -> u8 {
        self.take(1).first().copied().unwrap_or(0)
    }

    pub(crate) fn u16(&mut self) -> u16 {
        Self::big_endian(self.take(2)) as u16
    }

    pub(crate) fn i16(&mut self) -> i16 {
        self.u16() as i16
    }

    pub(crate) fn u32(&mut self) -> u32 {
        Self::big_endian(self.take(4)) as u32
    }

    pub(crate) fn i32(&mut self) -> i32 {
        self.u32() as i32
    }

    pub(crate) fn f64(&mut self) -> f64 {
        f64::from_bits(Self::big_endian(self.take(8)))
    }

    /// A reader limited to the next `count` bytes; this reader moves past them.
    pub(crate) fn sub(&mut self, count: usize) -> Reader<'a> {
        let start = self.offset();
        let bytes = self.take(count);
        Reader { data: bytes, base_offset: start, position: 0, failed: self.failed }
    }

    /// Skips to the next multiple of `alignment`, counted from the start of this reader.
    pub(crate) fn align(&mut self, alignment: usize) {
        let misalignment = self.position % alignment;
        if misalignment != 0 {
            self.skip((alignment - misalignment).min(self.remaining()));
        }
    }

    fn big_endian(bytes: &[u8]) -> u64 {
        bytes.iter().fold(0u64, |value, &byte| (value << 8) | u64::from(byte))
    }
}
