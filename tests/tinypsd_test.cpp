// tests against psd files built by tests/psd_writer.h, plus robustness against truncated and corrupted input

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "psd_writer.h"
#include "test_framework.h"
#include "tinypsd/tinypsd.hpp"

namespace
{

#include "zlib_vectors.inc"

/// \brief a w x h plane whose values depend on the position and a seed, so mixed up rows or channels show.
std::vector<uint8_t> pattern(int32_t width, int32_t height, int32_t seed)
{
   std::vector<uint8_t> values(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
   for (auto y = 0; y < height; ++y)
   {
      for (auto x = 0; x < width; ++x)
      {
         // long runs on the left half for packbits, noise on the right
         const auto value = x < width / 2 ? seed * 40 + y * 3 : seed * 17 + x * 31 + y * 57 + x * y;
         values[static_cast<std::size_t>(y * width + x)] = static_cast<uint8_t>(value);
      }
   }
   return values;
}

/// \brief 8 bit values to 16 bit big endian samples (v * 257 converts back to exactly v).
std::vector<uint8_t> to16(std::span<const uint8_t> values)
{
   std::vector<uint8_t> samples;
   for (const auto value : values)
   {
      const auto wide = static_cast<uint32_t>(value) * 257;
      samples.push_back(static_cast<uint8_t>(wide >> 8));
      samples.push_back(static_cast<uint8_t>(wide));
   }
   return samples;
}

/// \brief 8 bit values to 32 bit big endian linear floats that the loader's srgb encoding maps back to v.
std::vector<uint8_t> to32(std::span<const uint8_t> values)
{
   std::vector<uint8_t> samples;
   for (const auto value : values)
   {
      const auto encoded = static_cast<float>(value) / 255.0f;
      const auto linear = encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
      const auto bits = std::bit_cast<uint32_t>(linear);
      for (const auto shift : {24, 16, 8, 0})
      {
         samples.push_back(static_cast<uint8_t>(bits >> shift));
      }
   }
   return samples;
}

std::vector<uint8_t> atDepth(std::span<const uint8_t> values, uint16_t depth)
{
   if (depth == 16)
   {
      return to16(values);
   }
   if (depth == 32)
   {
      return to32(values);
   }
   return {values.begin(), values.end()};
}

/// \brief an rgb document with an rgba layer, an rgb layer without alpha and a composite.
test::DocumentData makeRgbDocument(uint16_t depth, test::Compression compression, int32_t width = 37, int32_t height = 11)
{
   test::DocumentData document;
   document._width = width;
   document._height = height;
   document._depth = depth;

   test::LayerData full{0, 0, height, width};
   full._name = "full";
   for (const auto id : std::to_array<int16_t>({0, 1, 2, -1}))
   {
      full._channels.push_back({id, atDepth(pattern(width, height, id + 2), depth), compression});
   }

   // an offset layer, partly outside the document, without a transparency channel
   test::LayerData offset{-2, 5, 6, 25};
   offset._name = "offset";
   for (const auto id : std::to_array<int16_t>({0, 1, 2}))
   {
      offset._channels.push_back({id, atDepth(pattern(20, 8, id + 7), depth), compression});
   }

   document._layers = {full, offset};
   for (auto channel = 0; channel < 3; ++channel)
   {
      document._composite.push_back(atDepth(pattern(width, height, channel + 20), depth));
   }
   document._composite_compression = compression;
   return document;
}

tinypsd::Document loadOrFail(const std::vector<std::byte>& file, const tinypsd::LoadOptions& options = {})
{
   auto document = tinypsd::load(file, options);
   CHECK_MESSAGE(document.has_value(), "load failed: {}", document ? std::string{} : tinypsd::describe(document.error()));
   return document ? std::move(*document) : tinypsd::Document{};
}

/// \brief checks that an image holds the planes in the given byte order; a null plane must be opaque (alpha)
bool imageMatches(const tinypsd::Image& image, const std::vector<std::vector<uint8_t>>& rgba, bool bgra = false)
{
   const auto pixel_count = static_cast<std::size_t>(image._width) * static_cast<std::size_t>(image._height);
   if (rgba.size() != 4 || image._pixels.size() != pixel_count * 4)
   {
      return false;
   }
   for (std::size_t index = 0; index < pixel_count; ++index)
   {
      const auto expected = [&](std::size_t channel) -> uint8_t { return rgba[channel].empty() ? uint8_t{255} : rgba[channel][index]; };
      const std::array<uint8_t, 4> want{expected(bgra ? 2 : 0), expected(1), expected(bgra ? 0 : 2), expected(3)};
      if (!std::equal(want.begin(), want.end(), image._pixels.begin() + static_cast<std::ptrdiff_t>(index * 4)))
      {
         return false;
      }
   }
   return true;
}

void checkRgbDocument(uint16_t depth, test::Compression compression)
{
   const auto data = makeRgbDocument(depth, compression);
   const auto document = loadOrFail(test::writePsd(data));
   CHECK(document._header._depth == depth);
   CHECK(document._layers.size() == 2);
   if (document._layers.size() != 2)
   {
      return;
   }

   const auto& full = document._layers[0];
   CHECK(full._name == "full");
   CHECK(imageMatches(full._image, {pattern(37, 11, 2), pattern(37, 11, 3), pattern(37, 11, 4), pattern(37, 11, 1)}));

   const auto& offset = document._layers[1];
   CHECK(offset._bounds._top == -2 && offset._bounds._left == 5 && offset._bounds.width() == 20 && offset._bounds.height() == 8);
   CHECK(imageMatches(offset._image, {pattern(20, 8, 7), pattern(20, 8, 8), pattern(20, 8, 9), {}}));

   CHECK(imageMatches(document._composite, {pattern(37, 11, 20), pattern(37, 11, 21), pattern(37, 11, 22), {}}));
}

std::filesystem::path temporaryFile(std::string_view name)
{
   return std::filesystem::temp_directory_path() / std::format("tinypsd_test_{}_{}", std::random_device{}(), name);
}

}  // namespace

// ----------------------------------------------------------------------------------------------------
// pixels
// ----------------------------------------------------------------------------------------------------

TEST_CASE("rgb at every depth and compression decodes to the written pixels")
{
   for (const auto depth : std::to_array<uint16_t>({8, 16, 32}))
   {
      for (const auto compression :
           {test::Compression::Raw, test::Compression::PackBits, test::Compression::Zip, test::Compression::ZipPredicted})
      {
         const auto failures_before = test::failureCount();
         checkRgbDocument(depth, compression);
         if (test::failureCount() != failures_before)
         {
            std::println("   ^ depth {}, compression {}", depth, static_cast<int32_t>(compression));
         }
      }
   }
}

TEST_CASE("bgra puts blue first")
{
   const auto data = makeRgbDocument(8, test::Compression::PackBits);
   const auto document = loadOrFail(test::writePsd(data), {._pixel_format = tinypsd::PixelFormat::Bgra8});
   CHECK(imageMatches(document._layers.at(0)._image, {pattern(37, 11, 2), pattern(37, 11, 3), pattern(37, 11, 4), pattern(37, 11, 1)}, true)
   );
}

TEST_CASE("grayscale, cmyk, indexed and bitmap become rgba")
{
   {
      test::DocumentData gray{._width = 9, ._height = 5, ._depth = 16, ._mode = 1, ._channels = 2};
      test::LayerData layer{0, 0, 5, 9};
      layer._channels = {
         {0, to16(pattern(9, 5, 1)), test::Compression::PackBits}, {-1, to16(pattern(9, 5, 2)), test::Compression::PackBits}
      };
      gray._layers = {layer};
      gray._negative_layer_count = true;  // the second composite channel is transparency
      gray._composite = {to16(pattern(9, 5, 3)), to16(pattern(9, 5, 4))};
      gray._composite_compression = test::Compression::PackBits;
      const auto document = loadOrFail(test::writePsd(gray));
      const auto value = pattern(9, 5, 1);
      CHECK(imageMatches(document._layers.at(0)._image, {value, value, value, pattern(9, 5, 2)}));
      CHECK(imageMatches(document._composite, {pattern(9, 5, 3), pattern(9, 5, 3), pattern(9, 5, 3), pattern(9, 5, 4)}));
   }
   {
      // cmyk is stored inverted, 255 = no ink: c = 255, m = 0 (full magenta), y = 255, k = 255 -> (255, 0, 255)
      test::DocumentData cmyk{._width = 2, ._height = 1, ._depth = 8, ._mode = 4, ._channels = 4};
      cmyk._composite = {{255, 255}, {0, 128}, {255, 255}, {255, 128}};
      const auto document = loadOrFail(test::writePsd(cmyk));
      CHECK(imageMatches(document._composite, {{255, 128}, {0, 64}, {255, 128}, {}}));
   }
   {
      test::DocumentData indexed{._width = 3, ._height = 1, ._depth = 8, ._mode = 2, ._channels = 1};
      indexed._palette.assign(768, 0);
      indexed._palette[5] = 10;        // red of entry 5
      indexed._palette[256 + 5] = 20;  // green
      indexed._palette[512 + 5] = 30;  // blue
      indexed._palette[255] = 255;
      indexed._composite = {{5, 255, 0}};
      const auto document = loadOrFail(test::writePsd(indexed));
      CHECK(imageMatches(document._composite, {{10, 255, 0}, {20, 0, 0}, {30, 0, 0}, {}}));
   }
   {
      // 1 = black, rows padded to whole bytes
      test::DocumentData bitmap{._width = 10, ._height = 2, ._depth = 1, ._mode = 0, ._channels = 1};
      bitmap._composite = {{0b1010'0000, 0b1100'0000, 0b0000'0000, 0b0100'0000}};
      bitmap._composite_compression = test::Compression::PackBits;
      const auto document = loadOrFail(test::writePsd(bitmap));
      std::vector<uint8_t> expected(20, 255);
      for (const auto index : {0, 2, 8, 9, 19})
      {
         expected[static_cast<std::size_t>(index)] = 0;
      }
      CHECK(imageMatches(document._composite, {expected, expected, expected, {}}));
   }
}

// ----------------------------------------------------------------------------------------------------
// layer properties
// ----------------------------------------------------------------------------------------------------

TEST_CASE("groups, flags, unicode names and masks")
{
   test::DocumentData data{._width = 8, ._height = 8};
   test::LayerData bottom_divider{};
   bottom_divider._name = "</Layer group>";
   bottom_divider._section_divider = 3;

   test::LayerData content{1, 1, 4, 6};
   content._name = "legacy name";
   content._unicode_name = u"Слой 👽";
   content._opacity = 128;
   content._clipping = 1;
   content._hidden = true;
   content._blend_mode = "mul ";
   for (const auto id : std::to_array<int16_t>({0, 1, 2, -1}))
   {
      content._channels.push_back({id, pattern(5, 3, id + 2), test::Compression::PackBits});
   }
   // a user mask with its own rectangle, decoded without disturbing the next layer
   content._channels.push_back({-2, pattern(3, 7, 9), test::Compression::Raw});
   content._mask_top = 0;
   content._mask_left = 0;
   content._mask_bottom = 7;
   content._mask_right = 3;

   test::LayerData nested{};
   nested._name = "nested group";
   nested._section_divider = 2;
   nested._nested_key = true;

   test::LayerData folder{};
   folder._name = "group";
   folder._section_divider = 1;

   test::LayerData no_extra{2, 2, 3, 3};
   no_extra._no_extra_data = true;
   no_extra._channels = {{0, {7}, test::Compression::Raw}, {1, {8}, test::Compression::Raw}, {2, {9}, test::Compression::Raw}};

   data._layers = {bottom_divider, content, nested, folder, no_extra};
   const auto document = loadOrFail(test::writePsd(data));
   CHECK(document._layers.size() == 5);
   if (document._layers.size() != 5)
   {
      return;
   }

   CHECK(document._layers[0]._section_divider == tinypsd::SectionDivider::BoundingSectionDivider);
   CHECK(document._layers[0].isSectionDivider() && !document._layers[0].isImageLayer());

   const auto& layer = document._layers[1];
   CHECK_MESSAGE(layer._name == "Слой 👽", "name '{}'", layer._name);
   CHECK(layer._opacity == 128);
   CHECK(layer._clipping);
   CHECK(!layer._visible);
   CHECK((layer._blend_mode == std::array{'m', 'u', 'l', ' '}));
   CHECK(layer.isImageLayer());
   CHECK(imageMatches(layer._image, {pattern(5, 3, 2), pattern(5, 3, 3), pattern(5, 3, 4), pattern(5, 3, 1)}));
   CHECK(layer._mask.has_value() && layer._mask->_values == pattern(3, 7, 9) && layer._mask->_bounds.width() == 3);
   CHECK(!layer._vector_mask.has_value());

   CHECK(document._layers[2]._section_divider == tinypsd::SectionDivider::ClosedFolder);
   CHECK(document._layers[3]._section_divider == tinypsd::SectionDivider::OpenFolder);
   CHECK(document._layers[3]._name == "group");

   // a layer without extra data has no name, but its pixels are still read (deceptus f8ccfc29)
   CHECK(document._layers[4]._name.empty());
   CHECK(imageMatches(document._layers[4]._image, {{7}, {8}, {9}, {}}));

   CHECK(document.findLayer("group") == &document._layers[3]);
   CHECK(document.findLayer("missing") == nullptr);
}

TEST_CASE("16 bit layers in an 'Lr16' tagged block, like photoshop writes them")
{
   auto data = makeRgbDocument(16, test::Compression::ZipPredicted);
   data._layers_in_tagged_block = true;
   const auto document = loadOrFail(test::writePsd(data));
   CHECK(document._layers.size() == 2);
   CHECK(
      !document._layers.empty() &&
      imageMatches(document._layers[0]._image, {pattern(37, 11, 2), pattern(37, 11, 3), pattern(37, 11, 4), pattern(37, 11, 1)})
   );
}

TEST_CASE("paths are read from the image resources")
{
   test::DocumentData data{._width = 200, ._height = 100};
   data._path = {{0.25f, 0.5f}, {0.75f, 0.5f}};
   const auto document = loadOrFail(test::writePsd(data));
   CHECK(document._paths.size() == 1);
   if (document._paths.empty())
   {
      return;
   }
   const auto& path = document._paths[0];
   CHECK(path._id == 2000 && path._name == "path");
   CHECK(path._subpaths.size() == 1 && path._subpaths[0]._closed && path._subpaths[0]._knots.size() == 2);
   if (path._subpaths.size() == 1 && path._subpaths[0]._knots.size() == 2)
   {
      const auto& knot = path._subpaths[0]._knots[1];
      CHECK(std::abs(knot._anchor._x - 150.0f) < 0.01f && std::abs(knot._anchor._y - 50.0f) < 0.01f);
      CHECK(knot._linked);
   }
}

TEST_CASE("raster masks: bounds, default color, flags, parameters")
{
   test::DocumentData data{._width = 20, ._height = 10};
   test::LayerData layer{1, 1, 5, 6};
   for (const auto id : std::to_array<int16_t>({0, 1, 2}))
   {
      layer._channels.push_back({id, pattern(5, 4, id), test::Compression::Raw});
   }
   layer._channels.push_back({-2, pattern(6, 6, 5), test::Compression::PackBits});
   layer._mask_top = 0;
   layer._mask_left = 2;
   layer._mask_bottom = 6;
   layer._mask_right = 8;
   layer._mask_default_color = 0;
   layer._mask_flags = 0x01 | 0x02;  // relative to layer, disabled
   layer._mask_density = uint8_t{200};
   layer._mask_feather = 2.5;
   layer._name = "masked";
   data._layers = {layer};

   const auto document = loadOrFail(test::writePsd(data));
   CHECK(document._layers.size() == 1 && document._layers[0]._mask.has_value());
   if (document._layers.empty() || !document._layers[0]._mask)
   {
      return;
   }
   const auto& mask = *document._layers[0]._mask;
   CHECK(mask._bounds._top == 0 && mask._bounds._left == 2 && mask._bounds.width() == 6 && mask._bounds.height() == 6);
   CHECK(mask._values == pattern(6, 6, 5));
   CHECK(mask._default_color == 0);
   CHECK(mask._relative_to_layer && mask._disabled);
   CHECK(mask._density == uint8_t{200});
   CHECK(mask._feather == 2.5);

   // valueAt works in document pixels and falls back to the default color outside the mask rectangle
   CHECK(mask.valueAt(2, 0) == pattern(6, 6, 5)[0]);
   CHECK(mask.valueAt(7, 5) == pattern(6, 6, 5)[35]);
   CHECK(mask.valueAt(1, 0) == 0 && mask.valueAt(8, 0) == 0 && mask.valueAt(2, 6) == 0);

   // without pixels the mask is still described, just without values
   const auto described = tinypsd::load(test::writePsd(data), {._load_layer_pixels = false});
   CHECK(described && described->_layers[0]._mask && described->_layers[0]._mask->_values.empty());
}

TEST_CASE("raster masks: the 'real' user mask (-3) wins over -2")
{
   test::DocumentData data{._width = 16, ._height = 16};
   test::LayerData layer{0, 0, 4, 4};
   for (const auto id : std::to_array<int16_t>({0, 1, 2}))
   {
      layer._channels.push_back({id, pattern(4, 4, id), test::Compression::Raw});
   }
   layer._channels.push_back({-2, pattern(3, 3, 7), test::Compression::Raw});
   layer._channels.push_back({-3, pattern(5, 2, 8), test::Compression::ZipPredicted});
   layer._mask_right = 3;
   layer._mask_bottom = 3;
   layer._real_mask = true;
   layer._real_mask_top = 10;
   layer._real_mask_left = 9;
   layer._real_mask_bottom = 12;
   layer._real_mask_right = 14;
   layer._real_mask_default_color = 255;
   layer._real_mask_flags = 0x02;
   layer._vector_mask = {{{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}}};
   data._layers = {layer};

   const auto document = loadOrFail(test::writePsd(data));
   const auto& mask = document._layers.empty() ? std::optional<tinypsd::LayerMask>{} : document._layers[0]._mask;
   CHECK(mask.has_value());
   if (mask)
   {
      CHECK(mask->_bounds._top == 10 && mask->_bounds._left == 9 && mask->_bounds.width() == 5 && mask->_bounds.height() == 2);
      CHECK(mask->_values == pattern(5, 2, 8));
      CHECK(mask->_default_color == 255 && mask->_disabled);
   }
   CHECK(!document._layers.empty() && document._layers[0]._vector_mask.has_value());
}

TEST_CASE("vector masks: flags, parameters and knots in document pixels")
{
   test::DocumentData data{._width = 200, ._height = 100};
   test::LayerData shape{10, 20, 30, 60};
   for (const auto id : std::to_array<int16_t>({0, 1, 2}))
   {
      shape._channels.push_back({id, pattern(40, 20, id), test::Compression::PackBits});
   }
   shape._name = "shape";
   shape._vector_mask = {{{0.25f, 0.5f}, {0.75f, 0.5f}, {0.5f, 0.25f}}, {{0.1f, 0.1f}, {0.2f, 0.2f}}};
   shape._vector_mask_new_key = true;
   shape._vector_mask_flags = 0x01 | 0x02 | 0x04;
   shape._vector_mask_starts_filled = true;
   shape._vector_mask_density = uint8_t{128};
   shape._vector_mask_feather = 1.5;

   test::LayerData plain = shape;
   plain._name = "plain";
   plain._vector_mask = {{{0.0f, 0.0f}, {1.0f, 1.0f}}};
   plain._vector_mask_new_key = false;
   plain._vector_mask_flags = 0;
   plain._vector_mask_starts_filled = false;
   plain._vector_mask_density.reset();
   plain._vector_mask_feather.reset();
   data._layers = {shape, plain};

   const auto document = loadOrFail(test::writePsd(data));
   CHECK(document._layers.size() == 2);
   if (document._layers.size() != 2 || !document._layers[0]._vector_mask || !document._layers[1]._vector_mask)
   {
      CHECK(false);
      return;
   }

   // a vector mask alone has no raster mask, and the layer pixels are unaffected
   const auto& layer = document._layers[0];
   CHECK(!layer._mask.has_value());
   CHECK(imageMatches(layer._image, {pattern(40, 20, 0), pattern(40, 20, 1), pattern(40, 20, 2), {}}));

   const auto& mask = *layer._vector_mask;
   CHECK(mask._inverted && !mask._linked && mask._disabled && mask._starts_filled);
   CHECK(mask._density == uint8_t{128} && mask._feather == 1.5);
   CHECK(mask._subpaths.size() == 2);
   if (mask._subpaths.size() == 2 && mask._subpaths[0]._knots.size() == 3)
   {
      CHECK(mask._subpaths[0]._closed && mask._subpaths[1]._knots.size() == 2);
      const auto& knot = mask._subpaths[0]._knots[1];
      CHECK(std::abs(knot._anchor._x - 150.0f) < 0.01f && std::abs(knot._anchor._y - 50.0f) < 0.01f);
   }

   const auto& plain_mask = *document._layers[1]._vector_mask;
   CHECK(!plain_mask._inverted && plain_mask._linked && !plain_mask._disabled && !plain_mask._starts_filled);
   CHECK(!plain_mask._density && !plain_mask._feather);
   CHECK(plain_mask._subpaths.size() == 1 && plain_mask._subpaths[0]._knots.size() == 2);
}

TEST_CASE("mask block layouts: real fields before parameters, and the ambiguous full parameter set")
{
   const auto make_layer = [](bool real)
   {
      test::LayerData layer{0, 0, 2, 2};
      for (const auto id : std::to_array<int16_t>({0, 1, 2}))
      {
         layer._channels.push_back({id, pattern(2, 2, id), test::Compression::Raw});
      }
      layer._channels.push_back({real ? int16_t{-3} : int16_t{-2}, pattern(3, 1, 4), test::Compression::Raw});
      layer._mask_right = real ? 0 : 3;
      layer._mask_bottom = real ? 0 : 1;
      layer._real_mask = real;
      layer._real_mask_right = 3;
      layer._real_mask_bottom = 1;
      // all four parameters: 19 bytes after the header, as long as real fields plus one parameter byte would be.
      // the feathers end in a zero byte, which is where a real layout would look for its parameter flags
      layer._mask_density = uint8_t{10};
      layer._mask_feather = 3.0;
      layer._vector_mask_density = uint8_t{20};
      layer._vector_mask_feather = 4.0;
      return layer;
   };

   for (const auto real : {false, true})
   {
      test::DocumentData data{._width = 4, ._height = 4};
      data._layers = {make_layer(real)};
      const auto document = loadOrFail(test::writePsd(data));
      const auto* const mask = document._layers.empty() || !document._layers[0]._mask ? nullptr : &*document._layers[0]._mask;
      CHECK_MESSAGE(mask != nullptr, "real {}: no mask", real);
      if (mask == nullptr)
      {
         continue;
      }
      CHECK_MESSAGE(mask->_bounds.width() == 3 && mask->_bounds.height() == 1, "real {}: bounds", real);
      CHECK_MESSAGE(mask->_values == pattern(3, 1, 4), "real {}: values", real);
      CHECK_MESSAGE(mask->_density == uint8_t{10} && mask->_feather == 3.0, "real {}: parameters", real);
   }
}

TEST_CASE("16 and 32 bit masks are coverage, not srgb encoded colors")
{
   // 0.5 in a 32 bit mask is half coverage, 128; the srgb curve used for 32 bit colors would make it 188
   const auto float_bytes = [](float value)
   {
      const auto bits = std::bit_cast<uint32_t>(value);
      return std::vector<uint8_t>{
         static_cast<uint8_t>(bits >> 24), static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits)
      };
   };
   test::DocumentData data{._width = 4, ._height = 4, ._depth = 32};
   test::LayerData layer{0, 0, 1, 1};
   for (const auto id : std::to_array<int16_t>({0, 1, 2}))
   {
      layer._channels.push_back({id, float_bytes(0.5f), test::Compression::Raw});
   }
   auto mask_samples = float_bytes(0.5f);
   const auto opaque = float_bytes(1.0f);
   mask_samples.insert(mask_samples.end(), opaque.begin(), opaque.end());
   layer._channels.push_back({-2, mask_samples, test::Compression::ZipPredicted});
   layer._mask_right = 2;
   layer._mask_bottom = 1;
   data._layers = {layer};

