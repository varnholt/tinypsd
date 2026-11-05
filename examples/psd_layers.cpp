// lists the layers of a psd file and writes each one, and the composite, as a png.
// usage: psd_layers <file.psd> [output directory]

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tinypsd/tinypsd.hpp>

namespace
{

uint32_t crc32(std::span<const uint8_t> bytes, uint32_t crc = 0)
{
   crc = ~crc;
   for (const auto byte : bytes)
   {
      crc ^= byte;
      for (auto bit = 0; bit < 8; ++bit)
      {
         crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
      }
   }
   return ~crc;
}

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
   for (const auto shift : {24, 16, 8, 0})
   {
      out.push_back(static_cast<uint8_t>(value >> shift));
   }
}

void appendChunk(std::vector<uint8_t>& png, std::string_view type, std::span<const uint8_t> data)
{
   appendU32(png, static_cast<uint32_t>(data.size()));
   std::vector<uint8_t> body(type.begin(), type.end());
   body.insert(body.end(), data.begin(), data.end());
   png.insert(png.end(), body.begin(), body.end());
   appendU32(png, crc32(body));
}

/// \brief an uncompressed rgba png (stored deflate blocks), so the example needs no zlib.
bool writePng(const std::filesystem::path& path, const tinypsd::Image& image)
{
   std::vector<uint8_t> raw;
   for (auto y = 0; y < image._height; ++y)
   {
      raw.push_back(0);  // no filter
      const auto row = image.row(y);
      raw.insert(raw.end(), row.begin(), row.end());
   }

   std::vector<uint8_t> zlib{0x78, 0x01};
   for (std::size_t offset = 0; offset < raw.size() || offset == 0;)
   {
      const auto length = std::min<std::size_t>(raw.size() - offset, 65535);
      zlib.push_back(offset + length == raw.size() ? 1 : 0);
      for (const auto value : {length, length >> 8, ~length, ~length >> 8})
      {
         zlib.push_back(static_cast<uint8_t>(value));
      }
      zlib.insert(
         zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.begin() + static_cast<std::ptrdiff_t>(offset + length)
      );
      offset += length;
      if (length == 0)
      {
         break;
      }
   }
   uint32_t a = 1;
   uint32_t b = 0;
   for (const auto value : raw)
   {
      a = (a + value) % 65521;
      b = (b + a) % 65521;
   }
   appendU32(zlib, (b << 16) | a);

   std::vector<uint8_t> header;
   appendU32(header, static_cast<uint32_t>(image._width));
   appendU32(header, static_cast<uint32_t>(image._height));
   header.insert(header.end(), {8, 6, 0, 0, 0});  // 8 bit rgba, deflate, no filter, no interlace

   std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
   appendChunk(png, "IHDR", header);
   appendChunk(png, "IDAT", zlib);
   appendChunk(png, "IEND", {});

   std::ofstream file{path, std::ios::binary};
   file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
   return static_cast<bool>(file);
}

/// \brief keeps layer names usable as file names.
std::string fileName(std::size_t index, std::string_view name)
{
   std::string safe;
   for (const auto character : name)
   {
      const auto keep = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                        (character >= '0' && character <= '9') || character == '-' || character == '_';
      safe += keep ? character : '_';
   }
   return std::format("layer_{:03}_{}.png", index, safe);
}

}  // namespace

int main(int argument_count, char** arguments)
{
   if (argument_count < 2)
   {
      std::println("usage: psd_layers <file.psd> [output directory]");
      return 2;
   }

   const auto document = tinypsd::loadFile(arguments[1]);
   if (!document)
   {
      std::println(stderr, "{}: {}", arguments[1], tinypsd::describe(document.error()));
      return 1;
   }

   const auto& header = document->_header;
   std::println(
      "{} x {}, {} bit, {} channels, {} layers", header._width, header._height, header._depth, header._channels, document->_layers.size()
   );

   // photoshop stores the layers bottom to top; show them like its layers panel, top first
   for (auto index = document->_layers.size(); index-- > 0;)
   {
      const auto& layer = document->_layers[index];
      const auto kind = layer.isSectionDivider() ? "group" : layer.isImageLayer() ? "pixels" : "empty";
      std::println(
         "  [{:3}] {:<8} {:<30} {:4} x {:<4} at ({}, {}) opacity {:3}{}",
         index,
         kind,
         layer._name,
         layer._bounds.width(),
         layer._bounds.height(),
         layer._bounds._left,
         layer._bounds._top,
         layer._opacity,
         layer._visible ? "" : " hidden"
      );
   }

   if (argument_count < 3)
   {
      return 0;
   }

   const std::filesystem::path output{arguments[2]};
   std::filesystem::create_directories(output);
   auto ok = true;
   for (std::size_t index = 0; index < document->_layers.size(); ++index)
   {
      const auto& layer = document->_layers[index];
      if (!layer._image.empty())
      {
         ok = writePng(output / fileName(index, layer._name), layer._image) && ok;
      }
   }
   if (!document->_composite.empty())
   {
      ok = writePng(output / "composite.png", document->_composite) && ok;
   }
   std::println("wrote the layers to {}", output.string());
   return ok ? 0 : 1;
}
