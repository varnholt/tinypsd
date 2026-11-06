#pragma once

// a small psd writer for test fixtures. it writes only what the tests need, but in the exact layout photoshop uses,
// so the loader is tested against files built from the specification rather than against its own reading of them.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace test
{

enum class Compression : uint16_t
{
   Raw = 0,
   PackBits = 1,
   Zip = 2,
   ZipPredicted = 3,
};

struct ChannelData
{
   int16_t _id{0};
   std::vector<uint8_t> _samples;  //!< big endian samples at the document depth, rows top to bottom
   Compression _compression{Compression::Raw};
};

struct KnotData
{
   float _x{0.0f};  //!< fraction of the document width
   float _y{0.0f};
};

struct LayerData
{
   int32_t _top{0};
   int32_t _left{0};
   int32_t _bottom{0};
   int32_t _right{0};
   std::vector<ChannelData> _channels;
   std::string _name;             //!< legacy pascal name, latin-1 bytes
   std::u16string _unicode_name;  //!< written as 'luni' if not empty
   int32_t _section_divider{-1};  //!< written as 'lsct' (or 'lsdk' if _nested_key) if >= 0
   bool _nested_key{false};
   uint8_t _opacity{255};
   uint8_t _clipping{0};
   bool _hidden{false};
   std::string _blend_mode{"norm"};
   bool _no_extra_data{false};  //!< extra data length 0, like some writers do for group markers
   int32_t _mask_top{0};        //!< rectangle of a -2 mask channel, if there is one
   int32_t _mask_left{0};
   int32_t _mask_bottom{0};
   int32_t _mask_right{0};
   uint8_t _mask_default_color{255};
   uint8_t _mask_flags{0};  //!< bit 0 position relative to layer, bit 1 disabled
   std::optional<uint8_t> _mask_density;
   std::optional<double> _mask_feather;
   std::optional<uint8_t> _vector_mask_density;
   std::optional<double> _vector_mask_feather;
   bool _real_mask{false};  //!< writes the "real" fields; a -3 channel uses this rectangle
   int32_t _real_mask_top{0};
   int32_t _real_mask_left{0};
   int32_t _real_mask_bottom{0};
   int32_t _real_mask_right{0};
   uint8_t _real_mask_default_color{0};
   uint8_t _real_mask_flags{0};
   std::vector<std::vector<KnotData>> _vector_mask;  //!< closed subpaths of a 'vmsk' block, if not empty
   bool _vector_mask_new_key{false};                 //!< 'vsms' instead of 'vmsk'
   uint32_t _vector_mask_flags{0};                   //!< bit 0 inverted, bit 1 not linked, bit 2 disabled
   bool _vector_mask_starts_filled{false};           //!< writes an initial fill record
};

struct DocumentData
{
   int32_t _width{4};
   int32_t _height{4};
   uint16_t _depth{8};
   uint16_t _mode{3};
   uint16_t _channels{3};
   std::vector<uint8_t> _palette;  //!< 768 bytes for indexed documents
   std::vector<LayerData> _layers;
   bool _negative_layer_count{false};
   bool _layers_in_tagged_block{false};           //!< 'Lr16' like 16 bit photoshop files, with an empty layer info
   std::vector<KnotData> _path;                   //!< one closed subpath with these anchors, if not empty
   std::vector<std::vector<uint8_t>> _composite;  //!< one sample buffer per document channel; empty = no image data
   Compression _composite_compression{Compression::Raw};
};

// ----------------------------------------------------------------------------------------------------

class ByteWriter
{
public:
   void u8(uint32_t value)
   {
      _bytes.push_back(static_cast<std::byte>(value & 0xff));
   }

   void u16(uint32_t value)
   {
      u8(value >> 8);
      u8(value);
   }

   void u32(uint32_t value)
   {
      u16(value >> 16);
      u16(value);
   }

   void i32(int32_t value)
   {
      u32(static_cast<uint32_t>(value));
   }

   void text(std::string_view value)
   {
      for (const auto character : value)
      {
         u8(static_cast<uint8_t>(character));
      }
   }

   void f64(double value)
   {
      const auto bits = std::bit_cast<uint64_t>(value);
      u32(static_cast<uint32_t>(bits >> 32));
      u32(static_cast<uint32_t>(bits));
   }

   void bytes(std::span<const uint8_t> values)
   {
      for (const auto value : values)
      {
         u8(value);
      }
   }

   void append(const ByteWriter& other)
   {
      _bytes.insert(_bytes.end(), other._bytes.begin(), other._bytes.end());
   }

   void padTo(std::size_t alignment, std::size_t from = 0)
   {
      while ((_bytes.size() - from) % alignment != 0)
      {
         u8(0);
      }
   }

   [[nodiscard]] std::size_t size() const
   {
      return _bytes.size();
   }

   std::vector<std::byte> _bytes;
};

/// \brief packbits as photoshop writes it: runs of 3 or more repeat, the rest literal.
inline std::vector<uint8_t> packBits(std::span<const uint8_t> row)
{
   std::vector<uint8_t> packed;
   std::size_t index = 0;
   while (index < row.size())
   {
      auto run = std::size_t{1};
      while (index + run < row.size() && run < 128 && row[index + run] == row[index])
      {
         ++run;
      }
      if (run >= 3)
      {
         packed.push_back(static_cast<uint8_t>(257 - run));
         packed.push_back(row[index]);
         index += run;
         continue;
      }
      auto literal = std::size_t{0};
      while (index + literal < row.size() && literal < 128)
      {
         const auto rest = row.size() - index - literal;
         if (rest >= 3 && row[index + literal] == row[index + literal + 1] && row[index + literal] == row[index + literal + 2])
         {
            break;
         }
         ++literal;
      }
      packed.push_back(static_cast<uint8_t>(literal - 1));
      packed.insert(
         packed.end(), row.begin() + static_cast<std::ptrdiff_t>(index), row.begin() + static_cast<std::ptrdiff_t>(index + literal)
      );
      index += literal;
   }
   return packed;
}

/// \brief a zlib stream of stored (uncompressed) deflate blocks, with the adler-32 checksum.
inline std::vector<uint8_t> zlibStored(std::span<const uint8_t> data)
{
   std::vector<uint8_t> stream{0x78, 0x01};
   std::size_t offset = 0;
   do
   {
      const auto length = std::min<std::size_t>(data.size() - offset, 65535);
      const auto last = offset + length == data.size();
      stream.push_back(last ? 1 : 0);
      stream.push_back(static_cast<uint8_t>(length));
      stream.push_back(static_cast<uint8_t>(length >> 8));
      stream.push_back(static_cast<uint8_t>(~length));
      stream.push_back(static_cast<uint8_t>(~length >> 8));
      stream.insert(
         stream.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + length)
      );
      offset += length;
   } while (offset < data.size());

   uint32_t a = 1;
   uint32_t b = 0;
   for (const auto value : data)
   {
      a = (a + value) % 65521;
      b = (b + a) % 65521;
   }
   const auto adler = (b << 16) | a;
   for (const auto shift : {24, 16, 8, 0})
   {
      stream.push_back(static_cast<uint8_t>(adler >> shift));
   }
   return stream;
}