   const auto document = loadOrFail(test::writePsd(data));
   CHECK(!document._layers.empty() && document._layers[0]._mask.has_value());
   if (!document._layers.empty() && document._layers[0]._mask)
   {
      CHECK(document._layers[0]._mask->_values == std::vector<uint8_t>({128, 255}));
      CHECK(document._layers[0]._image._pixels[0] == 188);  // the color channel keeps the srgb curve
   }

   test::DocumentData data16{._width = 4, ._height = 4, ._depth = 16};
   test::LayerData layer16{0, 0, 1, 1};
   for (const auto id : std::to_array<int16_t>({0, 1, 2}))
   {
      layer16._channels.push_back({id, {0x80, 0x00}, test::Compression::Raw});
   }
   layer16._channels.push_back({-2, {0x80, 0x00}, test::Compression::PackBits});
   layer16._mask_right = 1;
   layer16._mask_bottom = 1;
   data16._layers = {layer16};
   const auto document16 = loadOrFail(test::writePsd(data16));
   CHECK(!document16._layers.empty() && document16._layers[0]._mask && document16._layers[0]._mask->_values == std::vector<uint8_t>({128}));
}

// ----------------------------------------------------------------------------------------------------
// options and api
// ----------------------------------------------------------------------------------------------------

TEST_CASE("load options skip work and cap the image size")
{
   const auto file = test::writePsd(makeRgbDocument(8, test::Compression::PackBits));

   const auto names_only = loadOrFail(file, {._load_layer_pixels = false, ._load_composite = false});
   CHECK(names_only._layers.size() == 2 && names_only._layers[0]._image.empty());
   CHECK(names_only._composite.empty());
   CHECK(!names_only._layers.empty() && names_only._layers[1]._name == "offset");

   const auto too_large = tinypsd::load(file, {._max_pixels = 100});
   CHECK(!too_large && too_large.error()._code == tinypsd::ErrorCode::ImageTooLarge);
}

