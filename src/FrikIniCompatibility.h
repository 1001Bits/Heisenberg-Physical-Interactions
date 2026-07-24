#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace heisenberg::frik_ini_compat
{
    enum class RewriteResult
    {
        AlreadyDisabled,
        UpdatedExistingKey,
        AddedMissingKey,
        AddedMissingSection
    };

    // Enforce the setting FRIK itself reads for its legacy two-handed weapon solver.
    // The rewrite is deliberately text based so unrelated settings, comments, ordering,
    // newline style, and UTF-8 content remain byte-for-byte unchanged.
    RewriteResult DisableOffHandGripping(std::string& iniText);

    // Atomically replace the contents of `path` with `contents`.
    //
    // Writes to a temp file adjacent to `path` (same directory/volume, so the final rename is
    // atomic), verifies the write landed on disk, then renames the temp file over `path` via
    // std::filesystem::rename (MoveFileEx w/ MOVEFILE_REPLACE_EXISTING on Windows). `path` is
    // NEVER opened for writing directly and is left completely untouched if anything fails along
    // the way -- callers that previously truncated `path` in place risked leaving it empty/partial
    // on a mid-write failure (disk full, AV/OneDrive scan, a sharing violation from another
    // watcher). On failure `outError` is set to a human-readable reason; on success it is
    // untouched. Shared by every ini-writer in this mod that targets FRIK.ini so they all get the
    // same atomicity guarantee instead of each reimplementing it.
    bool WriteFileAtomic(const std::filesystem::path& path, std::string_view contents, std::string& outError);
}
