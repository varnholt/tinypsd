// prints everything tinypsd decodes from the test files, plus truncated and corrupted versions of them, in a text
// format that rust/tests/parity.rs prints the same way. both implementations are checked against tests/data/digest.txt,
// so the C++ library and the Rust crate cannot drift apart.
//
//   tinypsd_digest <tests/data/psd-tools> <tests/data/digest.txt>        compare, exit code 1 on a difference
//   tinypsd_digest <tests/data/psd-tools> --write <tests/data/digest.txt> regenerate
//
// the format, one item per line:
//   file <name> <variant>
//   error <code> <offset>                                 code is the ErrorCode in declaration order
//   header <width> <height> <channels> <depth> <mode>
//   layer <index> <name>
//   bounds <top> <left> <bottom> <right> opacity <n> visible <0|1> clipping <0|1> blend <hex> section <n>
//   image <width> <height> <bytes> <fnv-1a 64 of the pixels>
//   mask <top> <left> <bottom> <right> default <n> disabled <0|1> relative <0|1> density <n|-> feather <hex|-> values <bytes> <hash>
//   vector inverted <0|1> linked <0|1> disabled <0|1> filled <0|1> density <n|-> feather <hex|-> subpaths <n>
//   subpath <closed 0|1> <knots>
//   knot <in x> <in y> <anchor x> <anchor y> <out x> <out y> <linked 0|1>   floats as the hex of their bits
//   composite <width> <height> <bytes> <hash>
//   path <id> <name> subpaths <n>
// truncated and corrupted files get one line: "file <name> <variant> error <code> <offset>" or "... ok <hash>", where
// the hash is the fnv-1a 64 of their detailed digest.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "tinypsd/tinypsd.hpp"

