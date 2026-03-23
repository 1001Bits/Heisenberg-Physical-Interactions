#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace heisenberg
{
    // =========================================================================
    // SharedUtils — common utility functions used across multiple handlers
    // =========================================================================

    /**
     * Case-insensitive substring search.
     * Returns true if 'needle' appears anywhere in 'haystack' (ignoring case).
     */
    inline bool ContainsCI(const char* haystack, const char* needle)
    {
        if (!haystack || !needle) return false;
        std::string h(haystack);
        std::string n(needle);
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
        return h.find(n) != std::string::npos;
    }

    /**
     * Parse a hex form ID string like "0x0020DE62" or "0020DE62".
     * Returns 0 on failure.
     */
    inline std::uint32_t ParseHexFormID(const std::string& str)
    {
        if (str.empty()) return 0;

        try {
            // Handle "0x" prefix
            if (str.length() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
                return static_cast<std::uint32_t>(std::stoul(str.substr(2), nullptr, 16));
            }
            return static_cast<std::uint32_t>(std::stoul(str, nullptr, 16));
        } catch (...) {
            return 0;
        }
    }

    /**
     * Recursively search a NiAVObject tree for a node with the given name.
     * Returns the matching node, or nullptr if not found.
     */
    inline RE::NiAVObject* FindNodeByName(RE::NiAVObject* root, const std::string& nodeName)
    {
        if (!root) {
            return nullptr;
        }

        // Check if this node matches
        if (root->name.c_str() && nodeName == root->name.c_str()) {
            return root;
        }

        // If it's a NiNode, search children
        if (auto* niNode = root->IsNode()) {
            for (auto& child : niNode->children) {
                if (child) {
                    RE::NiAVObject* found = FindNodeByName(child.get(), nodeName);
                    if (found) {
                        return found;
                    }
                }
            }
        }

        return nullptr;
    }

    /**
     * Collect all node names from a NiAVObject tree for logging.
     *
     * @param node           The root node to start from
     * @param outNames       Output vector of formatted node name strings
     * @param depth          Current recursion depth (used for indentation)
     * @param includeWorldPos  If true, append world position "@ (x, y, z)" to each entry
     */
    inline void CollectNodeNamesRecursive(RE::NiAVObject* node, std::vector<std::string>& outNames,
                                          int depth = 0, bool includeWorldPos = false)
    {
        if (!node || depth > 10) {
            return;
        }

        std::string indent(depth * 2, ' ');
        std::string name = node->name.c_str() ? node->name.c_str() : "(unnamed)";

        if (includeWorldPos) {
            const auto& pos = node->world.translate;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%s%s @ (%.1f, %.1f, %.1f)",
                indent.c_str(), name.c_str(), pos.x, pos.y, pos.z);
            outNames.push_back(buffer);
        } else {
            outNames.push_back(indent + name);
        }

        if (auto* niNode = node->IsNode()) {
            for (auto& child : niNode->children) {
                if (child) {
                    CollectNodeNamesRecursive(child.get(), outNames, depth + 1, includeWorldPos);
                }
            }
        }
    }

    /**
     * Log the entire node tree of a TESObjectREFR for discovery/debugging.
     *
     * @param ref             The object reference whose 3D tree to log
     * @param logTag          Log prefix tag, e.g. "[ActivatorHandler]" or "[ItemInsertHandler]"
     * @param includeWorldPos If true, include world positions in the node listing
     */
    inline void LogRefNodeTree(RE::TESObjectREFR* ref, const char* logTag, bool includeWorldPos = false)
    {
        if (!ref) {
            return;
        }

        auto* node3D = ref->Get3D();
        if (!node3D) {
            spdlog::info("{} Ref {:08X} has no 3D node", logTag, ref->GetFormID());
            return;
        }

        std::uint32_t baseFormID = 0;
        const char* editorID = "";
        if (const auto baseObj = ref->GetObjectReference()) {
            baseFormID = baseObj->GetFormID();
            if (baseObj->GetFormEditorID()) {
                editorID = baseObj->GetFormEditorID();
            }
        }

        spdlog::info("{} ====== NODE TREE FOR {:08X} (base {:08X} '{}') ======",
            logTag, ref->GetFormID(), baseFormID, editorID);

        std::vector<std::string> nodeNames;
        CollectNodeNamesRecursive(node3D, nodeNames, 0, includeWorldPos);

        for (const auto& name : nodeNames) {
            spdlog::info("{}   {}", logTag, name);
        }

        spdlog::info("{} ====== END NODE TREE ({} nodes) ======", logTag, nodeNames.size());
    }

}  // namespace heisenberg
