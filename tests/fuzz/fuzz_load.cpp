// libFuzzer entry point: every input must load or fail cleanly, without sanitizer reports.
//   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DTINYPSD_BUILD_FUZZER=ON
//   ./build-fuzz/tests/fuzz/tinypsd_fuzz tests/data/psd-tools -max_total_time=60

#include <cstddef>
#include <cstdint>
#include <span>

#include <tinypsd/tinypsd.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
   // a small pixel cap keeps each run fast; the size checks are what is being fuzzed, not big allocations
   const auto document = tinypsd::load(std::as_bytes(std::span{data, size}), {._max_pixels = std::size_t{1} << 22});
   if (document)
   {
      for (const auto& layer : document->_layers)
      {
         if (layer._image._pixels.size() !=
             static_cast<std::size_t>(layer._image._width) * static_cast<std::size_t>(layer._image._height) * 4)
         {
            __builtin_trap();
         }
      }
   }
   return 0;
}