namespace
{

uint64_t fnv1a(std::span<const uint8_t> bytes)
{
   uint64_t hash = 0xcbf29ce484222325ull;
   for (const auto byte : bytes)
   {
      hash = (hash ^ byte) * 0x100000001b3ull;
   }
   return hash;
}

uint64_t fnv1a(std::string_view text)
{
   return fnv1a(std::span{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

std::string flag(bool value)
{
   return value ? "1" : "0";
}

std::string bits(float value)
{
   return std::format("{:08x}", std::bit_cast<uint32_t>(value));
}

std::string optionalByte(const std::optional<uint8_t>& value)
{
   return value ? std::to_string(*value) : "-";
}

std::string optionalDouble(const std::optional<double>& value)
{
   return value ? std::format("{:016x}", std::bit_cast<uint64_t>(*value)) : "-";
}

std::string image(const tinypsd::Image& image)
{
   return std::format("{} {} {} {:016x}", image._width, image._height, image._pixels.size(), fnv1a(image._pixels));
}

void subpaths(std::string& out, const std::vector<tinypsd::Subpath>& list)
{
   for (const auto& subpath : list)
   {
      out += std::format("subpath {} {}\n", flag(subpath._closed), subpath._knots.size());
      for (const auto& knot : subpath._knots)
      {
         out += std::format(
            "knot {} {} {} {} {} {} {}\n",
            bits(knot._in._x),
            bits(knot._in._y),
            bits(knot._anchor._x),
            bits(knot._anchor._y),
            bits(knot._out._x),
            bits(knot._out._y),
            flag(knot._linked)
         );
      }
   }
}

/// \brief the detailed digest of a load result, without the "file" line.
std::string digest(const std::expected<tinypsd::Document, tinypsd::Error>& result)
{
   if (!result)
   {
      return std::format("error {} {}\n", static_cast<int32_t>(result.error()._code), result.error()._offset);
   }
   const auto& document = *result;
   const auto& header = document._header;
   std::string out = std::format(
      "header {} {} {} {} {}\n", header._width, header._height, header._channels, header._depth, static_cast<int32_t>(header._color_mode)
   );
   for (std::size_t index = 0; index < document._layers.size(); ++index)
   {
      const auto& layer = document._layers[index];
      const auto& b = layer._bounds;
      uint32_t blend = 0;
      for (const auto character : layer._blend_mode)
      {
         blend = (blend << 8) | static_cast<uint8_t>(character);
      }
      out += std::format("layer {} {}\n", index, layer._name);
      out += std::format(
         "bounds {} {} {} {} opacity {} visible {} clipping {} blend {:08x} section {}\n",
         b._top,
         b._left,
         b._bottom,
         b._right,
         layer._opacity,
         flag(layer._visible),
         flag(layer._clipping),
         blend,
         static_cast<int32_t>(layer._section_divider)
      );
      out += "image " + image(layer._image) + "\n";
      if (layer._mask)
      {
         const auto& mask = *layer._mask;
         const auto& m = mask._bounds;
         out += std::format(
            "mask {} {} {} {} default {} disabled {} relative {} density {} feather {} values {} {:016x}\n",
            m._top,
            m._left,
            m._bottom,
            m._right,
            mask._default_color,
            flag(mask._disabled),
            flag(mask._relative_to_layer),
            optionalByte(mask._density),
            optionalDouble(mask._feather),
            mask._values.size(),
            fnv1a(mask._values)
         );
      }
      if (layer._vector_mask)
      {
         const auto& mask = *layer._vector_mask;
         out += std::format(
            "vector inverted {} linked {} disabled {} filled {} density {} feather {} subpaths {}\n",
            flag(mask._inverted),
            flag(mask._linked),
            flag(mask._disabled),
            flag(mask._starts_filled),
            optionalByte(mask._density),
            optionalDouble(mask._feather),
            mask._subpaths.size()
         );
         subpaths(out, mask._subpaths);
      }
   }
   out += "composite " + image(document._composite) + "\n";
   for (const auto& path : document._paths)
   {
      out += std::format("path {} {} subpaths {}\n", path._id, path._name, path._subpaths.size());
      subpaths(out, path._subpaths);
   }
   return out;
}

std::vector<std::byte> read(const std::filesystem::path& path)
{
   std::ifstream file{path, std::ios::binary};
   std::vector<char> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
   std::vector<std::byte> result(bytes.size());
   std::ranges::transform(bytes, result.begin(), [](char c) { return static_cast<std::byte>(c); });
   return result;
}

std::string digestAll(const std::filesystem::path& directory)
{
   std::vector<std::string> names;
   for (const auto& entry : std::filesystem::directory_iterator{directory})
   {
      const auto extension = entry.path().extension().string();
      if (extension == ".psd" || extension == ".psb")
      {
         names.push_back(entry.path().filename().string());
      }
   }
   std::ranges::sort(names);

   std::string out;
   for (const auto& name : names)
   {
      const auto data = read(directory / name);

      // the real file with each option set
      out += std::format("file {} default\n", name) + digest(tinypsd::load(data));
      out += std::format("file {} bgra\n", name) + digest(tinypsd::load(data, {._pixel_format = tinypsd::PixelFormat::Bgra8}));
      out += std::format("file {} structure\n", name) +
             digest(tinypsd::load(data, {._load_layer_pixels = false, ._load_composite = false}));

      // cut off at 15 points
      for (std::size_t part = 1; part < 16; ++part)
      {
         const auto size = data.size() * part / 16;
         const auto text = digest(tinypsd::load(std::span{data}.first(size)));
         out += std::format("file {} truncate{} ", name, size) +
                (text.starts_with("error") ? text : std::format("ok {:016x}\n", fnv1a(text)));
      }

      // 24 corrupted copies, from a generator seeded with the name
      auto state = fnv1a(name);
      const auto next = [&]
      {
         state = state * 6364136223846793005ull + 1442695040888963407ull;
         return state >> 33;
      };
      for (auto round = 0; round < 24 && !data.empty(); ++round)
      {
         auto corrupted = data;
         for (auto change = 0; change < 1 + round % 4; ++change)
         {
            const auto position = next() % corrupted.size();
            corrupted[position] = static_cast<std::byte>(next() & 0xff);
         }
         const auto text = digest(tinypsd::load(corrupted, {._max_pixels = std::size_t{1} << 20}));
         out += std::format("file {} corrupt{} ", name, round) + (text.starts_with("error") ? text : std::format("ok {:016x}\n", fnv1a(text)));
      }
   }
   return out;
}

}  // namespace

int main(int argc, char* argv[])
{
   const std::span args(argv, static_cast<std::size_t>(argc));
   const auto write = args.size() == 4 && std::string_view{args[2]} == "--write";
   if (args.size() != 3 && !write)
   {
      std::println(stderr, "usage: tinypsd_digest <psd directory> [--write] <digest file>");
      return 2;
   }
   const auto actual = digestAll(args[1]);
   const std::filesystem::path expected_path = args[write ? 3 : 2];
   if (write)
   {
      std::ofstream{expected_path, std::ios::binary} << actual;
      std::println("wrote {}", expected_path.string());
      return 0;
   }

   std::ifstream file{expected_path, std::ios::binary};
   const std::string expected{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
   if (actual == expected)
   {
      std::println("{} lines match {}", std::ranges::count(actual, '\n'), expected_path.string());
      return 0;
   }
   std::istringstream lhs{actual};
   std::istringstream rhs{expected};
   std::string left;
   std::string right;
   for (auto line = 1;; ++line)
   {
      const auto has_left = static_cast<bool>(std::getline(lhs, left));
      const auto has_right = static_cast<bool>(std::getline(rhs, right));
      if (!has_left && !has_right)
      {
         break;
      }
      if (left != right || has_left != has_right)
      {
         std::println(stderr, "line {} differs:\n  actual:   {}\n  expected: {}", line, left, right);
         break;
      }
   }
   return 1;
}