TEST_CASE("loadFile reads from disk and reports missing files")
{
   const auto path = temporaryFile("image.psd");
   const auto file = test::writePsd(makeRgbDocument(8, test::Compression::Raw));
   {
      std::ofstream stream{path, std::ios::binary};
      stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
   }
   const auto document = tinypsd::loadFile(path);
   CHECK(document && document->_layers.size() == 2);
   std::filesystem::remove(path);

   const auto missing = tinypsd::loadFile(path);
   CHECK(!missing && missing.error()._code == tinypsd::ErrorCode::CannotOpenFile);
   CHECK(!missing && tinypsd::describe(missing.error()).starts_with("cannot open file"));
}

TEST_CASE("unsupported and invalid headers are reported")
{
   const auto expect = [](std::vector<std::byte> file, tinypsd::ErrorCode code, std::string_view what)
   {
      const auto document = tinypsd::load(file);
      CHECK_MESSAGE(
         !document && document.error()._code == code, "{}: got {}", what, document ? "success" : tinypsd::describe(document.error())
      );
   };
   const auto patched = [](std::size_t offset, std::initializer_list<uint8_t> bytes)
   {
      auto file = test::writePsd(makeRgbDocument(8, test::Compression::Raw));
      for (const auto value : bytes)
      {
         file[offset++] = static_cast<std::byte>(value);
      }
      return file;
   };

   expect({}, tinypsd::ErrorCode::NotAPsdFile, "empty");
   expect(patched(0, {'G', 'I', 'F', '8'}), tinypsd::ErrorCode::NotAPsdFile, "other signature");
   expect(patched(4, {0, 2}), tinypsd::ErrorCode::UnsupportedVersion, "psb");
   expect(patched(4, {0, 3}), tinypsd::ErrorCode::Corrupt, "version 3");
   expect(patched(12, {0, 0}), tinypsd::ErrorCode::Corrupt, "no channels");
   expect(patched(14, {0, 0, 0, 0}), tinypsd::ErrorCode::Corrupt, "height 0");
   expect(patched(18, {0, 0, 0x75, 0x31}), tinypsd::ErrorCode::Corrupt, "width 30001");
   expect(patched(22, {0, 7}), tinypsd::ErrorCode::UnsupportedDepth, "depth 7");
   expect(patched(24, {0, 9}), tinypsd::ErrorCode::UnsupportedColorMode, "lab");
   expect(patched(24, {0, 7}), tinypsd::ErrorCode::UnsupportedColorMode, "multichannel");
   expect(patched(24, {0, 5}), tinypsd::ErrorCode::Corrupt, "color mode 5");
   expect(patched(24, {0, 4}), tinypsd::ErrorCode::Corrupt, "cmyk with 3 channels");
}

