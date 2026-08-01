#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace rock::frik_weapon_offset_key_policy
{
    // Editor IDs are stable but commonly omit the spaces and punctuation used
    // by FRIK's display-name keys (for example AssaultRifle vs Assault Rifle).
    // This projection is deliberately ASCII-oriented: editor IDs and FRIK's
    // canonical resource keys use this subset, while localized display names
    // continue through their exact lookup path.
    inline std::string normalizeEditorIdKey(std::string_view key)
    {
        std::string normalized;
        normalized.reserve(key.size());
        for (const char character : key) {
            const auto byte =
                static_cast<unsigned char>(character);
            if (std::isalnum(byte) == 0) {
                continue;
            }
            normalized.push_back(
                static_cast<char>(std::tolower(byte)));
        }
        return normalized;
    }

    // A normalized key is usable only when it identifies exactly one raw FRIK
    // key. Collisions such as "Assault Rifle" and "Assault-Rifle" fail closed
    // instead of selecting an unordered-map-dependent placement.
    template <class Value>
    class UniqueNormalizedIndex
    {
    public:
        void reserve(std::size_t count)
        {
            _entries.reserve(count);
        }

        void add(std::string_view rawKey, const Value& value)
        {
            std::string normalized =
                normalizeEditorIdKey(rawKey);
            if (normalized.empty()) {
                return;
            }

            auto [entry, inserted] =
                _entries.try_emplace(
                    std::move(normalized),
                    Entry{ .value = value });
            if (!inserted) {
                entry->second.ambiguous = true;
            }
        }

        [[nodiscard]] std::optional<Value> find(
            std::string_view editorIdKey) const
        {
            const std::string normalized =
                normalizeEditorIdKey(editorIdKey);
            if (normalized.empty()) {
                return std::nullopt;
            }

            const auto found = _entries.find(normalized);
            if (found == _entries.end() ||
                found->second.ambiguous) {
                return std::nullopt;
            }
            return found->second.value;
        }

    private:
        struct Entry
        {
            Value value{};
            bool ambiguous = false;
        };

        std::unordered_map<std::string, Entry> _entries;
    };
}