/// \brief photoshop's prediction, the inverse of what the loader undoes.
inline void predict(std::span<uint8_t> row, uint16_t depth, int32_t width)
{
   if (depth == 8)
   {
      for (auto index = row.size(); index-- > 1;)
      {
         row[index] = static_cast<uint8_t>(row[index] - row[index - 1]);
      }
   }
   else if (depth == 16)
   {
      for (auto index = row.size() - 2; index >= 2; index -= 2)
      {
         const auto value = static_cast<uint32_t>((row[index] << 8) | row[index + 1]);
         const auto previous = static_cast<uint32_t>((row[index - 2] << 8) | row[index - 1]);
         const auto delta = value - previous;
         row[index] = static_cast<uint8_t>(delta >> 8);
         row[index + 1] = static_cast<uint8_t>(delta);
      }
   }
   else if (depth == 32)
   {
      const std::vector<uint8_t> interleaved(row.begin(), row.end());
      const auto count = static_cast<std::size_t>(width);
      for (std::size_t x = 0; x < count; ++x)
      {
         for (std::size_t byte = 0; byte < 4; ++byte)
         {
            row[byte * count + x] = interleaved[x * 4 + byte];
         }
      }
      for (auto index = row.size(); index-- > 1;)
      {
         row[index] = static_cast<uint8_t>(row[index] - row[index - 1]);
      }
   }
}

