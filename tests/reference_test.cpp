// tests against real files from photoshop and gimp, taken from psd-tools (MIT, see tests/data/psd-tools/LICENSE).
// they check what does not depend on our own reading of the format: the same image saved at different depths and
// color modes has to decode to the same pixels, and names, groups and flags have to match what photoshop shows.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "test_framework.h"
#include "tinypsd/tinypsd.hpp"

namespace
{

tinypsd::Document loadReference(std::string_view name)
{
   const auto path = std::filesystem::path{TINYPSD_TEST_DATA_DIR} / "psd-tools" / name;
   auto document = tinypsd::loadFile(path);
   CHECK_MESSAGE(document.has_value(), "{}: {}", name, document ? std::string{} : tinypsd::describe(document.error()));
   return document ? std::move(*document) : tinypsd::Document{};
}

tinypsd::ErrorCode loadError(std::string_view name)
{
   const auto document = tinypsd::loadFile(std::filesystem::path{TINYPSD_TEST_DATA_DIR} / "psd-tools" / name);
   CHECK_MESSAGE(!document.has_value(), "{} should not load", name);
   return document ? tinypsd::ErrorCode::Corrupt : document.error()._code;
}

/// \return the largest difference between two images of the same size, 256 if the sizes differ
int32_t maxDifference(const tinypsd::Image& lhs, const tinypsd::Image& rhs)
{
   if (lhs._width != rhs._width || lhs._height != rhs._height || lhs._pixels.size() != rhs._pixels.size() || lhs.empty())
   {
      return 256;
   }
   auto difference = 0;
   for (std::size_t index = 0; index < lhs._pixels.size(); ++index)
   {
      difference = std::max(difference, std::abs(lhs._pixels[index] - rhs._pixels[index]));
   }
   return difference;
}

std::vector<std::string> names(const tinypsd::Document& document)
{
   std::vector<std::string> result;
   for (const auto& layer : document._layers)
   {
      result.push_back(layer._name);
   }
   return result;
}

}  // namespace

TEST_CASE("reference: one image at 8, 16 and 32 bit and in other color modes decodes the same")
{
   const auto rgb8 = loadReference("4x4_8bit_rgb.psd");
   const auto rgb16 = loadReference("4x4_16bit_rgb.psd");
   const auto rgba8 = loadReference("4x4_8bit_rgba.psd");
   const auto indexed = loadReference("4x4_8bit_index_color.psd");
   CHECK_MESSAGE(
      maxDifference(rgb8._composite, rgb16._composite) <= 1, "16 bit differs by {}", maxDifference(rgb8._composite, rgb16._composite)
   );
   CHECK_MESSAGE(
      maxDifference(rgb8._composite, rgba8._composite) <= 1, "rgba differs by {}", maxDifference(rgb8._composite, rgba8._composite)
   );
   CHECK_MESSAGE(
      maxDifference(rgb8._composite, indexed._composite) <= 1, "indexed differs by {}", maxDifference(rgb8._composite, indexed._composite)
   );

   const auto gray8 = loadReference("4x4_8bit_grayscale.psd");
   const auto gray16 = loadReference("4x4_16bit_grayscale.psd");
   const auto duotone = loadReference("4x4_8bit_duotone.psd");
   CHECK(maxDifference(gray8._composite, gray16._composite) <= 1);
   CHECK(maxDifference(gray8._composite, duotone._composite) <= 1);

   // photoshop's 32 bit documents are linear; the 16 bit save of the same document is the reference
   const auto deep16 = loadReference("16bit5x5.psd");
   const auto deep32 = loadReference("32bit5x5.psd");
   CHECK_MESSAGE(
      maxDifference(deep16._composite, deep32._composite) <= 1, "32 bit differs by {}", maxDifference(deep16._composite, deep32._composite)
   );
   CHECK(deep16._layers.size() == 3 && deep32._layers.size() == 3);
   for (std::size_t index = 0; index < std::min(deep16._layers.size(), deep32._layers.size()); ++index)
   {
      // zip with prediction, 16 bit deltas versus 32 bit byte planes
      CHECK(maxDifference(deep16._layers[index]._image, deep32._layers[index]._image) <= 1);
   }
}

TEST_CASE("reference: bitmap documents are black and white")
{
   for (const auto* name : {"4x4_1bit_bitmap.psd", "100x20_1bit_bitmap_rle.psd"})
   {
      const auto document = loadReference(name);
      const auto& pixels = document._composite._pixels;
      CHECK(!pixels.empty());
      CHECK(std::ranges::all_of(pixels, [](uint8_t value) { return value == 0 || value == 255; }));
      CHECK(std::ranges::count(pixels, uint8_t{0}) > 0);
   }
}

TEST_CASE("reference: names, groups and flags")
{
   // russian photoshop: the unicode names, not the legacy code page bytes
   const auto two = loadReference("2layers.psd");
   CHECK((names(two) == std::vector<std::string>{"Фон", "Слой"}));
   CHECK(two._layers.size() == 2 && two._layers[1]._bounds.width() == 85 && two._layers[1]._bounds.height() == 46);

   const auto emoji = loadReference("layer-name-emoji.psd");
   CHECK(emoji._layers.size() == 1 && emoji._layers[0]._name == "👽" && emoji._layers[0]._opacity == 128);

   const auto group = loadReference("group.psd");
   CHECK((names(group) == std::vector<std::string>{"Background", "</Layer group>", "Shape 1", "Group 1"}));
   if (group._layers.size() == 4)
   {
      CHECK(group._layers[1]._section_divider == tinypsd::SectionDivider::BoundingSectionDivider);
      CHECK(group._layers[3]._section_divider == tinypsd::SectionDivider::OpenFolder);
      CHECK(group._layers[2].isImageLayer() && !group._layers[2]._image.empty());
   }

   const auto empty_layer = loadReference("empty-layer.psd");
   const auto* empty = empty_layer.findLayer("empty");
   CHECK(empty != nullptr && !empty->isImageLayer() && empty->_image.empty());

   const auto hidden = loadReference("hidden-layer.psd");
   CHECK(hidden._layers.size() == 3 && hidden._layers[1]._visible && !hidden._layers[2]._visible);
}

TEST_CASE("reference: a gimp file with transparency")
{
   const auto document = loadReference("transparentbg-gimp.psd");
   CHECK(document._layers.size() == 1);
   if (!document._layers.empty())
   {
      const auto& pixels = document._layers[0]._image._pixels;
      auto transparent = false;
      for (std::size_t index = 3; index < pixels.size(); index += 4)
      {
         transparent = transparent || pixels[index] < 255;
      }
      CHECK(transparent);
   }
}

TEST_CASE("reference: unsupported files are reported, not misread")
{
   CHECK(loadError("1layer.psb") == tinypsd::ErrorCode::UnsupportedVersion);
   CHECK(loadError("4x4_8bit_lab.psd") == tinypsd::ErrorCode::UnsupportedColorMode);
}

int main()
{
   return test::runAll();
}
