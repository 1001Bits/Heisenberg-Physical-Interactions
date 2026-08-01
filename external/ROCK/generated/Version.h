#pragma once
// Static stand-in for ROCK's CMake-generated Version.h (cmake/Version.h.in).
// Version:: symbols are only referenced in ROCKMain.cpp, which the rock_engine
// target excludes; the PCH still #includes this header, so it must exist.
#include <cstddef>
#include <string_view>

namespace Version
{
    inline constexpr std::size_t MAJOR = 0;
    inline constexpr std::size_t MINOR = 5;
    inline constexpr std::size_t PATCH = 0;
    inline constexpr std::string_view NAME = "0.5.0";
    inline constexpr std::string_view PROJECT = "ROCK";
}