inline std::size_t rowBytes(int32_t width, uint16_t depth)
{
   return depth == 1 ? (static_cast<std::size_t>(width) + 7) / 8 : static_cast<std::size_t>(width) * depth / 8;
}

/// \brief encodes rows of several channels the way both layer channels and the composite need it.
///        packbits rows of all channels share one size table at the start.
inline std::vector<uint8_t>
encodeRows(const std::vector<std::span<const uint8_t>>& channels, int32_t width, int32_t height, uint16_t depth, Compression compression)
{
   const auto row_size = rowBytes(width, depth);
   std::vector<uint8_t> encoded;
   switch (compression)
   {
      case Compression::Raw:
         for (const auto& samples : channels)
         {
            encoded.insert(encoded.end(), samples.begin(), samples.end());
         }
         break;
      case Compression::PackBits:
      {
         std::vector<std::vector<uint8_t>> rows;
         for (const auto& samples : channels)
         {
            for (auto y = 0; y < height; ++y)
            {
               rows.push_back(packBits(samples.subspan(static_cast<std::size_t>(y) * row_size, row_size)));
            }
         }
         for (const auto& row : rows)
         {
            encoded.push_back(static_cast<uint8_t>(row.size() >> 8));
            encoded.push_back(static_cast<uint8_t>(row.size()));
         }
         for (const auto& row : rows)
         {
            encoded.insert(encoded.end(), row.begin(), row.end());
         }
         break;
      }
      case Compression::Zip:
      case Compression::ZipPredicted:
      {
         std::vector<uint8_t> all;
         for (const auto& samples : channels)
         {
            all.insert(all.end(), samples.begin(), samples.end());
         }
         if (compression == Compression::ZipPredicted)
         {
            for (std::size_t offset = 0; offset < all.size(); offset += row_size)
            {
               predict(std::span{all}.subspan(offset, row_size), depth, width);
            }
         }
         encoded = zlibStored(all);
         break;
      }
   }
   return encoded;
}

/// \brief 26 byte path records: a fill rule record, optionally an initial fill record, then each closed subpath.
inline ByteWriter pathRecords(const std::vector<std::vector<KnotData>>& subpaths, bool starts_filled)
{
   ByteWriter path;
   const auto fixed = [](float value) { return static_cast<int32_t>(value * 16777216.0f); };
   path.u16(6);  // fill rule record
   path.padTo(26);
   if (starts_filled)
   {
      path.u16(8);  // initial fill record
      path.u16(1);
      path.padTo(26);
   }
   for (const auto& knots : subpaths)
   {
      path.u16(0);  // closed subpath length record
      path.u16(static_cast<uint32_t>(knots.size()));
      path.padTo(26);
      for (const auto& knot : knots)
      {
         path.u16(1);  // closed, linked
         for (auto point = 0; point < 3; ++point)
         {
            path.i32(fixed(knot._y));
            path.i32(fixed(knot._x));
         }
      }
   }
   return path;
}