// ----------------------------------------------------------------------------------------------------
// decoders
// ----------------------------------------------------------------------------------------------------

TEST_CASE("packbits: 0x80 is a no-op, runs past the row are rejected")
{
   std::array<uint8_t, 4> row{};
   const auto unpack = [&](std::initializer_list<uint8_t> bytes)
   {
      std::vector<std::byte> source;
      for (const auto value : bytes)
      {
         source.push_back(static_cast<std::byte>(value));
      }
      row.fill(0);
      return tinypsd::detail::unpackBits(source, row);
   };

   CHECK(unpack({0x80, 0xfd, 0x07}) && row == (std::array<uint8_t, 4>{7, 7, 7, 7}));
   CHECK(unpack({0x01, 1, 2, 0xff, 3}) && row == (std::array<uint8_t, 4>{1, 2, 3, 3}));
   CHECK(!unpack({0xfc, 9}));     // 5 bytes into a row of 4
   CHECK(!unpack({0x04, 1, 2}));  // literal run longer than the input
   CHECK(!unpack({0x00, 1}));     // the input ends before the row is full
}

TEST_CASE("inflate: fixed and dynamic huffman blocks, checksum and size")
{
   const auto inflate = [](std::span<const uint8_t> stream, std::size_t size)
   {
      std::vector<uint8_t> output(size);
      const auto ok = tinypsd::detail::Inflater{std::as_bytes(stream)}.run(output);
      return std::pair{ok, output};
   };

   const std::string fixed_text = "tinypsd tinypsd tinypsd!";
   const auto [fixed_ok, fixed] = inflate(zlib_fixed, fixed_text.size());
   CHECK(fixed_ok && std::equal(fixed.begin(), fixed.end(), fixed_text.begin()));

   std::vector<uint8_t> dynamic_expected;
   for (auto index = 0; index < 3000; ++index)
   {
      dynamic_expected.push_back(static_cast<uint8_t>((index * 7 + (index / 13) * 31) % 251));
   }
   for (auto repeat = 0; repeat < 200; ++repeat)
   {
      for (const auto character : {'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'})
      {
         dynamic_expected.push_back(static_cast<uint8_t>(character));
      }
   }
   const auto [dynamic_ok, dynamic] = inflate(zlib_dynamic, dynamic_expected.size());
   CHECK(dynamic_ok && dynamic == dynamic_expected);

   // wrong sizes and a broken checksum are errors, not partial results
   CHECK(!inflate(zlib_dynamic, dynamic_expected.size() - 1).first);
   CHECK(!inflate(zlib_dynamic, dynamic_expected.size() + 1).first);
   auto broken = std::vector<uint8_t>(zlib_fixed.begin(), zlib_fixed.end());
   broken.back() ^= 0x01;
   CHECK(!inflate(broken, fixed_text.size()).first);
   CHECK(!inflate(std::span{zlib_fixed}.first(10), fixed_text.size()).first);
}

