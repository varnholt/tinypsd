#include <array>
#include <cstddef>
#include <print>

#include <tinypsd/tinypsd.hpp>

int main()
{
   // not a psd file: proves the header compiles, links and reports errors
   constexpr std::array<std::byte, 4> data{std::byte{'G'}, std::byte{'I'}, std::byte{'F'}, std::byte{'8'}};
   const auto document = tinypsd::load(data);
   std::println(
      "tinypsd {}.{}.{}: {}",
      tinypsd::version.major,
      tinypsd::version.minor,
      tinypsd::version.patch,
      document ? "loaded" : tinypsd::describe(document.error())
   );
   return document ? 1 : 0;
}
