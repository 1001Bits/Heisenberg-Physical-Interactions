#pragma once

// C++ standard library
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// F4SE / CommonLibF4 - MUST come before Windows includes
#include <RE/Fallout.h>
#include <F4SE/F4SE.h>

// Windows - MUST come after CommonLibF4
#include <Windows.h>

// spdlog
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

// F4VR Common Framework
#include "f4vr/F4VROffsets.h"

// Macros
#ifndef DLLEXPORT
#define DLLEXPORT __declspec(dllexport)
#endif

using namespace std::literals;
using namespace f4cf;