// ----------------------------------------------------------------------------------------------------
// robustness: this parses untrusted files. under the sanitizer build in ci these must not report anything
// ----------------------------------------------------------------------------------------------------

namespace
{

/// \brief a file that touches most of the parser: groups, unicode names, masks, paths, zip and packbits.
std::vector<std::byte> featureRichFile()
{
   auto data = makeRgbDocument(16, test::Compression::ZipPredicted, 13, 6);
   data._layers[1]._channels[0]._compression = test::Compression::PackBits;
   data._layers[0]._unicode_name = u"ünïcödé";
   test::LayerData divider{};
   divider._name = "</Layer group>";
   divider._section_divider = 3;
   data._layers.insert(data._layers.begin(), divider);
   auto& masked = data._layers[1];
   masked._channels.push_back({-2, std::vector<uint8_t>(5 * 4 * 2, uint8_t{0x40}), test::Compression::PackBits});
   masked._mask_top = 1;
   masked._mask_left = 2;
   masked._mask_bottom = 5;
   masked._mask_right = 7;
   masked._mask_density = uint8_t{180};
   masked._vector_mask_feather = 3.0;
   masked._vector_mask = {{{0.1f, 0.1f}, {0.8f, 0.2f}, {0.4f, 0.9f}}};
   data._path = {{0.1f, 0.2f}, {0.3f, 0.4f}, {0.5f, 0.6f}};
   data._composite_compression = test::Compression::PackBits;
   return test::writePsd(data);
}

}  // namespace

