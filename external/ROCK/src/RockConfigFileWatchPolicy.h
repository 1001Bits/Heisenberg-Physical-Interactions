#pragma once

#include <exception>
#include <utility>

namespace rock::config_file_watch_policy
{
    enum class PreflightDecision
    {
        Start,
        SkipUnresolvedPath,
        SkipUnavailableTarget,
        SkipStatusError,
    };

    [[nodiscard]] constexpr PreflightDecision decidePreflight(
        const bool pathResolved,
        const bool targetIsRegularFile,
        const bool statusFailed) noexcept
    {
        if (!pathResolved) {
            return PreflightDecision::SkipUnresolvedPath;
        }
        if (statusFailed) {
            return PreflightDecision::SkipStatusError;
        }
        if (!targetIsRegularFile) {
            return PreflightDecision::SkipUnavailableTarget;
        }
        return PreflightDecision::Start;
    }

    // FileWatch may throw while resolving/opening the target after a successful
    // preflight (for example when the file is removed between the two calls).
    // A worker-thread exception must never escape: std::thread would translate
    // it into std::terminate and take down the game process.
    template <class Factory>
    [[nodiscard]] std::exception_ptr captureFactoryFailure(
        Factory&& factory) noexcept
    {
        try {
            std::forward<Factory>(factory)();
            return {};
        } catch (...) {
            return std::current_exception();
        }
    }
}
