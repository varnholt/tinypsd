// tinypsd - a single-header C++23 loader for Adobe Photoshop (.psd) files
// https://github.com/varnholt/tinypsd
//
// MIT License, Copyright (c) Matthias Varnholt
//
// Format specification: https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
//
// usage:
//    auto document = tinypsd::loadFile("image.psd");
//    if (!document)
//    {
//       std::println("{}", tinypsd::describe(document.error()));
//       return;
//    }
//    for (const auto& layer : document->_layers)
//    {
//       // layer._name, layer._bounds, layer._image._pixels (4 bytes per pixel, top row first)
//    }

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinypsd
{

struct Version
{
   int32_t major;
   int32_t minor;
   int32_t patch;
};

inline constexpr Version version{1, 0, 0};

// ----------------------------------------------------------------------------------------------------
// public types
// ----------------------------------------------------------------------------------------------------

/// \brief color mode from the file header.
enum class ColorMode : uint16_t
{
   Bitmap = 0,
   Grayscale = 1,
   Indexed = 2,
   Rgb = 3,
   Cmyk = 4,
   Multichannel = 7,
   Duotone = 8,
   Lab = 9,
};

/// \brief byte order of the decoded pixels in memory, 4 bytes per pixel.
enum class PixelFormat
{
   Rgba8,  //!< r, g, b, a; what OpenGL, Vulkan and most image libraries expect
   Bgra8,  //!< b, g, r, a; what Direct3D and Windows bitmaps expect
};

enum class ErrorCode
{
   CannotOpenFile,        //!< the file does not exist or cannot be read
   NotAPsdFile,           //!< the signature is not '8BPS'
   UnsupportedVersion,    //!< version 2 is the large document format (.psb), which is not supported
   UnsupportedColorMode,  //!< Lab and multichannel documents
   UnsupportedDepth,      //!< a bit depth the color mode does not allow
   ImageTooLarge,         //!< more pixels than LoadOptions::_max_pixels
   Truncated,             //!< the data ends before a section does
   Corrupt,               //!< a value is out of its valid range
};

struct Error
{
   ErrorCode _code{ErrorCode::Corrupt};
   std::size_t _offset{0};  //!< byte offset in the file where the problem was found
   std::string _detail;     //!< what exactly was wrong
};

[[nodiscard]] constexpr std::string_view describe(ErrorCode code)
{
   switch (code)
   {
      case ErrorCode::CannotOpenFile:
         return "cannot open file";
      case ErrorCode::NotAPsdFile:
         return "not a psd file";
      case ErrorCode::UnsupportedVersion:
         return "unsupported version (psb is not supported)";
      case ErrorCode::UnsupportedColorMode:
         return "unsupported color mode";
      case ErrorCode::UnsupportedDepth:
         return "unsupported bit depth";
      case ErrorCode::ImageTooLarge:
         return "image too large";
      case ErrorCode::Truncated:
         return "file is truncated";
      case ErrorCode::Corrupt:
         return "file is corrupt";
   }
   return "unknown error";
}

/// \brief e.g. "file is truncated at byte 1234: layer channel data".
[[nodiscard]] inline std::string describe(const Error& error)
{
   auto text = std::string{describe(error._code)};
   if (error._code != ErrorCode::CannotOpenFile)
   {
      text += " at byte " + std::to_string(error._offset);
   }
   if (!error._detail.empty())
   {
      text += ": " + error._detail;
   }
   return text;
}

/// \brief group markers: photoshop stores a group as an open or closed folder layer above its content and a
///        bounding divider below it.
enum class SectionDivider : int32_t
{
   None = -1,  //!< an ordinary layer
   Other = 0,  //!< any other layer type that carries the section key
   OpenFolder = 1,
   ClosedFolder = 2,
   BoundingSectionDivider = 3,
};

/// \brief decoded 8 bit pixels, 4 bytes per pixel in the requested PixelFormat, top row first, no padding.
struct Image
{
   int32_t _width{0};
   int32_t _height{0};
   std::vector<uint8_t> _pixels;

   [[nodiscard]] bool empty() const
   {
      return _pixels.empty();
   }

   [[nodiscard]] std::span<const uint8_t> row(int32_t y) const
   {
      const auto row_size = static_cast<std::size_t>(_width) * 4;
      return std::span{_pixels}.subspan(static_cast<std::size_t>(y) * row_size, row_size);
   }
};

/// \brief layer rectangle in document pixels; right and bottom are exclusive.
struct Rect
{
   int32_t _top{0};
   int32_t _left{0};
   int32_t _bottom{0};
   int32_t _right{0};

   [[nodiscard]] int32_t width() const
   {
      return _right - _left;
   }

   [[nodiscard]] int32_t height() const
   {
      return _bottom - _top;
   }
};

struct Point
{
   float _x{0.0f};  //!< in document pixels
   float _y{0.0f};
};

/// \brief a bezier knot: the anchor and its two control points.
struct Knot
{
   Point _in;  //!< control point of the curve arriving at the anchor
   Point _anchor;
   Point _out;  //!< control point of the curve leaving the anchor
   bool _linked{false};
};

struct Subpath
{
   bool _closed{false};
   std::vector<Knot> _knots;
};

/// \brief a raster layer mask: 8 bit coverage that hides the layer where it is dark.
struct LayerMask
{
   Rect _bounds;                     //!< in document pixels; independent of the layer bounds
   uint8_t _default_color{0};        //!< the value outside _bounds: 0 hides the layer there, 255 shows it
   bool _disabled{false};            //!< switched off in photoshop; the layer shows unmasked
   bool _relative_to_layer{false};   //!< photoshop's "position relative to layer" flag
   std::optional<uint8_t> _density;  //!< masks panel density, 255 = full effect; absent if never changed
   std::optional<double> _feather;   //!< masks panel feather radius in pixels; absent if never changed
   std::vector<uint8_t> _values;     //!< _bounds sized, top row first, 255 = shown; empty if pixels were not loaded

   /// \return the mask value at a document pixel, the default color outside the bounds
   [[nodiscard]] uint8_t valueAt(int32_t x, int32_t y) const
   {
      if (_values.empty() || x < _bounds._left || x >= _bounds._right || y < _bounds._top || y >= _bounds._bottom)
      {
         return _default_color;
      }
      const auto index = static_cast<std::size_t>(y - _bounds._top) * static_cast<std::size_t>(_bounds.width()) +
                         static_cast<std::size_t>(x - _bounds._left);
      return _values[index];
   }
};

/// \brief a vector mask: bezier shapes that clip the layer, e.g. drawn with the pen tool or from a shape layer.
struct VectorMask
{
   std::vector<Subpath> _subpaths;  //!< in document pixels
   bool _inverted{false};
   bool _linked{true};  //!< moves with the layer
   bool _disabled{false};
   bool _starts_filled{false};       //!< photoshop's initial fill record: the shapes start from a filled canvas
   std::optional<uint8_t> _density;  //!< masks panel density, 255 = full effect; absent if never changed
   std::optional<double> _feather;   //!< masks panel feather radius in pixels; absent if never changed
};

struct Layer
{
   std::string _name;  //!< utf-8; the unicode name if the file stores one, else the legacy name
   Rect _bounds;
   uint8_t _opacity{255};
   bool _visible{true};
   bool _clipping{false};                                //!< clipped to the layer below
   std::array<char, 4> _blend_mode{'n', 'o', 'r', 'm'};  //!< photoshop key, e.g. "norm", "mul ", "scrn"
   SectionDivider _section_divider{SectionDivider::None};
   Image _image;                            //!< the layer pixels, _bounds sized; empty for group markers, empty layers or if not loaded
   std::optional<LayerMask> _mask;          //!< the raster mask, if the layer has one
   std::optional<VectorMask> _vector_mask;  //!< the vector mask, if the layer has one

   /// \brief a group marker (folder or bounding divider) rather than content.
   [[nodiscard]] bool isSectionDivider() const
   {
      return _section_divider != SectionDivider::None;
   }

   /// \brief an ordinary layer with at least one pixel.
   [[nodiscard]] bool isImageLayer() const
   {
      return !isSectionDivider() && _bounds.width() > 0 && _bounds.height() > 0;
   }
};

/// \brief a saved path from the image resources (the paths panel), e.g. a clipping path.
struct Path
{
   int32_t _id{0};  //!< image resource id, 2000 to 2997
   std::string _name;
   std::vector<Subpath> _subpaths;
};

struct Header
{
   int32_t _width{0};
   int32_t _height{0};
   int32_t _channels{0};
   int32_t _depth{0};  //!< bits per channel: 1, 8, 16 or 32
   ColorMode _color_mode{ColorMode::Rgb};
};

struct LoadOptions
{
   PixelFormat _pixel_format{PixelFormat::Rgba8};
   bool _load_layer_pixels{true};                  //!< false reads only names, bounds and flags, which is much faster
   bool _load_composite{true};                     //!< the merged image photoshop stores at the end of the file
   std::size_t _max_pixels{std::size_t{1} << 26};  //!< per image; guards against huge allocations, 64 megapixels
};

struct Document
{
   Header _header;
   std::vector<Layer> _layers;  //!< bottom to top, the order photoshop stores them in
   Image _composite;            //!< the merged image; empty if the file has none or it was not loaded
   std::vector<Path> _paths;

   [[nodiscard]] int32_t width() const
   {
      return _header._width;
   }

   [[nodiscard]] int32_t height() const
   {
      return _header._height;
   }

   /// \return the first layer with this name, or nullptr
   [[nodiscard]] const Layer* findLayer(std::string_view name) const
   {
      const auto it = std::ranges::find_if(_layers, [&](const Layer& layer) { return layer._name == name; });
      return it == _layers.end() ? nullptr : &*it;
   }
};

/// \brief parses a psd file that is already in memory, e.g. from an archive.
[[nodiscard]] std::expected<Document, Error> load(std::span<const std::byte> data, const LoadOptions& options = {});

/// \brief reads and parses a psd file.
[[nodiscard]] std::expected<Document, Error> loadFile(const std::filesystem::path& path, const LoadOptions& options = {});

// ----------------------------------------------------------------------------------------------------
// implementation
// ----------------------------------------------------------------------------------------------------

namespace detail
{

inline constexpr int32_t max_dimension = 30'000;  // psd limit; psb allows 300,000
inline constexpr int32_t max_channels = 56;

[[nodiscard]] constexpr uint32_t fourCc(const char (&text)[5])
{
   return (static_cast<uint32_t>(static_cast<uint8_t>(text[0])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(text[3]));
}

/// \brief big endian reader over a byte range; every read is bounds checked. a failed read sets a sticky error
///        and returns zero, so parsing code can read a group of fields and check once.
class Reader
{
public:
   Reader(std::span<const std::byte> data, std::size_t base_offset) : _data(data), _base_offset(base_offset)
   {
   }

   [[nodiscard]] std::size_t offset() const
   {
      return _base_offset + _position;
   }

   [[nodiscard]] std::size_t remaining() const
   {
      return _data.size() - _position;
   }

   [[nodiscard]] bool failed() const
   {
      return _failed;
   }

   [[nodiscard]] uint8_t u8()
   {
      const auto bytes = take(1);
      return bytes.empty() ? uint8_t{0} : std::to_integer<uint8_t>(bytes[0]);
   }

   [[nodiscard]] uint16_t u16()
   {
      return static_cast<uint16_t>(bigEndian(take(2)));
   }

   [[nodiscard]] int16_t i16()
   {
      return std::bit_cast<int16_t>(u16());
   }

   [[nodiscard]] uint32_t u32()
   {
      return static_cast<uint32_t>(bigEndian(take(4)));
   }

   [[nodiscard]] int32_t i32()
   {
      return std::bit_cast<int32_t>(u32());
   }

   [[nodiscard]] double f64()
   {
      return std::bit_cast<double>(bigEndian(take(8)));
   }

   /// \return the next count bytes, or an empty span (and the error is set) if there are fewer left
   [[nodiscard]] std::span<const std::byte> take(std::size_t count)
   {
      if (_failed || count > remaining())
      {
         _failed = true;
         _position = _data.size();
         return {};
      }
      const auto bytes = _data.subspan(_position, count);
      _position += count;
      return bytes;
   }

   void skip(std::size_t count)
   {
      (void)take(count);
   }

   /// \brief a reader limited to the next count bytes; this reader moves past them.
   [[nodiscard]] Reader sub(std::size_t count)
   {
      const auto start = offset();
      const auto bytes = take(count);
      Reader result{bytes, start};
      result._failed = _failed;
      return result;
   }

   /// \brief skips to the next multiple of alignment, counted from the start of this reader.
   void align(std::size_t alignment)
   {
      const auto misalignment = _position % alignment;
      if (misalignment != 0)
      {
         skip(std::min(alignment - misalignment, remaining()));
      }
   }

private:
   [[nodiscard]] static uint64_t bigEndian(std::span<const std::byte> bytes)
   {
      uint64_t value = 0;
      for (const auto byte : bytes)
      {
         value = (value << 8) | std::to_integer<uint64_t>(byte);
      }
      return value;
   }

   std::span<const std::byte> _data;
   std::size_t _base_offset{0};
   std::size_t _position{0};
   bool _failed{false};
};

/// \brief appends one unicode code point as utf-8.
inline void appendUtf8(std::string& text, uint32_t code_point)
{
   if (code_point < 0x80)
   {
      text += static_cast<char>(code_point);
   }
   else if (code_point < 0x800)
   {
      text += static_cast<char>(0xc0 | (code_point >> 6));
      text += static_cast<char>(0x80 | (code_point & 0x3f));
   }
   else if (code_point < 0x10000)
   {
      text += static_cast<char>(0xe0 | (code_point >> 12));
      text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
      text += static_cast<char>(0x80 | (code_point & 0x3f));
   }
   else
   {
      text += static_cast<char>(0xf0 | (code_point >> 18));
      text += static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
      text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
      text += static_cast<char>(0x80 | (code_point & 0x3f));
   }
}

/// \brief legacy pascal strings are in the system code page; latin-1 is the closest portable guess.
inline std::string latin1ToUtf8(std::span<const std::byte> bytes)
{
   std::string text;
   text.reserve(bytes.size());
   for (const auto byte : bytes)
   {
      appendUtf8(text, std::to_integer<uint32_t>(byte));
   }
   return text;
}

/// \brief utf-16 code units to utf-8; surrogate pairs become one code point, trailing nulls are dropped.
inline std::string utf16ToUtf8(std::span<const uint16_t> units)
{
   while (!units.empty() && units.back() == 0)
   {
      units = units.first(units.size() - 1);
   }
   std::string text;
   for (std::size_t index = 0; index < units.size(); ++index)
   {
      uint32_t code_point = units[index];
      const auto is_high_surrogate = code_point >= 0xd800 && code_point < 0xdc00;
      if (is_high_surrogate && index + 1 < units.size() && units[index + 1] >= 0xdc00 && units[index + 1] < 0xe000)
      {
         code_point = 0x10000 + ((code_point - 0xd800) << 10) + (units[index + 1] - 0xdc00u);
         ++index;
      }
      else if (code_point >= 0xd800 && code_point < 0xe000)
      {
         code_point = 0xfffd;  // unpaired surrogate
      }
      appendUtf8(text, code_point);
   }
   return text;
}

/// \brief photoshop's packbits flavor. writes exactly destination.size() bytes when the input is valid.
/// \return false if the input ends early or would write past the destination
[[nodiscard]] inline bool unpackBits(std::span<const std::byte> source, std::span<uint8_t> destination)
{
   std::size_t read = 0;
   std::size_t written = 0;
   while (written < destination.size())
   {
      if (read >= source.size())
      {
         return false;
      }
      const auto header = std::to_integer<uint8_t>(source[read++]);
      if (header < 0x80)
      {
         // header + 1 literal bytes
         const auto count = static_cast<std::size_t>(header) + 1;
         if (read + count > source.size() || written + count > destination.size())
         {
            return false;
         }
         for (std::size_t index = 0; index < count; ++index)
         {
            destination[written++] = std::to_integer<uint8_t>(source[read++]);
         }
      }
      else if (header > 0x80)
      {
         // the next byte repeated 257 - header times
         const auto count = 257 - static_cast<std::size_t>(header);
         if (read >= source.size() || written + count > destination.size())
         {
            return false;
         }
         const auto value = std::to_integer<uint8_t>(source[read++]);
         std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(written), count, value);
         written += count;
      }
      // 0x80 is a no-op
   }
   return true;
}

/// \brief inflates a zlib stream (rfc 1950/1951) into exactly destination.size() bytes. follows zlib's puff.c:
///        canonical huffman codes decoded bit by bit, small and with every read and write bounds checked.
class Inflater
{
public:
   explicit Inflater(std::span<const std::byte> source) : _source(source)
   {
   }

   [[nodiscard]] bool run(std::span<uint8_t> destination)
   {
      _destination = destination;

      // zlib header: deflate, 32k window at most, no preset dictionary, header checksum
      const auto method = byteAt(0);
      const auto flags = byteAt(1);
      if (_source.size() < 6 || (method & 0x0f) != 8 || (method >> 4) > 7 || (flags & 0x20) != 0 || ((method << 8) | flags) % 31 != 0)
      {
         return false;
      }
      _position = 2;

      auto last = false;
      while (!last)
      {
         last = bits(1) == 1;
         const auto type = bits(2);
         auto ok = false;
         switch (type)
         {
            case 0:
               ok = stored();
               break;
            case 1:
               ok = fixed();
               break;
            case 2:
               ok = dynamic();
               break;
            default:
               break;
         }
         if (!ok || _failed)
         {
            return false;
         }
      }

      // adler-32 of the output follows, byte aligned
      if (_written != destination.size() || _position + 4 > _source.size())
      {
         return false;
      }
      uint32_t a = 1;
      uint32_t b = 0;
      for (const auto value : destination)
      {
         a = (a + value) % 65521;
         b = (b + a) % 65521;
      }
      const auto expected = (static_cast<uint32_t>(byteAt(_position)) << 24) | (static_cast<uint32_t>(byteAt(_position + 1)) << 16) |
                            (static_cast<uint32_t>(byteAt(_position + 2)) << 8) | static_cast<uint32_t>(byteAt(_position + 3));
      return expected == ((b << 16) | a);
   }

private:
   static constexpr int32_t max_bits = 15;

   struct Huffman
   {
      std::array<uint16_t, max_bits + 1> _counts{};  //!< number of codes of each length
      std::array<uint16_t, 288> _symbols{};          //!< symbols ordered by code
   };

   [[nodiscard]] uint32_t byteAt(std::size_t index) const
   {
      return index < _source.size() ? std::to_integer<uint32_t>(_source[index]) : 0u;
   }

   /// \brief least significant bit first.
   [[nodiscard]] uint32_t bits(int32_t count)
   {
      auto value = _bit_buffer;
      while (_bit_count < count)
      {
         if (_position >= _source.size())
         {
            _failed = true;
            return 0;
         }
         value |= byteAt(_position++) << _bit_count;
         _bit_count += 8;
      }
      _bit_buffer = value >> count;
      _bit_count -= count;
      return value & ((1u << count) - 1);
   }

   [[nodiscard]] bool stored()
   {
      _bit_buffer = 0;
      _bit_count = 0;
      if (_position + 4 > _source.size())
      {
         return false;
      }
      const auto length = byteAt(_position) | (byteAt(_position + 1) << 8);
      const auto complement = byteAt(_position + 2) | (byteAt(_position + 3) << 8);
      _position += 4;
      if (length != (~complement & 0xffff) || _position + length > _source.size() || _written + length > _destination.size())
      {
         return false;
      }
      for (uint32_t index = 0; index < length; ++index)
      {
         _destination[_written++] = static_cast<uint8_t>(byteAt(_position++));
      }
      return true;
   }

   /// \return 0 for a complete code, a positive number for an incomplete one, a negative for an over-subscribed one
   [[nodiscard]] static int32_t build(Huffman& huffman, std::span<const uint16_t> lengths)
   {
      huffman._counts.fill(0);
      for (const auto length : lengths)
      {
         ++huffman._counts[length];
      }
      if (huffman._counts[0] == lengths.size())
      {
         return 0;  // no codes, complete but useless
      }
      auto left = 1;
      for (auto length = 1; length <= max_bits; ++length)
      {
         left <<= 1;
         left -= huffman._counts[static_cast<std::size_t>(length)];
         if (left < 0)
         {
            return left;
         }
      }
      std::array<uint16_t, max_bits + 1> offsets{};
      for (auto length = 1; length < max_bits; ++length)
      {
         offsets[static_cast<std::size_t>(length) + 1] =
            static_cast<uint16_t>(offsets[static_cast<std::size_t>(length)] + huffman._counts[static_cast<std::size_t>(length)]);
      }
      for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
      {
         if (lengths[symbol] != 0)
         {
            huffman._symbols[offsets[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
         }
      }
      return left;
   }

   /// \return the decoded symbol, or -1
   [[nodiscard]] int32_t decode(const Huffman& huffman)
   {
      auto code = 0;
      auto first = 0;
      auto index = 0;
      for (auto length = 1; length <= max_bits; ++length)
      {
         code |= static_cast<int32_t>(bits(1));
         if (_failed)
         {
            return -1;
         }
         const auto count = static_cast<int32_t>(huffman._counts[static_cast<std::size_t>(length)]);
         if (code - count < first)
         {
            return huffman._symbols[static_cast<std::size_t>(index + (code - first))];
         }
         index += count;
         first += count;
         first <<= 1;
         code <<= 1;
      }
      return -1;
   }

   [[nodiscard]] bool codes(const Huffman& lengths, const Huffman& distances)
   {
      static constexpr std::array<uint16_t, 29> length_base{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
      static constexpr std::array<uint8_t, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
      static constexpr std::array<uint16_t, 30> distance_base{1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                              33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                              1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
      static constexpr std::array<uint8_t, 30> distance_extra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
      while (true)
      {
         const auto symbol = decode(lengths);
         if (symbol < 0)
         {
            return false;
         }
         if (symbol < 256)
         {
            if (_written >= _destination.size())
            {
               return false;
            }
            _destination[_written++] = static_cast<uint8_t>(symbol);
            continue;
         }
         if (symbol == 256)
         {
            return true;
         }

         const auto length_index = static_cast<std::size_t>(symbol - 257);
         if (length_index >= length_base.size())
         {
            return false;
         }
         const auto length = length_base[length_index] + bits(length_extra[length_index]);
         const auto distance_symbol = decode(distances);
         if (distance_symbol < 0 || distance_symbol >= 30)
         {
            return false;
         }
         const auto distance =
            distance_base[static_cast<std::size_t>(distance_symbol)] + bits(distance_extra[static_cast<std::size_t>(distance_symbol)]);
         if (_failed || distance > _written || _written + length > _destination.size())
         {
            return false;
         }
         for (uint32_t index = 0; index < length; ++index)
         {
            _destination[_written] = _destination[_written - distance];
            ++_written;
         }
      }
   }

   [[nodiscard]] bool fixed()
   {
      std::array<uint16_t, 288 + 30> code_lengths{};
      std::fill_n(code_lengths.begin(), 144, uint16_t{8});
      std::fill_n(code_lengths.begin() + 144, 112, uint16_t{9});
      std::fill_n(code_lengths.begin() + 256, 24, uint16_t{7});
      std::fill_n(code_lengths.begin() + 280, 8, uint16_t{8});
      std::fill_n(code_lengths.begin() + 288, 30, uint16_t{5});
      Huffman lengths;
      Huffman distances;
      (void)build(lengths, std::span{code_lengths}.first(288));
      (void)build(distances, std::span{code_lengths}.subspan(288, 30));
      return codes(lengths, distances);
   }

   [[nodiscard]] bool dynamic()
   {
      static constexpr std::array<uint8_t, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      const auto length_count = static_cast<std::size_t>(bits(5)) + 257;
      const auto distance_count = static_cast<std::size_t>(bits(5)) + 1;
      const auto code_count = static_cast<std::size_t>(bits(4)) + 4;
      if (_failed || length_count > 286 || distance_count > 30)
      {
         return false;
      }

      std::array<uint16_t, 320> code_lengths{};
      for (std::size_t index = 0; index < code_count; ++index)
      {
         code_lengths[order[index]] = static_cast<uint16_t>(bits(3));
      }
      Huffman code_length_code;
      if (build(code_length_code, std::span{code_lengths}.first(19)) != 0)
      {
         return false;
      }

      code_lengths.fill(0);
      std::size_t index = 0;
      while (index < length_count + distance_count)
      {
         auto symbol = decode(code_length_code);
         if (symbol < 0)
         {
            return false;
         }
         if (symbol < 16)
         {
            code_lengths[index++] = static_cast<uint16_t>(symbol);
            continue;
         }
         uint16_t repeated = 0;
         std::size_t repeat = 0;
         if (symbol == 16)
         {
            if (index == 0)
            {
               return false;
            }
            repeated = code_lengths[index - 1];
            repeat = 3 + bits(2);
         }
         else if (symbol == 17)
         {
            repeat = 3 + bits(3);
         }
         else
         {
            repeat = 11 + bits(7);
         }
         if (_failed || index + repeat > length_count + distance_count)
         {
            return false;
         }
         std::fill_n(code_lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat, repeated);
         index += repeat;
      }
      if (code_lengths[256] == 0)
      {
         return false;  // no end of block code
      }

      Huffman lengths;
      Huffman distances;
      const auto length_result = build(lengths, std::span{code_lengths}.first(length_count));
      if (length_result < 0 || (length_result > 0 && length_count - lengths._counts[0] != 1))
      {
         return false;
      }
      const auto distance_result = build(distances, std::span{code_lengths}.subspan(length_count, distance_count));
      if (distance_result < 0 || (distance_result > 0 && distance_count - distances._counts[0] != 1))
      {
         return false;
      }
      return codes(lengths, distances);
   }

   std::span<const std::byte> _source;
   std::span<uint8_t> _destination;
   std::size_t _position{0};
   std::size_t _written{0};
   uint32_t _bit_buffer{0};
   int32_t _bit_count{0};
   bool _failed{false};
};

/// \brief undoes photoshop's "zip with prediction": each row holds differences to the previous sample. 32 bit
///        rows additionally store the bytes of all floats as four planes (all high bytes first).
inline void undoPrediction(std::span<uint8_t> row, int32_t depth, int32_t width)
{
   if (depth == 8)
   {
      for (std::size_t index = 1; index < row.size(); ++index)
      {
         row[index] = static_cast<uint8_t>(row[index] + row[index - 1]);
      }
   }
   else if (depth == 16)
   {
      for (std::size_t index = 2; index + 1 < row.size(); index += 2)
      {
         const auto previous = static_cast<uint32_t>((row[index - 2] << 8) | row[index - 1]);
         const auto value = static_cast<uint32_t>((row[index] << 8) | row[index + 1]) + previous;
         row[index] = static_cast<uint8_t>(value >> 8);
         row[index + 1] = static_cast<uint8_t>(value);
      }
   }
   else if (depth == 32)
   {
      for (std::size_t index = 1; index < row.size(); ++index)
      {
         row[index] = static_cast<uint8_t>(row[index] + row[index - 1]);
      }
      const std::vector<uint8_t> planes(row.begin(), row.end());
      const auto count = static_cast<std::size_t>(width);
      for (std::size_t x = 0; x < count; ++x)
      {
         for (std::size_t byte = 0; byte < 4; ++byte)
         {
            row[x * 4 + byte] = planes[byte * count + x];
         }
      }
   }
}

/// \brief photoshop's 32 bit documents are linear light; they are encoded with the srgb curve like photoshop
///        shows them. on photoshop files this matches the 16 bit version of the same document within one level.
[[nodiscard]] inline uint8_t floatTo8(float value)
{
   if (!(value > 0.0f))  // also catches nan
   {
      return 0;
   }
   if (value >= 1.0f)
   {
      return 255;
   }
   const auto encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
   return static_cast<uint8_t>(std::lround(encoded * 255.0f));
}

/// \brief how 32 bit float samples map to 8 bit: colors get the srgb curve, coverage like masks stays linear.
enum class SampleEncoding : int32_t
{
   Color,
   Linear,
};

[[nodiscard]] inline uint8_t linearFloatTo8(float value)
{
   if (!(value > 0.0f))  // also catches nan
   {
      return 0;
   }
   return value >= 1.0f ? uint8_t{255} : static_cast<uint8_t>(std::lround(value * 255.0f));
}

/// \brief converts one row of big endian samples to 8 bit.
inline void
samplesTo8Bit(std::span<const uint8_t> samples, int32_t depth, std::span<uint8_t> output, SampleEncoding encoding = SampleEncoding::Color)
{
   switch (depth)
   {
      case 8:
         std::ranges::copy(samples.first(output.size()), output.begin());
         break;
      case 16:
         for (std::size_t index = 0; index < output.size(); ++index)
         {
            const auto value = static_cast<uint32_t>((samples[index * 2] << 8) | samples[index * 2 + 1]);
            output[index] = static_cast<uint8_t>((value * 255 + 32767) / 65535);
         }
         break;
      case 32:
         for (std::size_t index = 0; index < output.size(); ++index)
         {
            const auto bits = (static_cast<uint32_t>(samples[index * 4]) << 24) | (static_cast<uint32_t>(samples[index * 4 + 1]) << 16) |
                              (static_cast<uint32_t>(samples[index * 4 + 2]) << 8) | static_cast<uint32_t>(samples[index * 4 + 3]);
            const auto value = std::bit_cast<float>(bits);
            output[index] = encoding == SampleEncoding::Color ? floatTo8(value) : linearFloatTo8(value);
         }
         break;
      default:
         break;
   }
}

/// \brief one channel converted to 8 bit, width * height bytes.
struct Plane
{
   int32_t _id{0};
   std::vector<uint8_t> _values;
};

class Parser
{
public:
   Parser(std::span<const std::byte> data, const LoadOptions& options) : _reader(data, 0), _options(options)
   {
   }

   std::expected<Document, Error> parse()
   {
      if (!parseHeader() || !parseColorModeData() || !parseImageResources() || !parseLayerAndMaskInformation() || !parseImageData())
      {
         return std::unexpected{_error};
      }
      return std::move(_document);
   }

private:
   // ------------------------------------------------------------------------------------------------
   // errors
   // ------------------------------------------------------------------------------------------------

   bool fail(ErrorCode code, std::size_t offset, std::string detail)
   {
      _error = {code, offset, std::move(detail)};
      return false;
   }

   /// \return false with a Truncated error if the reader ran out of data
   bool checkRead(const Reader& reader, std::string_view what)
   {
      if (reader.failed())
      {
         return fail(ErrorCode::Truncated, reader.offset(), std::string{what});
      }
      return true;
   }

   bool checkPixels(int64_t width, int64_t height, std::size_t offset, std::string_view what)
   {
      if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > _options._max_pixels)
      {
         return fail(ErrorCode::ImageTooLarge, offset, std::format("{}: {} x {}", what, width, height));
      }
      return true;
   }

   // ------------------------------------------------------------------------------------------------
   // file header section
   // ------------------------------------------------------------------------------------------------

   bool parseHeader()
   {
      // 4 signature '8BPS', 2 version, 6 reserved, 2 channels, 4 height, 4 width, 2 depth, 2 color mode
      const auto signature = _reader.u32();
      const auto file_version = _reader.u16();
      _reader.skip(6);
      const auto channels = _reader.u16();
      const auto height = _reader.u32();
      const auto width = _reader.u32();
      const auto depth = _reader.u16();
      const auto mode = _reader.u16();
      if (_reader.failed())
      {
         // a short file that does not even start like a psd is not truncated, it is something else
         return fail(signature == fourCc("8BPS") ? ErrorCode::Truncated : ErrorCode::NotAPsdFile, _reader.offset(), "file header");
      }
      if (signature != fourCc("8BPS"))
      {
         return fail(ErrorCode::NotAPsdFile, 0, "signature is not 8BPS");
      }
      if (file_version == 2)
      {
         return fail(ErrorCode::UnsupportedVersion, 4, "large document format (psb)");
      }
      if (file_version != 1)
      {
         return fail(ErrorCode::Corrupt, 4, std::format("version {}", file_version));
      }
      if (channels < 1 || channels > max_channels)
      {
         return fail(ErrorCode::Corrupt, 12, std::format("{} channels", channels));
      }
      if (width < 1 || height < 1 || width > max_dimension || height > max_dimension)
      {
         return fail(ErrorCode::Corrupt, 14, std::format("size {} x {}", width, height));
      }

      auto& header = _document._header;
      header._width = static_cast<int32_t>(width);
      header._height = static_cast<int32_t>(height);
      header._channels = channels;
      header._depth = depth;
      header._color_mode = static_cast<ColorMode>(mode);

      switch (header._color_mode)
      {
         case ColorMode::Bitmap:
            if (depth != 1)
            {
               return fail(ErrorCode::UnsupportedDepth, 22, std::format("bitmap with depth {}", depth));
            }
            break;
         case ColorMode::Indexed:
            if (depth != 8)
            {
               return fail(ErrorCode::UnsupportedDepth, 22, std::format("indexed color with depth {}", depth));
            }
            break;
         case ColorMode::Grayscale:
         case ColorMode::Duotone:
         case ColorMode::Rgb:
         case ColorMode::Cmyk:
            if (depth != 8 && depth != 16 && depth != 32)
            {
               return fail(ErrorCode::UnsupportedDepth, 22, std::format("depth {}", depth));
            }
            break;
         case ColorMode::Multichannel:
         case ColorMode::Lab:
            return fail(ErrorCode::UnsupportedColorMode, 24, std::format("color mode {}", mode));
         default:
            return fail(ErrorCode::Corrupt, 24, std::format("color mode {}", mode));
      }

      if (colorChannelCount() > header._channels)
      {
         return fail(ErrorCode::Corrupt, 12, std::format("{} channels for color mode {}", channels, mode));
      }
      return checkPixels(width, height, 14, "document");
   }

   [[nodiscard]] int32_t colorChannelCount() const
   {
      switch (_document._header._color_mode)
      {
         case ColorMode::Rgb:
            return 3;
         case ColorMode::Cmyk:
            return 4;
         default:
            return 1;
      }
   }

   [[nodiscard]] std::size_t bytesPerRow(int32_t width) const
   {
      const auto depth = static_cast<std::size_t>(_document._header._depth);
      return depth == 1 ? (static_cast<std::size_t>(width) + 7) / 8 : static_cast<std::size_t>(width) * depth / 8;
   }

   // ------------------------------------------------------------------------------------------------
   // color mode data section: the palette of indexed documents
   // ------------------------------------------------------------------------------------------------

   bool parseColorModeData()
   {
      const auto length = _reader.u32();
      auto section = _reader.sub(length);
      if (!checkRead(_reader, "color mode data"))
      {
         return false;
      }
      if (_document._header._color_mode == ColorMode::Indexed)
      {
         // 256 red values, then 256 green, then 256 blue
         const auto table = section.take(768);
         if (table.empty())
         {
            return fail(ErrorCode::Corrupt, section.offset(), "indexed color without a 768 byte palette");
         }
         for (std::size_t index = 0; index < 768; ++index)
         {
            _palette[index] = std::to_integer<uint8_t>(table[index]);
         }
      }
      return true;
   }

   // ------------------------------------------------------------------------------------------------
   // image resources section: only the saved paths are read
   // ------------------------------------------------------------------------------------------------

   bool parseImageResources()
   {
      const auto length = _reader.u32();
      auto section = _reader.sub(length);
      if (!checkRead(_reader, "image resources"))
      {
         return false;
      }

      // 4 signature, 2 id, pascal name padded to even, 4 size, data padded to even.
      // the resources are optional for us, so a malformed block ends the section instead of the load
      while (section.remaining() >= 12)
      {
         const auto signature = section.u32();
         if (signature != fourCc("8BIM") && signature != fourCc("MeSa") && signature != fourCc("PHUT") && signature != fourCc("AgHg") &&
             signature != fourCc("DCSR"))
         {
            break;
         }
         const auto id = section.u16();
         const auto name_length = section.u8();
         const auto name = latin1ToUtf8(section.take(name_length));
         section.align(2);
         const auto size = section.u32();
         auto block = section.sub(size);
         section.align(2);
         if (section.failed())
         {
            break;
         }
         if (id >= 2000 && id <= 2997)
         {
            parsePath(block, id, name);
         }
      }
      return true;
   }

   void parsePath(Reader block, int32_t id, std::string name)
   {
      _document._paths.push_back(Path{id, std::move(name), parsePathRecords(block, nullptr)});
   }

   /// \brief the 26 byte path records of a saved path or a vector mask.
   /// \param starts_filled set from the initial fill record, if not null
   std::vector<Subpath> parsePathRecords(Reader block, bool* starts_filled)
   {
      // points are 8.24 fixed point fractions of the document size, vertical first
      const auto width = static_cast<float>(_document._header._width);
      const auto height = static_cast<float>(_document._header._height);
      const auto read_point = [&](Reader& record)
      {
         const auto y = static_cast<float>(record.i32()) / 16777216.0f;
         const auto x = static_cast<float>(record.i32()) / 16777216.0f;
         return Point{x * width, y * height};
      };

      std::vector<Subpath> subpaths;
      while (block.remaining() >= 26)
      {
         auto record = block.sub(26);
         const auto type = record.u16();
         switch (type)
         {
            case 0:  // closed subpath length record
            case 3:  // open subpath length record
            {
               auto& subpath = subpaths.emplace_back();
               subpath._closed = type == 0;
               subpath._knots.reserve(std::min<std::size_t>(record.u16(), block.remaining() / 26));
               break;
            }
            case 1:  // closed subpath knot, linked
            case 2:  // closed subpath knot, unlinked
            case 4:  // open subpath knot, linked
            case 5:  // open subpath knot, unlinked
            {
               // knots before any length record are malformed and ignored, the old loader wrote out of bounds here
               if (subpaths.empty())
               {
                  break;
               }
               Knot knot;
               knot._in = read_point(record);
               knot._anchor = read_point(record);
               knot._out = read_point(record);
               knot._linked = type == 1 || type == 4;
               subpaths.back()._knots.push_back(knot);
               break;
            }
            case 8:  // initial fill record: 1 = start with all pixels filled
               if (starts_filled != nullptr)
               {
                  *starts_filled = record.u16() == 1;
               }
               break;
            default:  // fill rule and clipboard records
               break;
         }
      }
      return subpaths;
   }

   // ------------------------------------------------------------------------------------------------
   // layer and mask information section
   // ------------------------------------------------------------------------------------------------

   bool parseLayerAndMaskInformation()
   {
      const auto length = _reader.u32();
      auto section = _reader.sub(length);
      if (!checkRead(_reader, "layer and mask information"))
      {
         return false;
      }
      if (length == 0)
      {
         return true;
      }

      // layer info, with its own length
      const auto layer_info_length = section.u32();
      auto layer_info = section.sub(layer_info_length);
      if (!checkRead(section, "layer info"))
      {
         return false;
      }
      if (layer_info_length > 0 && !parseLayerInfo(layer_info))
      {
         return false;
      }

      // global layer mask info, ignored
      const auto global_mask_length = section.u32();
      section.skip(global_mask_length);
      if (section.failed())
      {
         return true;  // nothing after it is required
      }

      // tagged blocks. 16 and 32 bit documents keep their layers here ('Lr16', 'Lr32') and leave the layer
      // info above empty; that is where the old loader crashed on 16 bit files
      while (section.remaining() >= 12)
      {
         const auto signature = section.u32();
         if (signature != fourCc("8BIM") && signature != fourCc("8B64"))
         {
            break;
         }
         const auto key = section.u32();
         const auto block_length = section.u32();
         auto block = section.sub(block_length);
         if (section.failed())
         {
            return fail(ErrorCode::Truncated, block.offset(), "tagged block in the layer and mask information");
         }
         section.align(2);
         if ((key == fourCc("Lr16") || key == fourCc("Lr32") || key == fourCc("Layr")) && _document._layers.empty())
         {
            if (!parseLayerInfo(block))
            {
               return false;
            }
         }
      }
      return true;
   }

   bool parseLayerInfo(Reader& info)
   {
      // a negative count means the first alpha channel of the composite is its transparency
      const auto count = info.i16();
      if (!checkRead(info, "layer count"))
      {
         return false;
      }
      _composite_has_alpha = count < 0;
      const auto layer_count = std::abs(static_cast<int32_t>(count));

      // a layer record is at least 34 bytes, so the count cannot exceed what the data could hold
      if (static_cast<std::size_t>(layer_count) > info.remaining() / 34)
      {
         return fail(ErrorCode::Corrupt, info.offset(), std::format("{} layers in {} bytes", layer_count, info.remaining()));
      }

      std::vector<std::vector<ChannelInfo>> channels(static_cast<std::size_t>(layer_count));
      std::vector<int32_t> mask_channels(static_cast<std::size_t>(layer_count), no_mask_channel);
      _document._layers.resize(static_cast<std::size_t>(layer_count));
      for (auto index = 0; index < layer_count; ++index)
      {
         const auto layer_index = static_cast<std::size_t>(index);
         if (!parseLayerRecord(info, _document._layers[layer_index], channels[layer_index], mask_channels[layer_index]))
         {
            return false;
         }
      }

      // the channel image data of all layers follows the records, in the same order
      for (auto index = 0; index < layer_count; ++index)
      {
         const auto layer_index = static_cast<std::size_t>(index);
         if (!parseLayerPixels(info, _document._layers[layer_index], channels[layer_index], mask_channels[layer_index]))
         {
            return false;
         }
      }
      return true;
   }

   struct ChannelInfo
   {
      int32_t _id{0};
      uint32_t _length{0};  //!< including the 2 byte compression field
   };

   /// \brief channel ids: -2 is the layer mask, -3 the "real" user mask photoshop writes when a layer has both a
   ///        user mask and a rendered one; 0 means the layer has no raster mask to decode.
   static constexpr int32_t no_mask_channel = 0;

   bool parseLayerRecord(Reader& info, Layer& layer, std::vector<ChannelInfo>& channels, int32_t& mask_channel)
   {
      const auto record_offset = info.offset();
      layer._bounds._top = info.i32();
      layer._bounds._left = info.i32();
      layer._bounds._bottom = info.i32();
      layer._bounds._right = info.i32();
      const auto channel_count = info.u16();
      if (!checkRead(info, "layer record"))
      {
         return false;
      }

      // 64 bit, so corrupt coordinates like top = -2^31 cannot overflow
      const auto width = static_cast<int64_t>(layer._bounds._right) - layer._bounds._left;
      const auto height = static_cast<int64_t>(layer._bounds._bottom) - layer._bounds._top;
      if (width < 0 || height < 0 || width > max_dimension || height > max_dimension)
      {
         return fail(ErrorCode::Corrupt, record_offset, std::format("layer size {} x {}", width, height));
      }
      if (channel_count > max_channels)
      {
         return fail(ErrorCode::Corrupt, record_offset + 16, std::format("{} layer channels", channel_count));
      }

      for (auto index = 0; index < channel_count; ++index)
      {
         ChannelInfo channel;
         channel._id = info.i16();
         channel._length = info.u32();
         channels.push_back(channel);
      }

      const auto blend_signature = info.u32();
      for (auto& character : layer._blend_mode)
      {
         character = static_cast<char>(info.u8());
      }
      layer._opacity = info.u8();
      layer._clipping = info.u8() != 0;
      const auto flags = info.u8();
      info.skip(1);  // filler
      const auto extra_length = info.u32();
      auto extra = info.sub(extra_length);
      if (!checkRead(info, "layer record"))
      {
         return false;
      }
      if (blend_signature != fourCc("8BIM"))
      {
         return fail(ErrorCode::Corrupt, record_offset, "layer blend mode signature is not 8BIM");
      }
      layer._visible = (flags & 0x02) == 0;

      // layers without extra data have no name and no section key (fix from deceptus: f8ccfc29)
      if (extra_length > 0)
      {
         parseLayerExtraData(extra, layer, channels, mask_channel);
      }
      return true;
   }

   /// \brief the layer mask / adjustment layer data of a layer record.
   struct MaskBlock
   {
      bool _present{false};
      Rect _bounds;
      uint8_t _default_color{0};
      uint8_t _flags{0};
      bool _has_real{false};
      Rect _real_bounds;
      uint8_t _real_default_color{0};
      uint8_t _real_flags{0};
      std::optional<uint8_t> _user_density;
      std::optional<double> _user_feather;
      std::optional<uint8_t> _vector_density;
      std::optional<double> _vector_feather;
   };

   [[nodiscard]] static Rect readRect(Reader& reader)
   {
      Rect rect;
      rect._top = reader.i32();
      rect._left = reader.i32();
      rect._bottom = reader.i32();
      rect._right = reader.i32();
      return rect;
   }

   [[nodiscard]] static bool plausibleRect(const Rect& rect)
   {
      const auto width = static_cast<int64_t>(rect._right) - rect._left;
      const auto height = static_cast<int64_t>(rect._bottom) - rect._top;
      return width >= 0 && height >= 0 && width <= max_dimension && height <= max_dimension;
   }

   /// \return the size of the mask parameters: their flag byte plus a density byte or a feather double for each bit
   [[nodiscard]] static std::size_t maskParametersSize(uint8_t parameters)
   {
      return 1 + ((parameters & 0x01) != 0 ? 1 : 0) + ((parameters & 0x02) != 0 ? 8 : 0) + ((parameters & 0x04) != 0 ? 1 : 0) +
             ((parameters & 0x08) != 0 ? 8 : 0);
   }

   /// \brief 0 bytes = no mask; otherwise rectangle, default color, flags, then optionally the "real" flags, default
   ///        color and rectangle, then the mask parameters if flag bit 4 is set, or 2 bytes of padding.
   ///        photoshop writes the real fields before the parameters, against the specification (psd-tools found the
   ///        same). whether real fields are there follows from which layout adds up to the block size; the size alone
   ///        is ambiguous, two feathers make a block without real fields as long as one with them.
   [[nodiscard]] static MaskBlock parseMaskBlock(Reader block)
   {
      MaskBlock mask;
      const auto size = block.remaining();
      if (size < 18)
      {
         return mask;
      }
      mask._bounds = readRect(block);
      mask._default_color = block.u8();
      mask._flags = block.u8();
      const auto has_parameters = (mask._flags & 0x10) != 0;

      // the parameter flag byte at the position each layout would put it
      const auto parameters_at = [&](std::size_t skip)
      {
         auto probe = block;
         probe.skip(skip);
         return probe.u8();
      };
      const auto rest = block.remaining();
      const auto fits = [&](std::size_t used) { return used <= rest && rest - used < 4; };  // a little padding is fine
      auto has_real = rest >= 18;
      if (has_parameters)
      {
         // set parameter flags are part of each candidate: bit 4 promises at least one parameter
         const auto real_parameters = rest >= 18 ? parameters_at(18) : uint8_t{0};
         const auto real_used = 18 + maskParametersSize(real_parameters);
         const auto plain_used = maskParametersSize(parameters_at(0));
         const auto real_fits = rest >= 18 && (real_parameters & 0x0f) != 0 && fits(real_used);
         const auto plain_fits = fits(plain_used);
         has_real = real_fits && (!plain_fits || real_used == rest || plain_used != rest);
      }
      if (has_real)
      {
         mask._real_flags = block.u8();
         mask._real_default_color = block.u8();
         mask._real_bounds = readRect(block);
         mask._has_real = true;
      }
      if (has_parameters)
      {
         const auto parameters = block.u8();
         if ((parameters & 0x01) != 0)
         {
            mask._user_density = block.u8();
         }
         if ((parameters & 0x02) != 0)
         {
            mask._user_feather = block.f64();
         }
         if ((parameters & 0x04) != 0)
         {
            mask._vector_density = block.u8();
         }
         if ((parameters & 0x08) != 0)
         {
            mask._vector_feather = block.f64();
         }
      }
      mask._present = !block.failed();
      return mask;
   }

   /// \brief mask data, blending ranges, the name, then tagged blocks. all optional, so a malformed part ends
   ///        the parsing of the extra data instead of the load.
   void parseLayerExtraData(Reader& extra, Layer& layer, const std::vector<ChannelInfo>& channels, int32_t& mask_channel)
   {
      const auto mask_length = extra.u32();
      const auto mask_block = parseMaskBlock(extra.sub(mask_length));
      extra.skip(extra.u32());  // layer blending ranges

      // pascal string padded to a multiple of 4 bytes, including the length byte
      const auto name_length = extra.u8();
      const auto name = extra.take(name_length);
      extra.align(4);
      if (extra.failed())
      {
         return;
      }
      layer._name = latin1ToUtf8(name);
      applyMaskBlock(mask_block, layer, channels, mask_channel);

      while (extra.remaining() >= 12)
      {
         const auto signature = extra.u32();
         if (signature != fourCc("8BIM") && signature != fourCc("8B64"))
         {
            break;
         }
         const auto key = extra.u32();
         const auto length = extra.u32();
         auto block = extra.sub(length);
         extra.align(2);
         if (extra.failed())
         {
            break;
         }

         // 'lsct' is the section divider; nested groups use 'lsdk' with the same layout
         if (key == fourCc("lsct") || key == fourCc("lsdk"))
         {
            const auto type = block.u32();
            if (!block.failed() && type <= 3)
            {
               layer._section_divider = static_cast<SectionDivider>(type);
            }
         }

         // vector mask: version, flags (invert, not linked, disabled), then path records. 'vsms' is the newer key
         if ((key == fourCc("vmsk") || key == fourCc("vsms")) && !layer._vector_mask.has_value())
         {
            (void)block.u32();  // version, 3
            const auto flags = block.u32();
            if (!block.failed())
            {
               VectorMask mask;
               mask._inverted = (flags & 0x01) != 0;
               mask._linked = (flags & 0x02) == 0;
               mask._disabled = (flags & 0x04) != 0;
               mask._density = mask_block._vector_density;
               mask._feather = mask_block._vector_feather;
               mask._subpaths = parsePathRecords(block, &mask._starts_filled);
               layer._vector_mask = std::move(mask);
            }
         }

         // unicode name: 4 byte count of utf-16 code units, then the code units. the old loader kept only the
         // low byte of each unit
         if (key == fourCc("luni"))
         {
            const auto unit_count = block.u32();
            if (!block.failed() && unit_count <= block.remaining() / 2)
            {
               std::vector<uint16_t> units(unit_count);
               for (auto& unit : units)
               {
                  unit = block.u16();
               }
               layer._name = utf16ToUtf8(units);
            }
         }
      }
   }

   /// \brief a raster mask exists if its channel does: -3 together with the "real" fields, else -2. a layer with
   ///        only a vector mask has the mask fields but no mask channel.
   static void applyMaskBlock(const MaskBlock& block, Layer& layer, const std::vector<ChannelInfo>& channels, int32_t& mask_channel)
   {
      const auto has_channel = [&](int32_t id)
      { return std::ranges::any_of(channels, [id](const ChannelInfo& channel) { return channel._id == id; }); };
      if (!block._present)
      {
         return;
      }
      const auto use_real = block._has_real && has_channel(-3);
      if (!use_real && !has_channel(-2))
      {
         return;
      }
      LayerMask mask;
      mask._bounds = use_real ? block._real_bounds : block._bounds;
      mask._default_color = use_real ? block._real_default_color : block._default_color;
      const auto flags = use_real ? block._real_flags : block._flags;
      mask._relative_to_layer = (flags & 0x01) != 0;
      mask._disabled = (flags & 0x02) != 0;
      mask._density = block._user_density;
      mask._feather = block._user_feather;
      if (!plausibleRect(mask._bounds))
      {
         return;
      }
      layer._mask = std::move(mask);
      mask_channel = use_real ? -3 : -2;
   }

   bool parseLayerPixels(Reader& info, Layer& layer, const std::vector<ChannelInfo>& channels, int32_t mask_channel)
   {
      const auto width = layer._bounds.width();
      const auto height = layer._bounds.height();
      const auto has_pixels = width > 0 && height > 0;
      if (has_pixels && _options._load_layer_pixels && !checkPixels(width, height, info.offset(), "layer"))
      {
         return false;
      }

      std::vector<Plane> planes;
      for (const auto& channel : channels)
      {
         // the declared length bounds each channel, so a bad channel cannot shift the ones after it
         auto data = info.sub(channel._length);
         if (!checkRead(info, "layer channel data"))
         {
            return false;
         }

         // the raster mask has its own rectangle, and holds coverage rather than color
         if (channel._id == mask_channel && layer._mask.has_value() && _options._load_layer_pixels)
         {
            auto& mask = *layer._mask;
            const auto mask_width = mask._bounds.width();
            const auto mask_height = mask._bounds.height();
            if (mask_width > 0 && mask_height > 0)
            {
               if (!checkPixels(mask_width, mask_height, data.offset(), "layer mask") ||
                   !decodeChannel(data, mask_width, mask_height, mask._values, SampleEncoding::Linear))
               {
                  return false;
               }
            }
            continue;
         }

         // the other mask channel (-2 next to a "real" -3) and channels of other color modes are skipped
         const auto wanted = channel._id >= -1 && channel._id < colorChannelCount();
         if (!has_pixels || !_options._load_layer_pixels || !wanted)
         {
            continue;
         }

         Plane plane{channel._id, {}};
         // transparency is coverage, so it stays linear in 32 bit documents like the masks
         const auto encoding = channel._id == -1 ? SampleEncoding::Linear : SampleEncoding::Color;
         if (!decodeChannel(data, width, height, plane._values, encoding))
         {
            return false;
         }
         planes.push_back(std::move(plane));
      }

      if (has_pixels && _options._load_layer_pixels && !layer.isSectionDivider())
      {
         const auto find = [&](int32_t id) -> const std::vector<uint8_t>*
         {
            const auto it = std::ranges::find_if(planes, [id](const Plane& plane) { return plane._id == id; });
            return it == planes.end() ? nullptr : &it->_values;
         };
         std::array<const std::vector<uint8_t>*, 4> color{};
         for (auto index = 0; index < colorChannelCount(); ++index)
         {
            color[static_cast<std::size_t>(index)] = find(index);
         }
         layer._image = compose(width, height, color, find(-1));
      }
      return true;
   }

   /// \brief decodes one layer channel, compression field included, into width * height 8 bit values.
   bool
   decodeChannel(Reader& data, int32_t width, int32_t height, std::vector<uint8_t>& values, SampleEncoding encoding = SampleEncoding::Color)
   {
      const auto compression = data.u16();
      if (!checkRead(data, "channel compression"))
      {
         return false;
      }
      const auto row_bytes = bytesPerRow(width);
      if (!checkPlausibleSize(data, compression, row_bytes, height, 1))
      {
         return false;
      }
      std::vector<uint8_t> samples(row_bytes);
      values.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);

      if (compression == 0)
      {
         const auto raw = data.take(row_bytes * static_cast<std::size_t>(height));
         if (!checkRead(data, "raw channel data"))
         {
            return false;
         }
         for (auto y = 0; y < height; ++y)
         {
            const auto row = raw.subspan(static_cast<std::size_t>(y) * row_bytes, row_bytes);
            std::ranges::transform(row, samples.begin(), [](std::byte byte) { return std::to_integer<uint8_t>(byte); });
            samplesTo8Bit(samples, _document._header._depth, rowOf(values, width, y), encoding);
         }
         return true;
      }
      if (compression == 1)
      {
         return decodeRleRows(data, width, height, 1, [&](int32_t, int32_t y) { return rowOf(values, width, y); }, encoding);
      }
      if (compression == 2 || compression == 3)
      {
         const auto offset = data.offset();
         std::vector<uint8_t> raw(row_bytes * static_cast<std::size_t>(height));
         if (!Inflater{data.take(data.remaining())}.run(raw))
         {
            return fail(ErrorCode::Corrupt, offset, "zip data does not inflate to the channel size");
         }
         for (auto y = 0; y < height; ++y)
         {
            const auto row = std::span{raw}.subspan(static_cast<std::size_t>(y) * row_bytes, row_bytes);
            if (compression == 3)
            {
               undoPrediction(row, _document._header._depth, width);
            }
            samplesTo8Bit(row, _document._header._depth, rowOf(values, width, y), encoding);
         }
         return true;
      }
      return fail(ErrorCode::Corrupt, data.offset() - 2, std::format("compression {}", compression));
   }

   /// \brief rejects data that cannot hold the rows it claims, before the output is allocated. raw rows need their
   ///        full size, packbits rows a 2 byte size each plus at least 2 bytes per 128 output bytes.
   bool checkPlausibleSize(const Reader& data, uint16_t compression, std::size_t row_bytes, int32_t height, int32_t channel_count)
   {
      const auto rows = static_cast<uint64_t>(height) * static_cast<uint64_t>(channel_count);
      uint64_t minimum = 0;
      if (compression == 0)
      {
         minimum = rows * row_bytes;
      }
      else if (compression == 1)
      {
         minimum = rows * (2 + 2 * ((row_bytes + 127) / 128));
      }
      else if (compression == 2 || compression == 3)
      {
         // deflate expands at most about 1032 to 1, plus the zlib header and checksum
         minimum = rows * row_bytes / 1032 + 6;
      }
      if (minimum > data.remaining())
      {
         return fail(
            compression == 0 ? ErrorCode::Truncated : ErrorCode::Corrupt,
            data.offset(),
            std::format("{} rows need at least {} bytes, {} left", rows, minimum, data.remaining())
         );
      }
      return true;
   }

   [[nodiscard]] static std::span<uint8_t> rowOf(std::vector<uint8_t>& values, int32_t width, int32_t y)
   {
      return std::span{values}.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(width), static_cast<std::size_t>(width));
   }

   /// \brief packbits rows: a table of compressed row sizes for all rows of all channels, then the rows.
   ///        output(channel, y) returns where the 8 bit values of that row go.
   /// \param linear_channel a channel that holds coverage (the composite's transparency) and stays linear, or -1
   template <class Output>
   bool decodeRleRows(
      Reader& data,
      int32_t width,
      int32_t height,
      int32_t channel_count,
      Output&& output,
      SampleEncoding encoding = SampleEncoding::Color,
      int32_t linear_channel = -1
   )
   {
      const auto row_count = static_cast<std::size_t>(height) * static_cast<std::size_t>(channel_count);
      std::vector<uint16_t> row_sizes(row_count);
      std::size_t total = 0;
      for (auto& size : row_sizes)
      {
         size = data.u16();
         total += size;
      }
      if (!checkRead(data, "packbits row sizes"))
      {
         return false;
      }

      // packbits needs at least 2 bytes per 128 output bytes; this also bounds the allocation by the file size
      const auto row_bytes = bytesPerRow(width);
      const auto minimum_row_size = 2 * ((row_bytes + 127) / 128);
      if (total > data.remaining() || total < minimum_row_size * row_count)
      {
         return fail(ErrorCode::Corrupt, data.offset(), std::format("packbits sizes {} for {} bytes of data", total, data.remaining()));
      }

      std::vector<uint8_t> samples(row_bytes);
      for (auto channel = 0; channel < channel_count; ++channel)
      {
         for (auto y = 0; y < height; ++y)
         {
            const auto offset = data.offset();
            const auto source =
               data.take(row_sizes[static_cast<std::size_t>(channel) * static_cast<std::size_t>(height) + static_cast<std::size_t>(y)]);
            if (!unpackBits(source, samples))
            {
               return fail(ErrorCode::Corrupt, offset, "packbits row does not decode to the row size");
            }
            const auto destination = output(channel, y);
            if (_document._header._depth == 1)
            {
               expandBits(samples, destination);
            }
            else
            {
               samplesTo8Bit(samples, _document._header._depth, destination, channel == linear_channel ? SampleEncoding::Linear : encoding);
            }
         }
      }
      return true;
   }

   /// \brief 1 bit per pixel, 1 = black.
   static void expandBits(std::span<const uint8_t> bits, std::span<uint8_t> output)
   {
      for (std::size_t x = 0; x < output.size(); ++x)
      {
         const auto bit = (bits[x / 8] >> (7 - x % 8)) & 1;
         output[x] = bit != 0 ? uint8_t{0} : uint8_t{255};
      }
   }

   /// \brief combines 8 bit planes into 4 byte pixels. missing color planes count as 0, a missing alpha as opaque.
   [[nodiscard]] Image
   compose(int32_t width, int32_t height, const std::array<const std::vector<uint8_t>*, 4>& color, const std::vector<uint8_t>* alpha) const
   {
      Image image{width, height, std::vector<uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)};
      const auto value = [](const std::vector<uint8_t>* plane, std::size_t index) -> uint32_t
      { return plane != nullptr ? (*plane)[index] : 0u; };
      const auto pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
      const auto bgra = _options._pixel_format == PixelFormat::Bgra8;

      for (std::size_t index = 0; index < pixel_count; ++index)
      {
         uint32_t red = 0;
         uint32_t green = 0;
         uint32_t blue = 0;
         switch (_document._header._color_mode)
         {
            case ColorMode::Rgb:
               red = value(color[0], index);
               green = value(color[1], index);
               blue = value(color[2], index);
               break;
            case ColorMode::Cmyk:
            {
               // photoshop stores cmyk inverted: 255 is no ink
               const auto black = value(color[3], index);
               red = (value(color[0], index) * black + 127) / 255;
               green = (value(color[1], index) * black + 127) / 255;
               blue = (value(color[2], index) * black + 127) / 255;
               break;
            }
            case ColorMode::Indexed:
            {
               const auto entry = value(color[0], index);
               red = _palette[entry];
               green = _palette[256 + entry];
               blue = _palette[512 + entry];
               break;
            }
            default:  // grayscale, duotone (shown as its gray ramp), bitmap
               red = green = blue = value(color[0], index);
               break;
         }

         auto* const pixel = image._pixels.data() + index * 4;
         pixel[0] = static_cast<uint8_t>(bgra ? blue : red);
         pixel[1] = static_cast<uint8_t>(green);
         pixel[2] = static_cast<uint8_t>(bgra ? red : blue);
         pixel[3] = alpha != nullptr ? (*alpha)[index] : uint8_t{255};
      }
      return image;
   }

   // ------------------------------------------------------------------------------------------------
   // image data section: the merged composite
   // ------------------------------------------------------------------------------------------------

   bool parseImageData()
   {
      if (!_options._load_composite || _reader.remaining() == 0)
      {
         return true;  // some writers leave the composite out
      }

      const auto& header = _document._header;
      const auto compression = _reader.u16();
      if (!checkRead(_reader, "image data compression"))
      {
         return false;
      }

      // the composite has all document channels; we keep the color ones and the transparency, if any
      if (!checkPlausibleSize(_reader, compression, bytesPerRow(header._width), header._height, header._channels))
      {
         return false;
      }
      const auto color_count = colorChannelCount();
      const auto alpha_index = _composite_has_alpha && header._channels > color_count ? color_count : -1;
      const auto kept = alpha_index >= 0 ? color_count + 1 : color_count;
      std::vector<std::vector<uint8_t>> planes(static_cast<std::size_t>(kept));
      for (auto& plane : planes)
      {
         plane.assign(static_cast<std::size_t>(header._width) * static_cast<std::size_t>(header._height), 0);
      }
      std::vector<uint8_t> discarded(static_cast<std::size_t>(header._width));
      const auto destination = [&](int32_t channel, int32_t y) -> std::span<uint8_t>
      {
         if (channel < kept)
         {
            return rowOf(planes[static_cast<std::size_t>(channel)], header._width, y);
         }
         return discarded;
      };
      // the transparency is coverage and stays linear in 32 bit documents; colors get the srgb curve
      const auto encodingOf = [&](int32_t channel) { return channel == alpha_index ? SampleEncoding::Linear : SampleEncoding::Color; };

      if (compression == 0)
      {
         const auto row_bytes = bytesPerRow(header._width);
         std::vector<uint8_t> samples(row_bytes);
         for (auto channel = 0; channel < header._channels; ++channel)
         {
            for (auto y = 0; y < header._height; ++y)
            {
               const auto row = _reader.take(row_bytes);
               if (!checkRead(_reader, "raw image data"))
               {
                  return false;
               }
               std::ranges::transform(row, samples.begin(), [](std::byte byte) { return std::to_integer<uint8_t>(byte); });
               if (header._depth == 1)
               {
                  expandBits(samples, destination(channel, y));
               }
               else
               {
                  samplesTo8Bit(samples, header._depth, destination(channel, y), encodingOf(channel));
               }
            }
         }
      }
      else if (compression == 1)
      {
         if (!decodeRleRows(_reader, header._width, header._height, header._channels, destination, SampleEncoding::Color, alpha_index))
         {
            return false;
         }
      }
      else if (compression == 2 || compression == 3)
      {
         const auto offset = _reader.offset();
         const auto row_bytes = bytesPerRow(header._width);
         std::vector<uint8_t> raw(row_bytes * static_cast<std::size_t>(header._height) * static_cast<std::size_t>(header._channels));
         if (!Inflater{_reader.take(_reader.remaining())}.run(raw))
         {
            return fail(ErrorCode::Corrupt, offset, "zip data does not inflate to the image size");
         }
         for (auto channel = 0; channel < header._channels; ++channel)
         {
            for (auto y = 0; y < header._height; ++y)
            {
               const auto row_index =
                  static_cast<std::size_t>(channel) * static_cast<std::size_t>(header._height) + static_cast<std::size_t>(y);
               const auto row = std::span{raw}.subspan(row_index * row_bytes, row_bytes);
               if (compression == 3)
               {
                  undoPrediction(row, header._depth, header._width);
               }
               if (header._depth == 1)
               {
                  expandBits(row, destination(channel, y));
               }
               else
               {
                  samplesTo8Bit(row, header._depth, destination(channel, y), encodingOf(channel));
               }
            }
         }
      }
      else
      {
         return fail(ErrorCode::Corrupt, _reader.offset() - 2, std::format("compression {}", compression));
      }

      std::array<const std::vector<uint8_t>*, 4> color{};
      for (auto index = 0; index < color_count; ++index)
      {
         color[static_cast<std::size_t>(index)] = &planes[static_cast<std::size_t>(index)];
      }
      _document._composite = compose(header._width, header._height, color, alpha_index >= 0 ? &planes.back() : nullptr);
      return true;
   }

   Reader _reader;
   LoadOptions _options;
   Document _document;
   Error _error;
   std::array<uint8_t, 768> _palette{};
   bool _composite_has_alpha{false};
};

}  // namespace detail

inline std::expected<Document, Error> load(std::span<const std::byte> data, const LoadOptions& options)
{
   return detail::Parser{data, options}.parse();
}

inline std::expected<Document, Error> loadFile(const std::filesystem::path& path, const LoadOptions& options)
{
   std::ifstream file{path, std::ios::binary};
   if (!file)
   {
      return std::unexpected{Error{ErrorCode::CannotOpenFile, 0, path.string()}};
   }
   file.seekg(0, std::ios::end);
   const auto size = file.tellg();
   if (size < 0)
   {
      return std::unexpected{Error{ErrorCode::CannotOpenFile, 0, path.string()}};
   }
   file.seekg(0, std::ios::beg);
   std::vector<std::byte> data(static_cast<std::size_t>(size));
   if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
   {
      return std::unexpected{Error{ErrorCode::CannotOpenFile, 0, path.string()}};
   }
   return load(data, options);
}

}  // namespace tinypsd