TEST_CASE("every truncation is an error or a smaller valid result, never a crash")
{
   const auto file = featureRichFile();
   CHECK(tinypsd::load(file).has_value());
   auto successes = 0;
   for (std::size_t size = 0; size < file.size(); ++size)
   {
      const auto document = tinypsd::load(std::span{file}.first(size));
      if (document)
      {
         // only a missing composite at the very end may still load
         ++successes;
         CHECK(document->_layers.size() == 3 && document->_composite.empty());
      }
   }
   CHECK_MESSAGE(successes <= 1, "{} truncated sizes loaded", successes);
}

TEST_CASE("random corruption never crashes")
{
   const auto file = featureRichFile();
   std::mt19937 random{20261105u};
   std::uniform_int_distribution<std::size_t> position{0, file.size() - 1};
   std::uniform_int_distribution<int32_t> byte_value{0, 255};
   auto loaded = 0;
   for (auto iteration = 0; iteration < 3000; ++iteration)
   {
      auto corrupted = file;
      const auto changes = 1 + iteration % 4;
      for (auto change = 0; change < changes; ++change)
      {
         corrupted[position(random)] = static_cast<std::byte>(byte_value(random));
      }
      const auto document = tinypsd::load(corrupted, {._max_pixels = 1u << 20});
      if (document)
      {
         ++loaded;
         for (const auto& layer : document->_layers)
         {
            const auto expected = static_cast<std::size_t>(layer._image._width) * static_cast<std::size_t>(layer._image._height) * 4;
            CHECK(layer._image._pixels.size() == expected);
            if (layer._mask && !layer._mask->_values.empty())
            {
               const auto mask_size =
                  static_cast<std::size_t>(layer._mask->_bounds.width()) * static_cast<std::size_t>(layer._mask->_bounds.height());
               CHECK(layer._mask->_values.size() == mask_size);
               (void)layer._mask->valueAt(layer._mask->_bounds._right - 1, layer._mask->_bounds._bottom - 1);
            }
         }
      }
   }
   std::println("   {} of 3000 corrupted files still loaded", loaded);
}

int main()
{
   return test::runAll();
}
