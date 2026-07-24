#include "FrikIniCompatibility.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace heisenberg::frik_ini_compat
{
    namespace
    {
        constexpr std::string_view kSection = "Fallout4VRBody";
        constexpr std::string_view kKey = "EnableOffHandGripping";
        constexpr std::string_view kDisabledLine = "EnableOffHandGripping = false";

        struct Replacement
        {
            std::size_t begin;
            std::size_t end;
        };

        bool IsSpace(const char ch)
        {
            return ch == ' ' || ch == '\t';
        }

        char LowerAscii(const char ch)
        {
            if (ch >= 'A' && ch <= 'Z') {
                return static_cast<char>(ch + ('a' - 'A'));
            }
            return ch;
        }

        bool EqualsAsciiInsensitive(const std::string_view lhs, const std::string_view rhs)
        {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (LowerAscii(lhs[i]) != LowerAscii(rhs[i])) {
                    return false;
                }
            }
            return true;
        }

        std::pair<std::size_t, std::size_t> TrimBounds(
            const std::string_view text,
            std::size_t begin,
            std::size_t end)
        {
            while (begin < end && IsSpace(text[begin])) {
                ++begin;
            }
            while (end > begin && IsSpace(text[end - 1])) {
                --end;
            }
            return { begin, end };
        }

        bool IsFalseToken(const std::string_view token)
        {
            return EqualsAsciiInsensitive(token, "false") ||
                EqualsAsciiInsensitive(token, "no") ||
                EqualsAsciiInsensitive(token, "off") ||
                token == "0";
        }

        std::string_view NewlineFor(const std::string_view text)
        {
            return text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        }

        void EnsureEndsWithNewline(std::string& text, const std::string_view newline)
        {
            if (!text.empty() && text.back() != '\n') {
                text.append(newline);
            }
        }
    }

    RewriteResult DisableOffHandGripping(std::string& iniText)
    {
        const std::string_view original{ iniText };
        const std::string_view newline = NewlineFor(original);
        std::vector<Replacement> replacements;

        bool targetSectionFound = false;
        bool inTargetSection = false;
        bool keyFound = false;
        std::size_t targetSectionInsertAt = std::string::npos;

        std::size_t lineStart = 0;
        while (lineStart <= original.size()) {
            const std::size_t newlineAt = original.find('\n', lineStart);
            const std::size_t nextLine = newlineAt == std::string_view::npos ? original.size() : newlineAt + 1;
            std::size_t lineEnd = newlineAt == std::string_view::npos ? original.size() : newlineAt;
            if (lineEnd > lineStart && original[lineEnd - 1] == '\r') {
                --lineEnd;
            }

            std::size_t parseStart = lineStart;
            if (lineStart == 0 && lineEnd >= 3 &&
                static_cast<unsigned char>(original[0]) == 0xEF &&
                static_cast<unsigned char>(original[1]) == 0xBB &&
                static_cast<unsigned char>(original[2]) == 0xBF) {
                parseStart = 3;
            }

            const auto [trimmedStart, trimmedEnd] = TrimBounds(original, parseStart, lineEnd);
            if (trimmedStart < trimmedEnd && original[trimmedStart] != ';' && original[trimmedStart] != '#') {
                if (original[trimmedStart] == '[') {
                    if (inTargetSection && targetSectionInsertAt == std::string::npos) {
                        targetSectionInsertAt = lineStart;
                    }

                    const std::size_t close = original.find(']', trimmedStart + 1);
                    if (close != std::string_view::npos && close < trimmedEnd) {
                        const auto [nameStart, nameEnd] = TrimBounds(original, trimmedStart + 1, close);
                        inTargetSection = EqualsAsciiInsensitive(original.substr(nameStart, nameEnd - nameStart), kSection);
                        targetSectionFound = targetSectionFound || inTargetSection;
                    } else {
                        inTargetSection = false;
                    }
                } else if (inTargetSection) {
                    const std::size_t equals = original.find('=', trimmedStart);
                    if (equals != std::string_view::npos && equals < trimmedEnd) {
                        const auto [keyStart, keyEnd] = TrimBounds(original, trimmedStart, equals);
                        if (EqualsAsciiInsensitive(original.substr(keyStart, keyEnd - keyStart), kKey)) {
                            keyFound = true;
                            std::size_t valueStart = equals + 1;
                            while (valueStart < lineEnd && IsSpace(original[valueStart])) {
                                ++valueStart;
                            }

                            std::size_t valueEnd = valueStart;
                            while (valueEnd < lineEnd &&
                                !IsSpace(original[valueEnd]) &&
                                original[valueEnd] != ';' &&
                                original[valueEnd] != '#') {
                                ++valueEnd;
                            }

                            const std::string_view token = original.substr(valueStart, valueEnd - valueStart);
                            if (!IsFalseToken(token)) {
                                replacements.push_back({ valueStart, valueEnd });
                            }
                        }
                    }
                }
            }

            if (newlineAt == std::string_view::npos) {
                break;
            }
            lineStart = nextLine;
        }

        if (!replacements.empty()) {
            std::sort(replacements.begin(), replacements.end(), [](const Replacement& lhs, const Replacement& rhs) {
                return lhs.begin > rhs.begin;
            });
            for (const auto& replacement : replacements) {
                iniText.replace(replacement.begin, replacement.end - replacement.begin, "false");
            }
            return RewriteResult::UpdatedExistingKey;
        }

        if (keyFound) {
            return RewriteResult::AlreadyDisabled;
        }

        if (targetSectionFound) {
            if (targetSectionInsertAt == std::string::npos) {
                EnsureEndsWithNewline(iniText, newline);
                targetSectionInsertAt = iniText.size();
            }
            std::string insertion;
            if (targetSectionInsertAt > 0 && iniText[targetSectionInsertAt - 1] != '\n') {
                insertion.append(newline);
            }
            insertion.append(kDisabledLine);
            insertion.append(newline);
            iniText.insert(targetSectionInsertAt, insertion);
            return RewriteResult::AddedMissingKey;
        }

        EnsureEndsWithNewline(iniText, newline);
        if (!iniText.empty() && iniText.size() >= newline.size() &&
            iniText.compare(iniText.size() - newline.size(), newline.size(), newline) == 0) {
            iniText.append(newline);
        }
        iniText.append("[Fallout4VRBody]");
        iniText.append(newline);
        iniText.append(kDisabledLine);
        iniText.append(newline);
        return RewriteResult::AddedMissingSection;
    }

    bool WriteFileAtomic(const std::filesystem::path& path, const std::string_view contents, std::string& outError)
    {
        std::error_code ec;

        // Same directory (and therefore same volume) as `path` so the final rename below is a
        // single atomic filesystem op rather than a cross-volume copy+delete.
        std::filesystem::path tempPath{ path };
        tempPath += L".tmp";

        {
            std::ofstream temp(tempPath, std::ios::binary | std::ios::trunc);
            if (!temp) {
                outError = "could not create temp file '" + tempPath.string() + "'";
                return false;
            }
            temp.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            temp.flush();
            if (!temp.good()) {
                outError = "failed while writing temp file '" + tempPath.string() + "'";
                temp.close();
                std::filesystem::remove(tempPath, ec);  // best-effort cleanup; original untouched either way
                return false;
            }
            // temp closes here (RAII, end of scope) -- required before rename on Windows, which
            // cannot replace a file that is still open for write.
        }

        // Sanity-check what actually landed on disk before we let it anywhere near the original.
        const std::uintmax_t writtenSize = std::filesystem::file_size(tempPath, ec);
        if (ec || writtenSize != contents.size()) {
            outError = "temp file size mismatch after write for '" + tempPath.string() + "'";
            std::filesystem::remove(tempPath, ec);
            return false;
        }

        // std::filesystem::rename is required by the standard to atomically replace an existing
        // regular-file destination; on Windows this is implemented via MoveFileExW with
        // MOVEFILE_REPLACE_EXISTING. `path` is never opened/truncated directly by this function --
        // it either ends up fully replaced by the verified temp content, or (on any failure above)
        // left completely as-is.
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            outError = "atomic rename to '" + path.string() + "' failed: " + ec.message();
            std::filesystem::remove(tempPath, ec);
            return false;
        }

        return true;
    }
}