inline std::vector<std::byte> writePsd(const DocumentData& document)
{
   ByteWriter file;

   // file header
   file.text("8BPS");
   file.u16(1);
   for (auto index = 0; index < 6; ++index)
   {
      file.u8(0);
   }
   file.u16(document._channels);
   file.i32(document._height);
   file.i32(document._width);
   file.u16(document._depth);
   file.u16(document._mode);

   // color mode data
   file.u32(static_cast<uint32_t>(document._palette.size()));
   file.bytes(document._palette);

   // image resources: one path, if any
   ByteWriter resources;
   if (!document._path.empty())
   {
      const auto path = pathRecords({document._path}, false);
      resources.text("8BIM");
      resources.u16(2000);
      resources.u8(4);
      resources.text("path");
      resources.u8(0);  // pad the name to an even size
      resources.u32(static_cast<uint32_t>(path.size()));
      resources.append(path);
      resources.padTo(2);
   }
   file.u32(static_cast<uint32_t>(resources.size()));
   file.append(resources);

   // layer info: records, then channel data
   ByteWriter layer_info;
   const auto count = static_cast<int32_t>(document._layers.size());
   layer_info.u16(static_cast<uint32_t>(static_cast<int16_t>(document._negative_layer_count ? -count : count)));
   std::vector<std::vector<std::vector<uint8_t>>> encoded_channels;
   for (const auto& layer : document._layers)
   {
      auto& encoded = encoded_channels.emplace_back();
      for (const auto& channel : layer._channels)
      {
         auto width = layer._right - layer._left;
         auto height = layer._bottom - layer._top;
         if (channel._id == -2)
         {
            width = layer._mask_right - layer._mask_left;
            height = layer._mask_bottom - layer._mask_top;
         }
         else if (channel._id == -3)
         {
            width = layer._real_mask_right - layer._real_mask_left;
            height = layer._real_mask_bottom - layer._real_mask_top;
         }
         auto data = std::vector<uint8_t>{0, static_cast<uint8_t>(channel._compression)};
         if (width > 0 && height > 0)
         {
            const auto rows = encodeRows({channel._samples}, width, height, document._depth, channel._compression);
            data.insert(data.end(), rows.begin(), rows.end());
         }
         encoded.push_back(std::move(data));
      }

      layer_info.i32(layer._top);
      layer_info.i32(layer._left);
      layer_info.i32(layer._bottom);
      layer_info.i32(layer._right);
      layer_info.u16(static_cast<uint32_t>(layer._channels.size()));
      for (std::size_t index = 0; index < layer._channels.size(); ++index)
      {
         layer_info.u16(static_cast<uint32_t>(static_cast<uint16_t>(layer._channels[index]._id)));
         layer_info.u32(static_cast<uint32_t>(encoded[index].size()));
      }
      layer_info.text("8BIM");
      layer_info.text(layer._blend_mode);
      layer_info.u8(layer._opacity);
      layer_info.u8(layer._clipping);
      layer_info.u8(layer._hidden ? 0x02 : 0x00);
      layer_info.u8(0);

      if (layer._no_extra_data)
      {
         layer_info.u32(0);
         continue;
      }

      ByteWriter extra;
      const auto has_mask = std::ranges::any_of(layer._channels, [](const ChannelData& channel) { return channel._id <= -2; });
      const auto has_parameters = layer._mask_density || layer._mask_feather || layer._vector_mask_density || layer._vector_mask_feather;
      if (has_mask || has_parameters || layer._real_mask)
      {
         ByteWriter mask;
         mask.i32(layer._mask_top);
         mask.i32(layer._mask_left);
         mask.i32(layer._mask_bottom);
         mask.i32(layer._mask_right);
         mask.u8(layer._mask_default_color);
         mask.u8(layer._mask_flags | (has_parameters ? 0x10u : 0x00u));
         // photoshop writes the real fields before the parameters, against the specification
         if (layer._real_mask)
         {
            mask.u8(layer._real_mask_flags);
            mask.u8(layer._real_mask_default_color);
            mask.i32(layer._real_mask_top);
            mask.i32(layer._real_mask_left);
            mask.i32(layer._real_mask_bottom);
            mask.i32(layer._real_mask_right);
         }
         if (has_parameters)
         {
            mask.u8(
               (layer._mask_density ? 0x01u : 0u) | (layer._mask_feather ? 0x02u : 0u) | (layer._vector_mask_density ? 0x04u : 0u) |
               (layer._vector_mask_feather ? 0x08u : 0u)
            );
            if (layer._mask_density)
            {
               mask.u8(*layer._mask_density);
            }
            if (layer._mask_feather)
            {
               mask.f64(*layer._mask_feather);
            }
            if (layer._vector_mask_density)
            {
               mask.u8(*layer._vector_mask_density);
            }
            if (layer._vector_mask_feather)
            {
               mask.f64(*layer._vector_mask_feather);
            }
         }
         if (!layer._real_mask && !has_parameters)
         {
            mask.u16(0);  // padding, the block is 20 bytes
         }
         extra.u32(static_cast<uint32_t>(mask.size()));
         extra.append(mask);
      }
      else
      {
         extra.u32(0);
      }
      extra.u32(0);  // blending ranges
      extra.u8(static_cast<uint32_t>(layer._name.size()));
      extra.text(layer._name);
      extra.padTo(4);
      if (layer._section_divider >= 0)
      {
         extra.text("8BIM");
         extra.text(layer._nested_key ? "lsdk" : "lsct");
         extra.u32(4);
         extra.i32(layer._section_divider);
      }
      if (!layer._vector_mask.empty())
      {
         const auto records = pathRecords(layer._vector_mask, layer._vector_mask_starts_filled);
         extra.text("8BIM");
         extra.text(layer._vector_mask_new_key ? "vsms" : "vmsk");
         extra.u32(static_cast<uint32_t>(8 + records.size()));
         extra.u32(3);  // version
         extra.u32(layer._vector_mask_flags);
         extra.append(records);
      }
      if (!layer._unicode_name.empty())
      {
         extra.text("8BIM");
         extra.text("luni");
         const auto units = layer._unicode_name.size() + 1;  // photoshop writes a trailing null
         const auto length = 4 + units * 2;
         extra.u32(static_cast<uint32_t>(length + length % 4));
         extra.u32(static_cast<uint32_t>(units));
         for (const auto unit : layer._unicode_name)
         {
            extra.u16(unit);
         }
         extra.u16(0);
         extra.padTo(4);
      }
      layer_info.u32(static_cast<uint32_t>(extra.size()));
      layer_info.append(extra);
   }
   for (const auto& layer : encoded_channels)
   {
      for (const auto& channel : layer)
      {
         layer_info.bytes(channel);
      }
   }
   layer_info.padTo(2);

   ByteWriter layer_and_mask;
   if (document._layers.empty())
   {
      // no layer info at all
   }
   else if (document._layers_in_tagged_block)
   {
      layer_and_mask.u32(0);  // empty layer info
      layer_and_mask.u32(0);  // global layer mask info
      layer_and_mask.text("8BIM");
      layer_and_mask.text("Lr16");
      layer_and_mask.u32(static_cast<uint32_t>(layer_info.size()));
      layer_and_mask.append(layer_info);
   }
   else
   {
      layer_and_mask.u32(static_cast<uint32_t>(layer_info.size()));
      layer_and_mask.append(layer_info);
      layer_and_mask.u32(0);  // global layer mask info
   }
   file.u32(static_cast<uint32_t>(layer_and_mask.size()));
   file.append(layer_and_mask);

   // composite image data
   if (!document._composite.empty())
   {
      file.u16(static_cast<uint16_t>(document._composite_compression));
      std::vector<std::span<const uint8_t>> channels;
      for (const auto& samples : document._composite)
      {
         channels.emplace_back(samples);
      }
      file.bytes(encodeRows(channels, document._width, document._height, document._depth, document._composite_compression));
   }
   return file._bytes;
}

}  // namespace test
