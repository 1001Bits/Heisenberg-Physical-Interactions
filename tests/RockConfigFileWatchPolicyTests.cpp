#include "RockConfigFileWatchPolicy.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using namespace rock::config_file_watch_policy;

    Require(
        decidePreflight(false, false, false) ==
            PreflightDecision::SkipUnresolvedPath,
        "an unresolved INI path must not start FileWatch");
    Require(
        decidePreflight(true, false, false) ==
            PreflightDecision::SkipUnavailableTarget,
        "a missing or non-file INI target must run on defaults without FileWatch");
    Require(
        decidePreflight(true, false, true) ==
            PreflightDecision::SkipStatusError,
        "a filesystem status error must fail closed without FileWatch");
    Require(
        decidePreflight(true, true, false) ==
            PreflightDecision::Start,
        "an available regular INI file must retain hot reload");

    int successfulCalls = 0;
    const auto noFailure = captureFactoryFailure([&] { ++successfulCalls; });
    Require(
        !noFailure && successfulCalls == 1,
        "a successful watcher factory must run exactly once");

    const auto missingFileFailure = captureFactoryFailure([] {
        throw std::system_error(
            std::make_error_code(std::errc::no_such_file_or_directory),
            "watch target missing");
    });
    Require(
        static_cast<bool>(missingFileFailure),
        "a FileWatch missing-file exception must be captured");
    try {
        std::rethrow_exception(missingFileFailure);
    } catch (const std::system_error& error) {
        Require(
            error.code() ==
                std::make_error_code(std::errc::no_such_file_or_directory),
            "the captured watcher error must retain its original error code");
    } catch (...) {
        Require(false, "the captured watcher error must retain its original exception type");
    }

    std::exception_ptr workerFailure;
    std::thread worker([&workerFailure]() noexcept {
        workerFailure = captureFactoryFailure([] {
            throw std::system_error(
                std::make_error_code(std::errc::no_such_file_or_directory),
                "watch target removed during startup");
        });
    });
    worker.join();
    Require(
        static_cast<bool>(workerFailure),
        "a watcher exception must not escape its std::thread boundary");

    const auto unknownFailure = captureFactoryFailure([] { throw 17; });
    Require(
        static_cast<bool>(unknownFailure),
        "non-standard watcher failures must also be captured before the thread boundary");

    std::cout << "RockConfig file-watch policy tests passed\n";
    return 0;
}
