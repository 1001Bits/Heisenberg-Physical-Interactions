#pragma once

// Heisenberg shim for ROCK's PhysicsLog.h: maps ROCK_LOG_*(Category, fmt, args...) onto spdlog.
// The category token is kept as a bracketed prefix string so log lines stay greppable.

#include <spdlog/spdlog.h>

#define ROCK_LOG_TRACE(cat, ...)    ::spdlog::trace("[" #cat "] " __VA_ARGS__)
#define ROCK_LOG_DEBUG(cat, ...)    ::spdlog::debug("[" #cat "] " __VA_ARGS__)
#define ROCK_LOG_INFO(cat, ...)     ::spdlog::info("[" #cat "] " __VA_ARGS__)
#define ROCK_LOG_WARN(cat, ...)     ::spdlog::warn("[" #cat "] " __VA_ARGS__)
#define ROCK_LOG_ERROR(cat, ...)    ::spdlog::error("[" #cat "] " __VA_ARGS__)
#define ROCK_LOG_CRITICAL(cat, ...) ::spdlog::critical("[" #cat "] " __VA_ARGS__)

// Sampled variant (cat, intervalMs, fmt, args...). We drop the throttle interval and log every
// call at debug level — these only fire on grab events, not per-frame, so volume is fine.
#define ROCK_LOG_SAMPLE_DEBUG(cat, intervalMs, ...) ::spdlog::debug("[" #cat "] " __VA_ARGS__)
