#include "ItemOffsets.h"
#include "ShapeReferences.h"
#include "EmbeddedOffsets.h"
#include "F4VROffsets.h"
#include "Utils.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace heisenberg
{
    // Path relative to game folder for item offsets
    constexpr auto OFFSETS_FOLDER = "Data\\F4SE\\Plugins\\Heisenberg\\item_offsets";
    
    // Helper to strip [Category] prefix and {{{Components}}} suffix from item names
    // e.g., "[Scrap] Boston Bugle{{{Cloth}}}" -> "Boston Bugle"
    static std::string StripCategoryPrefix(const std::string& name)
    {
        std::string result = name;
        
        // 1. Strip [Category] prefix
        // Look for pattern: [Something] ActualName
        if (result.size() > 2 && result[0] == '[')
        {
            size_t closeBracket = result.find(']');
            if (closeBracket != std::string::npos && closeBracket + 1 < result.size())
            {
                // Skip the bracket and any space after it
                size_t start = closeBracket + 1;
                while (start < result.size() && result[start] == ' ')
                    start++;
                if (start < result.size())
                    result = result.substr(start);
            }
        }
        
        // 2. Strip {{{Components}}} suffix
        // Look for pattern: ItemName{{{Something}}}
        size_t braceStart = result.find("{{{");
        if (braceStart != std::string::npos)
        {
            result = result.substr(0, braceStart);
        }
        
        return result;
    }

    // Convert string to lowercase
    static std::string ToLower(const std::string& str)
    {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }

    // Extract category prefix from item name (e.g., "[Drink] Nuka-Cola" -> "Drink")
    static std::string ExtractCategoryPrefix(const std::string& name)
    {
        if (name.size() > 2 && name[0] == '[')
        {
            size_t closeBracket = name.find(']');
            if (closeBracket != std::string::npos && closeBracket > 1)
            {
                return name.substr(1, closeBracket - 1);
            }
        }
        return "";
    }

    // Dominant axis enumeration for shape matching
    enum class DominantAxis { LENGTH, WIDTH, HEIGHT, NONE };

    // Get dominant axis (which dimension is longest)
    static DominantAxis GetDominantAxis(float L, float W, float H)
    {
        float maxDim = (std::max)({L, W, H});
        float minDim = (std::min)({L, W, H});
        
        // If difference is small (<20%), object is roughly cubic/spherical
        if (maxDim > 0 && (maxDim - minDim) / maxDim < 0.2f)
            return DominantAxis::NONE;
        
        if (L >= W && L >= H) return DominantAxis::LENGTH;
        if (W >= L && W >= H) return DominantAxis::WIDTH;
        return DominantAxis::HEIGHT;
    }

    // Check if two items have same dominant axis
    static bool SameDominantAxis(DominantAxis a, DominantAxis b)
    {
        // NONE matches anything (cubes/spheres are orientation-agnostic)
        if (a == DominantAxis::NONE || b == DominantAxis::NONE)
            return true;
        return a == b;
    }

    // Calculate partial name match score (uses real item name only, not category/materials)
    static float GetNameMatchScore(const std::string& itemName, const std::string& candidateName)
    {
        // Extract real names only (no category, no materials)
        std::string realItem = StripCategoryPrefix(itemName);
        std::string realCand = StripCategoryPrefix(candidateName);
        
        if (realItem.empty() || realCand.empty())
            return 0.0f;
        
        // Case-insensitive comparison
        std::string lowerItem = ToLower(realItem);
        std::string lowerCand = ToLower(realCand);
        
        // Exact match (after stripping category/materials)
        if (lowerItem == lowerCand)
            return 1.0f;
        
        // Substring match ("Nuka-Cola" in "Nuka-Cola Quantum")
        if (lowerItem.find(lowerCand) != std::string::npos ||
            lowerCand.find(lowerItem) != std::string::npos)
        {
            return 0.85f;
        }
        
        // Shared prefix >= 4 chars ("Buff" in "Buffout" and "Bufftats")
        size_t prefixLen = 0;
        while (prefixLen < lowerItem.size() && prefixLen < lowerCand.size() &&
               lowerItem[prefixLen] == lowerCand[prefixLen])
        {
            prefixLen++;
        }
        if (prefixLen >= 4)
            return 0.70f;
        
        // Shared suffix >= 4 chars ("On A Stick")
        size_t suffixLen = 0;
        while (suffixLen < lowerItem.size() && suffixLen < lowerCand.size() &&
               lowerItem[lowerItem.size() - 1 - suffixLen] == 
               lowerCand[lowerCand.size() - 1 - suffixLen])
        {
            suffixLen++;
        }
        if (suffixLen >= 4)
            return 0.70f;
        
        return 0.0f;  // No name match
    }

    // Calculate match score between an item and a candidate offset
    // Returns: score from 0.0 (no match) to 1.0 (perfect match)
    // 
    // Priority matching (see copilot-instructions.md):
    // Priority 3-6: Exact Dims variations = 1.0
    // Priority 7: Partial Name Match = 0.85
    // Priority 8-13: Similar Dims variations = 0.55 to 0.95
    // Priority 14-15: Same Ratio Only = 0.20 to 0.45
    static float CalculateMatchScore(
        const ItemOffset& candidate,
        const std::string& candidateName,
        float L, float W, float H,
        const std::string& itemName,
        const std::string& category,
        const std::string& itemType)
    {
        // === DIMENSION MATCHING ===
        float dL = std::abs(candidate.length - L);
        float dW = std::abs(candidate.width - W);
        float dH = std::abs(candidate.height - H);
        
        // === RATIO CALCULATION ===
        float minDim = (std::max)((std::min)({L, W, H}), 1.0f);
        float ratioL = L / minDim;
        float ratioW = W / minDim;
        float ratioH = H / minDim;
        
        float candMin = (std::max)((std::min)({candidate.length, candidate.width, candidate.height}), 1.0f);
        float candRatioL = candidate.length / candMin;
        float candRatioW = candidate.width / candMin;
        float candRatioH = candidate.height / candMin;
        
        float ratioDiff = (std::abs(ratioL - candRatioL) + 
                           std::abs(ratioW - candRatioW) + 
                           std::abs(ratioH - candRatioH)) / 3.0f;
        bool sameRatio = ratioDiff < 0.3f;
        
        // === DOMINANT AXIS ===
        DominantAxis itemAxis = GetDominantAxis(L, W, H);
        DominantAxis candAxis = GetDominantAxis(candidate.length, candidate.width, candidate.height);
        bool sameAxis = SameDominantAxis(itemAxis, candAxis);
        
        // === CATEGORY/TYPE ===
        std::string candCategory = ExtractCategoryPrefix(candidateName);
        bool sameCat = !category.empty() && category == candCategory;
        bool sameType = !itemType.empty() && !candidate.itemType.empty() && itemType == candidate.itemType;
        
        // === SCORING (following exact priority list) ===
        
        // Priority 3-6: Exact dimensions (truly exact - NO tolerance)
        if (dL == 0.0f && dW == 0.0f && dH == 0.0f)
        {
            return 1.0f;  // Exact dims always score 1.0
        }
        
        // Priority 8-13: Similar dimensions (within 25% each axis)
        if (L > 0.0f && W > 0.0f && H > 0.0f &&
            dL / L <= 0.25f && dW / W <= 0.25f && dH / H <= 0.25f)
        {
            // Priority 8: Similar Dims + Same Ratio + Same Axis + Same Cat + Same Type = 0.95
            if (sameRatio && sameAxis && sameCat && sameType)
                return 0.95f;
            // Priority 9: Similar Dims + Same Ratio + Same Axis + Same Cat = 0.90
            if (sameRatio && sameAxis && sameCat)
                return 0.90f;
            // Priority 10: Similar Dims + Same Ratio + Same Axis = 0.80-0.85
            if (sameRatio && sameAxis)
                return 0.80f;
            // Priority 11: Similar Dims + Same Ratio + Diff Axis = 0.75-0.80
            if (sameRatio && !sameAxis)
                return 0.75f;
            // Priority 12: Similar Dims + Diff Ratio + Same Axis = 0.60-0.70
            if (!sameRatio && sameAxis)
                return 0.60f;
            // Priority 13: Similar Dims + Diff Ratio + Diff Axis = 0.55-0.65
            return 0.55f;
        }
        
        // Priority 14-15: Same Ratio Only (size differs significantly)
        if (sameRatio)
        {
            // Priority 14: Same Ratio Only + Same Axis = 0.30-0.45
            if (sameAxis)
                return 0.30f;
            // Priority 15: Same Ratio Only + Diff Axis = 0.20-0.35
            return 0.20f;
        }
        
        // No match
        return 0.0f;
    }

    std::string ItemOffsetManager::GetOffsetsPath()
    {
        return OFFSETS_FOLDER;
    }

    void ItemOffsetManager::Initialize()
    {
        if (_initialized)
            return;

        _initialized = true;

        // Set default offset - palm center position
        // In wand-local space: X=right, Y=forward (toward fingers), Z=up
        // These values position the object in the palm, ~5cm from finger bases
        _defaultOffset.position = RE::NiPoint3(0.0f, 5.0f, 3.5f);  // Forward into palm, slightly up
        _defaultOffset.rotation.MakeIdentity();
        _defaultOffset.scale = 1.0f;

        // Create offsets folder if it doesn't exist (for user-saved offsets)
        try
        {
            fs::create_directories(GetOffsetsPath());
        }
        catch (const std::exception& e)
        {
            spdlog::warn("[ItemOffsets] Failed to create offsets folder: {}", e.what());
        }

        // Load embedded offsets first (pre-configured items)
        LoadEmbeddedOffsets();
        spdlog::info("[ItemOffsets] Loaded {} embedded offsets", _offsets.size());

        // Then load user overrides from filesystem (can override embedded offsets)
        size_t countBefore = _offsets.size();
        LoadOffsetsFromFilesystem();
        size_t userOffsets = _offsets.size() - countBefore;
        
        spdlog::info("[ItemOffsets] Initialized with {} total offsets ({} embedded + {} user)", 
                     _offsets.size(), EmbeddedOffsets::kOffsetCount, userOffsets);
    }

    void ItemOffsetManager::LoadEmbeddedOffsets()
    {
        for (const auto& data : EmbeddedOffsets::kOffsets)
        {
            ItemOffset offset;
            
            // Position
            offset.position.x = data.posX;
            offset.position.y = data.posY;
            offset.position.z = data.posZ;
            
            // Rotation (3x4 matrix, row-major)
            for (int i = 0; i < 3; i++) {
                for (int k = 0; k < 4; k++) {
                    offset.rotation[i][k] = data.rot[i * 4 + k];
                }
            }
            
            // Scale and dimensions
            offset.scale = data.scale;
            offset.length = data.dimL;
            offset.width = data.dimW;
            offset.height = data.dimH;
            
            // Finger settings
            offset.fingerDistance = data.fingerDistance;
            offset.thumbCurl = data.fingerCurls[0];
            offset.indexCurl = data.fingerCurls[1];
            offset.middleCurl = data.fingerCurls[2];
            offset.ringCurl = data.fingerCurls[3];
            offset.pinkyCurl = data.fingerCurls[4];
            offset.hasFingerCurls = true;
            
            // Metadata
            offset.itemType = std::string(data.itemType);
            offset.formId = std::string(data.formId);
            offset.isRightHandSpace = data.isRightHandSpace;
            offset.isFRIKOffset = data.isFRIKOffset;
            offset.isLeftHanded = data.isLeftHanded;
            offset.isPowerArmor = data.isPowerArmor;
            offset.isThrowable = data.isThrowable;
            
            // Build the lookup key based on flags
            // Format: BaseName[_L][_PA][_T] where:
            //   _L = left-handed, _PA = power armor, _T = throwable
            std::string cleanName = StripCategoryPrefix(std::string(data.name));
            std::string lookupKey = cleanName;
            if (data.isLeftHanded) lookupKey += "_L";
            if (data.isPowerArmor) lookupKey += "_PA";
            if (data.isThrowable) lookupKey += "_T";
            
            _offsets[lookupKey] = offset;
            
            // Update form ID index (only for base offsets, not variants)
            if (!offset.formId.empty() && offset.formId != "00000000" && 
                !data.isLeftHanded && !data.isPowerArmor && !data.isThrowable) {
                _formIdToName[offset.formId] = cleanName;
            }
        }
    }

    void ItemOffsetManager::LoadOffsetsFromFilesystem()
    {
        const auto path = GetOffsetsPath();
        
        if (!fs::exists(path))
        {
            spdlog::info("[ItemOffsets] Offsets folder does not exist: {}", path);
            return;
        }

        try
        {
            for (const auto& entry : fs::directory_iterator(path))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".json")
                {
                    LoadOffsetJsonFile(entry.path().string());
                }
            }
        }
        catch (const std::exception& e)
        {
            spdlog::error("[ItemOffsets] Error loading offsets: {}", e.what());
        }
    }

    void ItemOffsetManager::LoadOffsetJsonFile(const std::string& filePath)
    {
        try
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                spdlog::warn("[ItemOffsets] Cannot open file: {}", filePath);
                return;
            }

            json j;
            file >> j;
            file.close();

            // Parse each item in the JSON
            for (auto& [itemName, value] : j.items())
            {
                ItemOffset offset;

                // Load position
                if (value.contains("position"))
                {
                    offset.position.x = value["position"]["x"].get<float>();
                    offset.position.y = value["position"]["y"].get<float>();
                    offset.position.z = value["position"]["z"].get<float>();
                }

                // Load rotation (as 3x4 matrix like FRIK)
                if (value.contains("rotation"))
                {
                    auto& rot = value["rotation"];
                    if (rot.is_array() && rot.size() >= 12)
                    {
                        for (int i = 0; i < 3; i++)
                        {
                            for (int k = 0; k < 4; k++)
                            {
                                offset.rotation[i][k] = rot[i * 4 + k].get<float>();
                            }
                        }
                    }
                }

                // Load scale
                if (value.contains("scale"))
                {
                    offset.scale = value["scale"].get<float>();
                }

                // Load dimensions
                if (value.contains("dimensions"))
                {
                    offset.length = value["dimensions"]["length"].get<float>();
                    offset.width = value["dimensions"]["width"].get<float>();
                    offset.height = value["dimensions"]["height"].get<float>();
                }

                // Load finger distance
                if (value.contains("fingerDistance"))
                {
                    offset.fingerDistance = value["fingerDistance"].get<float>();
                }
                
                // Load finger curl values
                if (value.contains("fingerCurls"))
                {
                    auto& curls = value["fingerCurls"];
                    offset.thumbCurl = curls.value("thumb", 0.5f);
                    offset.indexCurl = curls.value("index", 0.5f);
                    offset.middleCurl = curls.value("middle", 0.5f);
                    offset.ringCurl = curls.value("ring", 0.5f);
                    offset.pinkyCurl = curls.value("pinky", 0.5f);
                    offset.hasFingerCurls = true;
                }

                // Load per-joint curl values (15 floats)
                if (value.contains("jointCurls") && value["jointCurls"].is_array() && value["jointCurls"].size() == 15)
                {
                    for (int ji = 0; ji < 15; ji++) {
                        offset.jointCurls[ji] = value["jointCurls"][ji].get<float>();
                    }
                    offset.hasJointCurls = true;
                }
                
                // Load metadata
                if (value.contains("itemType"))
                {
                    offset.itemType = value["itemType"].get<std::string>();
                }
                if (value.contains("formId"))
                {
                    offset.formId = value["formId"].get<std::string>();
                }
                
                // Load variant flags (if present)
                if (value.contains("variant"))
                {
                    auto& variant = value["variant"];
                    offset.isLeftHanded = variant.value("isLeftHanded", false);
                    offset.isPowerArmor = variant.value("isPowerArmor", false);
                    offset.isThrowable = variant.value("isThrowable", false);
                }

                // Strip [Category] prefix to get the actual item name for lookup
                // e.g., "[Drink] Nuka-Cherry" -> "Nuka-Cherry"
                std::string cleanName = StripCategoryPrefix(itemName);
                
                _offsets[cleanName] = offset;
                
                // Update form ID index (formId is already loaded from JSON above)
                if (!offset.formId.empty() && offset.formId != "00000000") {
                    _formIdToName[offset.formId] = cleanName;
                }
                
                spdlog::debug("[ItemOffsets] Loaded offset for '{}' (from '{}')", cleanName, itemName);
            }
        }
        catch (const std::exception& e)
        {
            spdlog::error("[ItemOffsets] Error parsing {}: {}", filePath, e.what());
        }
    }

    void ItemOffsetManager::SaveOffsetToJsonFile(const std::string& itemName, const ItemOffset& offset)
    {
        const auto filePath = GetOffsetsPath() + "\\" + itemName + ".json";

        try
        {
            json j;

            // Save item metadata
            j[itemName]["itemType"] = offset.itemType;
            j[itemName]["formId"] = offset.formId;

            // Save position
            j[itemName]["position"]["x"] = offset.position.x;
            j[itemName]["position"]["y"] = offset.position.y;
            j[itemName]["position"]["z"] = offset.position.z;

            // Save rotation as 3x4 matrix
            for (int i = 0; i < 3; i++)
            {
                for (int k = 0; k < 4; k++)
                {
                    j[itemName]["rotation"].push_back(offset.rotation[i][k]);
                }
            }

            // Save scale
            j[itemName]["scale"] = offset.scale;

            // Save dimensions (bounding box)
            j[itemName]["dimensions"]["length"] = offset.length;
            j[itemName]["dimensions"]["width"] = offset.width;
            j[itemName]["dimensions"]["height"] = offset.height;

            // Save finger distance (for automatic finger curl)
            j[itemName]["fingerDistance"] = offset.fingerDistance;
            
            // Save finger curl values if set
            if (offset.hasFingerCurls)
            {
                j[itemName]["fingerCurls"]["thumb"] = offset.thumbCurl;
                j[itemName]["fingerCurls"]["index"] = offset.indexCurl;
                j[itemName]["fingerCurls"]["middle"] = offset.middleCurl;
                j[itemName]["fingerCurls"]["ring"] = offset.ringCurl;
                j[itemName]["fingerCurls"]["pinky"] = offset.pinkyCurl;
            }

            // Save per-joint curl values (15 floats)
            if (offset.hasJointCurls)
            {
                j[itemName]["jointCurls"] = nlohmann::json::array();
                for (int ji = 0; ji < 15; ji++) {
                    j[itemName]["jointCurls"].push_back(offset.jointCurls[ji]);
                }
            }
            
            // Save variant flags (so the offset file indicates which context it was saved in)
            j[itemName]["variant"]["isLeftHanded"] = offset.isLeftHanded;
            j[itemName]["variant"]["isPowerArmor"] = offset.isPowerArmor;
            j[itemName]["variant"]["isThrowable"] = offset.isThrowable;

            // Write to file
            std::ofstream file(filePath);
            if (!file.is_open())
            {
                spdlog::error("[ItemOffsets] Cannot open file for writing: {}", filePath);
                return;
            }

            file << std::setw(4) << j;
            file.close();

            spdlog::info("[ItemOffsets] Saved offset for '{}' [{}] to {} (dims: {:.1f}x{:.1f}x{:.1f})", 
                         itemName, offset.itemType, filePath, offset.length, offset.width, offset.height);
        }
        catch (const std::exception& e)
        {
            spdlog::error("[ItemOffsets] Error saving {}: {}", filePath, e.what());
        }
    }

    std::string ItemOffsetManager::GetItemName(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return "";

        // Try the native GetDisplayFullName method on TESObjectREFR
        // This uses CommonLibF4's REL::RelocationID to find the correct VR offset
        try
        {
            const char* displayName = refr->GetDisplayFullName();
            if (displayName && displayName[0] != '\0')
            {
                return std::string(displayName);
            }
        }
        catch (...)
        {
            // If GetDisplayFullName fails, fall through to fallback
        }

        // Fallback to base form name
        auto* baseForm = refr->GetObjectReference();
        if (baseForm)
        {
            auto fullName = RE::TESFullName::GetFullName(*baseForm, false);
            if (!fullName.empty())
            {
                return std::string(fullName);
            }
            
            // Last resort: form ID as hex string
            char buf[16];
            snprintf(buf, sizeof(buf), "%08X", static_cast<uint32_t>(baseForm->formID));
            return std::string(buf);
        }

        return "";
    }

    std::string ItemOffsetManager::GetItemType(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return "UNKNOWN";

        auto* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return "UNKNOWN";

        // Get form type and convert to string
        auto formType = baseForm->GetFormType();
        switch (formType)
        {
            case RE::ENUM_FORM_ID::kMISC: return "MISC";
            case RE::ENUM_FORM_ID::kWEAP: return "WEAP";
            case RE::ENUM_FORM_ID::kARMO: return "ARMO";
            case RE::ENUM_FORM_ID::kALCH: return "ALCH";
            case RE::ENUM_FORM_ID::kAMMO: return "AMMO";
            case RE::ENUM_FORM_ID::kBOOK: return "BOOK";
            case RE::ENUM_FORM_ID::kINGR: return "INGR";
            case RE::ENUM_FORM_ID::kKEYM: return "KEYM";
            case RE::ENUM_FORM_ID::kNOTE: return "NOTE";
            case RE::ENUM_FORM_ID::kACTI: return "ACTI";
            case RE::ENUM_FORM_ID::kFURN: return "FURN";
            case RE::ENUM_FORM_ID::kCONT: return "CONT";
            case RE::ENUM_FORM_ID::kSTAT: return "STAT";
            case RE::ENUM_FORM_ID::kMSTT: return "MSTT";
            case RE::ENUM_FORM_ID::kFLOR: return "FLOR";
            case RE::ENUM_FORM_ID::kTREE: return "TREE";
            default:
            {
                // Return numeric type if not recognized
                char buf[16];
                snprintf(buf, sizeof(buf), "TYPE_%d", static_cast<int>(formType));
                return std::string(buf);
            }
        }
    }

    std::string ItemOffsetManager::GetItemFormId(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return "00000000";

        auto* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return "00000000";

        char buf[16];
        snprintf(buf, sizeof(buf), "%08X", static_cast<uint32_t>(baseForm->formID));
        return std::string(buf);
    }

    void ItemOffsetManager::GetItemDimensions(RE::TESObjectREFR* refr, float& outLength, float& outWidth, float& outHeight)
    {
        outLength = 0.0f;
        outWidth = 0.0f;
        outHeight = 0.0f;

        if (!refr)
            return;

        // Get the base form's bounding data
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return;

        // Get bounds from TESBoundObject
        const auto& bounds = baseForm->boundData;
        
        // Calculate dimensions from min/max bounds (in game units)
        // Note: These are int16 values representing the bounding box extents
        outLength = static_cast<float>(bounds.boundMax.x - bounds.boundMin.x);
        outWidth = static_cast<float>(bounds.boundMax.y - bounds.boundMin.y);
        outHeight = static_cast<float>(bounds.boundMax.z - bounds.boundMin.z);

        // Also try to get more accurate dimensions from the 3D model's world bound if available
        auto* node = refr->Get3D(false);
        if (node) {
            // Use the radius as an approximation - the actual bounds would need more complex calculation
            float radius = node->worldBound.fRadius;
            if (radius > 0.0f) {
                // If we have a valid world bound, log it for comparison
                spdlog::debug("[ItemOffsets] {} worldBound radius: {:.1f}, BOUND_DATA: ({:.1f}, {:.1f}, {:.1f})",
                              GetItemName(refr), radius, outLength, outWidth, outHeight);
            }
        }
    }

    std::optional<ItemOffset> ItemOffsetManager::GetOffset(RE::TESObjectREFR* refr) const
    {
        if (!refr) return std::nullopt;
        
        // Priority 1: Try form ID lookup first (most reliable - handles mods and localizations)
        std::string formId = GetItemFormId(refr);
        if (!formId.empty() && formId != "00000000") {
            auto formIdIt = _formIdToName.find(formId);
            if (formIdIt != _formIdToName.end()) {
                auto offsetIt = _offsets.find(formIdIt->second);
                if (offsetIt != _offsets.end()) {
                    spdlog::info("[ItemOffsets] Priority 1: FormID match {:08X} -> '{}'", 
                                 refr->formID, formIdIt->second);
                    return offsetIt->second;
                }
            }
        }
        
        // Priority 2: Try exact name match
        return GetOffset(GetItemName(refr));
    }
    
    std::optional<ItemOffset> ItemOffsetManager::GetOffset(const std::string& itemName) const
    {
        // Exact match only - no fuzzy matching
        // Fuzzy/dimension-based fallbacks are handled by GetOffsetWithFallback()
        auto it = _offsets.find(itemName);
        if (it != _offsets.end())
        {
            return it->second;
        }
        
        return std::nullopt;
    }

    void ItemOffsetManager::SaveOffset(RE::TESObjectREFR* refr, const ItemOffset& offset)
    {
        std::string itemName = GetItemName(refr);
        
        // Create a copy of offset to add form ID if not already set
        ItemOffset offsetWithId = offset;
        if (offsetWithId.formId.empty()) {
            offsetWithId.formId = GetItemFormId(refr);
        }
        
        SaveOffset(itemName, offsetWithId);
        
        // Update form ID index
        if (!offsetWithId.formId.empty() && offsetWithId.formId != "00000000") {
            _formIdToName[offsetWithId.formId] = itemName;
        }
    }

    void ItemOffsetManager::SaveOffset(const std::string& itemName, const ItemOffset& offset)
    {
        if (itemName.empty())
            return;

        _offsets[itemName] = offset;
        
        // Update form ID index
        if (!offset.formId.empty() && offset.formId != "00000000") {
            _formIdToName[offset.formId] = itemName;
        }
        
        SaveOffsetToJsonFile(itemName, offset);
    }
    
    void ItemOffsetManager::SaveOffset(RE::TESObjectREFR* refr, const ItemOffset& offset, bool isLeft)
    {
        std::string itemName = GetItemName(refr);
        
        // Create a copy of offset to add form ID if not already set
        ItemOffset offsetWithId = offset;
        if (offsetWithId.formId.empty()) {
            offsetWithId.formId = GetItemFormId(refr);
        }
        
        SaveOffset(itemName, offsetWithId, isLeft);
        
        // Update form ID index (without suffix for lookup)
        if (!offsetWithId.formId.empty() && offsetWithId.formId != "00000000") {
            _formIdToName[offsetWithId.formId] = itemName;
        }
    }
    
    void ItemOffsetManager::SaveOffset(const std::string& itemName, const ItemOffset& offset, bool isLeft)
    {
        if (itemName.empty())
            return;
        
        // Create a copy to set variant flags
        ItemOffset offsetWithFlags = offset;
        offsetWithFlags.isLeftHanded = isLeft;
        
        // Detect power armor status
        bool inPowerArmor = Utils::IsPlayerInPowerArmor();
        offsetWithFlags.isPowerArmor = inPowerArmor;
        
        // Build filename with suffixes: ItemName_L or ItemName_R, plus _PA if in power armor
        std::string handSuffix = isLeft ? "_L" : "_R";
        std::string paSuffix = inPowerArmor ? "_PA" : "";
        std::string suffixedName = itemName + handSuffix + paSuffix;
        
        _offsets[suffixedName] = offsetWithFlags;
        
        // Update form ID index (use base name, not suffixed)
        if (!offset.formId.empty() && offset.formId != "00000000") {
            _formIdToName[offset.formId] = itemName;
        }
        
        SaveOffsetToJsonFile(suffixedName, offsetWithFlags);
        
        spdlog::info("[ItemOffsets] Saved {} hand{} offset for '{}'", 
                     isLeft ? "LEFT" : "RIGHT", 
                     inPowerArmor ? " (Power Armor)" : "",
                     itemName);
    }
    
    std::optional<ItemOffset> ItemOffsetManager::GetOffset(RE::TESObjectREFR* refr, bool isLeft) const
    {
        std::string itemName = GetItemName(refr);
        return GetOffset(itemName, isLeft);
    }
    
    std::optional<ItemOffset> ItemOffsetManager::GetExactOffset(RE::TESObjectREFR* refr, bool isLeft) const
    {
        if (!refr) return std::nullopt;
        
        std::string itemName = GetItemName(refr);
        std::string handSuffix = isLeft ? "_L" : "_R";
        
        // Priority 1: FormID lookup (most reliable)
        std::string formId = GetItemFormId(refr);
        if (!formId.empty() && formId != "00000000") {
            auto formIdIt = _formIdToName.find(formId);
            if (formIdIt != _formIdToName.end()) {
                std::string baseName = formIdIt->second;
                
                // Try hand-specific variant first
                std::string handName = baseName + handSuffix;
                auto handIt = _offsets.find(handName);
                if (handIt != _offsets.end()) {
                    spdlog::info("[ItemOffsets]  Exact FormID match {} -> '{}' ({} hand)",
                                formId, handName, isLeft ? "LEFT" : "RIGHT");
                    ItemOffset result = handIt->second;
                    result.matchedName = handName;
                    return result;
                }

                // Try generic (no hand suffix)
                auto offsetIt = _offsets.find(baseName);
                if (offsetIt != _offsets.end()) {
                    spdlog::info("[ItemOffsets]  Exact FormID match {} -> '{}'", formId, baseName);
                    ItemOffset result = offsetIt->second;
                    result.matchedName = baseName;
                    return result;
                }
            }
        }

        // Priority 2: Exact name match with hand suffix
        std::string handItemName = itemName + handSuffix;
        auto handIt = _offsets.find(handItemName);
        if (handIt != _offsets.end()) {
            spdlog::info("[ItemOffsets]  Exact name match '{}' ({} hand)", itemName, isLeft ? "LEFT" : "RIGHT");
            ItemOffset result = handIt->second;
            result.matchedName = handItemName;
            return result;
        }

        // Priority 3: Exact name match (generic)
        auto nameIt = _offsets.find(itemName);
        if (nameIt != _offsets.end()) {
            spdlog::info("[ItemOffsets]  Exact name match '{}'", itemName);
            ItemOffset result = nameIt->second;
            result.matchedName = itemName;
            return result;
        }
        
        // No exact match found - do NOT use dimension-based fallback
        spdlog::debug("[ItemOffsets] No exact match for '{}' (FormID: {})", itemName, formId);
        return std::nullopt;
    }
    
    std::optional<ItemOffset> ItemOffsetManager::GetOffset(const std::string& itemName, bool isLeft) const
    {
        // First try hand-specific offset
        std::string handSuffix = isLeft ? "_L" : "_R";
        std::string handItemName = itemName + handSuffix;
        
        auto it = _offsets.find(handItemName);
        if (it != _offsets.end())
        {
            spdlog::debug("[ItemOffsets] Found {} hand offset for '{}'", isLeft ? "LEFT" : "RIGHT", itemName);
            return it->second;
        }
        
        // Fall back to generic offset (no hand suffix)
        // This maintains backward compatibility with existing offsets
        return GetOffset(itemName);
    }

    bool ItemOffsetManager::HasOffset(const std::string& itemName) const
    {
        return _offsets.find(itemName) != _offsets.end();
    }
    
    std::optional<ItemOffset> ItemOffsetManager::FindSimilarOffset(float length, float width, float height) const
    {
        // Don't try to match if we have invalid dimensions
        if (length <= 0.0f || width <= 0.0f || height <= 0.0f) {
            return std::nullopt;
        }

        // Sort dimensions to get a normalized aspect ratio (largest:middle:smallest)
        float dims[3] = {length, width, height};
        std::sort(dims, dims + 3, std::greater<float>());  // Descending order
        float targetLargest = dims[0];
        float targetMiddle = dims[1];
        float targetSmallest = dims[2];

        // Calculate normalized ratios (largest = 1.0, others relative to it)
        // This captures the "shape" of the object regardless of absolute size
        float targetRatio1 = (targetLargest > 0.01f) ? targetMiddle / targetLargest : 1.0f;
        float targetRatio2 = (targetLargest > 0.01f) ? targetSmallest / targetLargest : 1.0f;

        struct MatchCandidate {
            std::string name;
            const ItemOffset* offset;
            float score;  // Lower is better
        };
        
        MatchCandidate bestMatch{"", nullptr, 999999.0f};

        for (const auto& [itemName, offset] : _offsets) {
            // Skip items without valid dimensions
            if (offset.length <= 0.0f || offset.width <= 0.0f || offset.height <= 0.0f) {
                continue;
            }

            // Sort this item's dimensions to get normalized ratios
            float itemDims[3] = {offset.length, offset.width, offset.height};
            std::sort(itemDims, itemDims + 3, std::greater<float>());
            float itemLargest = itemDims[0];
            float itemMiddle = itemDims[1];
            float itemSmallest = itemDims[2];

            // Calculate normalized ratios for this item
            float itemRatio1 = (itemLargest > 0.01f) ? itemMiddle / itemLargest : 1.0f;
            float itemRatio2 = (itemLargest > 0.01f) ? itemSmallest / itemLargest : 1.0f;

            // Calculate ratio difference (how similar the shape is)
            // Both ratios range from 0 to 1, so difference ranges from 0 to 2 total
            float ratioDiff = std::abs(targetRatio1 - itemRatio1) + std::abs(targetRatio2 - itemRatio2);

            // Calculate size difference (how similar the scale is)
            // Express as ratio of larger to smaller, so 1.0 = same size, 2.0 = one is 2x larger
            float sizeRatio = (targetLargest > itemLargest) 
                             ? targetLargest / itemLargest 
                             : itemLargest / targetLargest;

            // Combined score: prioritize shape match, then size match
            // ratioDiff: 0 = perfect shape match, 2 = worst possible
            // sizeRatio: 1 = same size, higher = more different
            float score = ratioDiff * 10.0f +        // Shape similarity (most important)
                          (sizeRatio - 1.0f) * 1.0f; // Size similarity (less important)

            if (score < bestMatch.score) {
                bestMatch = {itemName, &offset, score};
            }
        }

        // Only reject if we have no offsets at all, or score is extremely bad
        // maxAspectRatioDiff is used as a threshold for "good enough" match
        if (bestMatch.offset == nullptr) {
            spdlog::debug("[ItemOffsets] No offsets loaded, cannot find similar item");
            return std::nullopt;
        }

        // Log the match quality
        if (bestMatch.score < 1.0f) {
            spdlog::info("[ItemOffsets] EXCELLENT match: '{}' (score: {:.3f}) for dims ({:.1f}, {:.1f}, {:.1f})",
                         bestMatch.name, bestMatch.score, length, width, height);
        } else if (bestMatch.score < 3.0f) {
            spdlog::info("[ItemOffsets] GOOD match: '{}' (score: {:.3f}) for dims ({:.1f}, {:.1f}, {:.1f})",
                         bestMatch.name, bestMatch.score, length, width, height);
        } else {
            spdlog::info("[ItemOffsets] APPROXIMATE match: '{}' (score: {:.3f}) for dims ({:.1f}, {:.1f}, {:.1f})",
                         bestMatch.name, bestMatch.score, length, width, height);
        }
        
        // Log rotation from matched item
        spdlog::info("[ItemOffsets] Matched rotation row0=({:.3f},{:.3f},{:.3f}), pos=({:.2f},{:.2f},{:.2f})",
                     bestMatch.offset->rotation.entry[0][0], bestMatch.offset->rotation.entry[0][1], bestMatch.offset->rotation.entry[0][2],
                     bestMatch.offset->position.x, bestMatch.offset->position.y, bestMatch.offset->position.z);

        // Use the matched offset directly - DON'T scale position!
        // Same aspect ratio = same grip style = same offset
        // Only finger distance/curl needs to adjust for size
        ItemOffset result = *bestMatch.offset;
        
        // Calculate size ratio for finger distance only
        float refDims[3] = {result.length, result.width, result.height};
        std::sort(refDims, refDims + 3, std::greater<float>());
        float refLargest = refDims[0];
        
        if (refLargest > 0.01f) {
            float sizeRatio = targetLargest / refLargest;
            
            // Only adjust finger distance for size - position stays the same
            // Bigger objects need fingers to spread wider
            result.fingerDistance *= sizeRatio;
            
            spdlog::info("[ItemOffsets] Using offset from '{}' (sizeRatio={:.2f}x) - position unchanged, fingerDistance scaled",
                         bestMatch.name, sizeRatio);
        }
        
        // Store the actual dimensions of the target item
        result.length = length;
        result.width = width;
        result.height = height;

        return result;
    }

    std::optional<ItemOffset> ItemOffsetManager::GetOffsetWithFallback(RE::TESObjectREFR* refr) const
    {
        if (!refr) {
            return std::nullopt;
        }

        // =====================================================================
        // PRIORITY MATCHING (from copilot-instructions.md):
        // 1. FormId Exact        - Direct FormId lookup
        // 2. Name Exact          - Exact name match
        // 3. Exact Dims + Same Cat + Same Type = 1.0
        // 4. Exact Dims + Same Cat = 1.0
        // 5. Exact Dims + Same Type = 1.0
        // 6. Exact Dims = 1.0
        // 7. Partial Name Match = 0.85
        // 8-13. Similar Dims variations = 0.55 to 0.95
        // 14-15. Same Ratio Only = 0.20 to 0.45
        // No match (score < 0.5) = Snap to hand
        // =====================================================================

        // Get item name and base form ID for logging
        std::string itemName = GetItemName(refr);
        std::string baseFormId = GetItemFormId(const_cast<RE::TESObjectREFR*>(refr));
        
        spdlog::info("[ItemOffsets] === OFFSET SELECTION for '{}' (FormID: {:08X}, BaseFormID: {}) ===", 
                    itemName, refr->formID, baseFormId);
        
        // Priority 1: FormId Exact - most reliable (handles mods and localizations)
        if (!baseFormId.empty() && baseFormId != "00000000") {
            auto formIdIt = _formIdToName.find(baseFormId);
            if (formIdIt != _formIdToName.end()) {
                auto offsetIt = _offsets.find(formIdIt->second);
                if (offsetIt != _offsets.end()) {
                    spdlog::info("[ItemOffsets]  Priority 1: FORMID match {} -> '{}'", 
                                 baseFormId, formIdIt->second);
                    spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                                offsetIt->second.position.x, offsetIt->second.position.y, 
                                offsetIt->second.position.z, offsetIt->second.fingerDistance);
                    ItemOffset result = offsetIt->second;
                    result.matchQuality = OffsetMatchQuality::Exact;
                    result.matchedName = formIdIt->second;  // The name associated with this FormId
                    return result;
                }
            }
        }
        
        // Priority 2: Name Exact
        
        auto exactMatch = GetOffset(itemName);
        if (exactMatch.has_value()) {
            spdlog::info("[ItemOffsets]  Priority 2: EXACT NAME match for '{}'", itemName);
            spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                        exactMatch->position.x, exactMatch->position.y, exactMatch->position.z,
                        exactMatch->fingerDistance);
            ItemOffset result = exactMatch.value();
            result.matchQuality = OffsetMatchQuality::Exact;
            result.matchedName = itemName;  // Exact match uses item name directly
            return result;
        }
        spdlog::debug("[ItemOffsets] × No exact name match for '{}'", itemName);

        // Get dimensions and metadata for fuzzy matching
        float L = 0, W = 0, H = 0;
        GetItemDimensions(const_cast<RE::TESObjectREFR*>(refr), L, W, H);
        std::string category = ExtractCategoryPrefix(itemName);
        std::string itemType = "";  // Could get from form type if needed
        
        // Get form type string for logging
        std::string formTypeStr = "UNKNOWN";
        if (auto* baseForm = refr->GetObjectReference()) {
            auto formType = baseForm->GetFormType();
            switch (formType) {
                case RE::ENUM_FORM_ID::kWEAP: formTypeStr = "WEAP"; break;
                case RE::ENUM_FORM_ID::kARMO: formTypeStr = "ARMO"; break;
                case RE::ENUM_FORM_ID::kMISC: formTypeStr = "MISC"; break;
                case RE::ENUM_FORM_ID::kALCH: formTypeStr = "ALCH"; break;
                case RE::ENUM_FORM_ID::kAMMO: formTypeStr = "AMMO"; break;
                case RE::ENUM_FORM_ID::kBOOK: formTypeStr = "BOOK"; break;
                case RE::ENUM_FORM_ID::kINGR: formTypeStr = "INGR"; break;
                case RE::ENUM_FORM_ID::kKEYM: formTypeStr = "KEYM"; break;
                case RE::ENUM_FORM_ID::kNOTE: formTypeStr = "NOTE"; break;
                default: formTypeStr = std::to_string(static_cast<int>(formType)); break;
            }
            itemType = formTypeStr;
        }
        
        spdlog::info("[ItemOffsets] Item dims: L={:.2f} W={:.2f} H={:.2f}, category='{}', type='{}'",
                    L, W, H, category.empty() ? "(none)" : category, formTypeStr);
        spdlog::info("[ItemOffsets] Searching {} registered offsets for best match...", _offsets.size());
        
        // =====================================================================
        // ARMOR-SPECIFIC MATCHING: Find closest armor by XZ dimensions
        // Armor pieces are typically flat, so XZ (length/width) matter most
        // =====================================================================
        bool isArmor = (formTypeStr == "ARMO");
        
        if (isArmor && L > 0 && W > 0) {
            const ItemOffset* bestArmorMatch = nullptr;
            std::string bestArmorName;
            float bestArmorScore = 999999.0f;  // Lower is better (distance)
            
            for (const auto& [candName, candOffset] : _offsets) {
                // Only match against other armor offsets
                if (candOffset.itemType != "ARMO")
                    continue;
                
                if (candOffset.length <= 0 || candOffset.width <= 0)
                    continue;
                
                // Calculate XZ distance (ignoring height since armor is flat)
                float dL = std::abs(candOffset.length - L);
                float dW = std::abs(candOffset.width - W);
                float xzDistance = std::sqrt(dL * dL + dW * dW);
                
                if (xzDistance < bestArmorScore) {
                    bestArmorScore = xzDistance;
                    bestArmorMatch = &candOffset;
                    bestArmorName = candName;
                }
            }
            
            // Use armor match if found (any distance, since we just want closest)
            if (bestArmorMatch) {
                spdlog::info("[ItemOffsets]  ARMOR XZ-match for '{}' -> '{}'", itemName, bestArmorName);
                spdlog::info("[ItemOffsets]   -> XZ distance: {:.2f}, matched dims: L={:.2f} W={:.2f}",
                            bestArmorScore, bestArmorMatch->length, bestArmorMatch->width);
                spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                            bestArmorMatch->position.x, bestArmorMatch->position.y, bestArmorMatch->position.z,
                            bestArmorMatch->fingerDistance);
                ItemOffset result = *bestArmorMatch;
                result.length = L;
                result.width = W;
                result.height = H;
                // Armor XZ match is dimension-based if exact, fuzzy otherwise
                result.matchQuality = (bestArmorScore < 0.1f) ? OffsetMatchQuality::Dimensions : OffsetMatchQuality::Fuzzy;
                result.matchedName = bestArmorName;
                return result;
            }
            
            spdlog::info("[ItemOffsets] × No armor offsets in database, falling through to general matching");
        }
        
        // If dimensions are invalid, snap to hand
        if (L <= 0 || W <= 0) {
            spdlog::info("[ItemOffsets] ⚠ Invalid dimensions (L={:.2f}, W={:.2f}), using snap-to-hand", L, W);
            ItemOffset snapOffset = CalculateOffsetFromDimensions(L, W, H);
            spdlog::info("[ItemOffsets]   -> Calculated snap offset: pos=({:.2f}, {:.2f}, {:.2f})",
                        snapOffset.position.x, snapOffset.position.y, snapOffset.position.z);
            return snapOffset;
        }

        const ItemOffset* bestMatch = nullptr;
        std::string bestMatchName;
        float bestScore = 0.0f;
        
        // Track top candidates for logging
        struct MatchCandidate {
            std::string name;
            float score;
            float dL, dW, dH;
        };
        std::vector<MatchCandidate> topCandidates;

        // Priority 3-6: Exact Dims (with category/type as tiebreakers, but all score 1.0)
        for (const auto& [candName, candOffset] : _offsets)
        {
            if (candOffset.length <= 0 || candOffset.width <= 0)
                continue;
                
            float dL = std::abs(candOffset.length - L);
            float dW = std::abs(candOffset.width - W);
            float dH = std::abs(candOffset.height - H);
            
            // Truly exact dimensions (NO tolerance)
            if (dL == 0.0f && dW == 0.0f && dH == 0.0f)
            {
                std::string candCategory = ExtractCategoryPrefix(candName);
                bool sameCat = !category.empty() && category == candCategory;
                bool sameType = !itemType.empty() && !candOffset.itemType.empty() && itemType == candOffset.itemType;
                
                // Use category/type as priority ordering (all still score 1.0)
                float priorityScore = 1.0f;
                if (sameCat && sameType)
                    priorityScore = 1.03f;  // Priority 3
                else if (sameCat)
                    priorityScore = 1.02f;  // Priority 4
                else if (sameType)
                    priorityScore = 1.01f;  // Priority 5
                // else Priority 6 = 1.0f
                
                if (priorityScore > bestScore)
                {
                    bestScore = priorityScore;
                    bestMatch = &candOffset;
                    bestMatchName = candName;
                }
            }
        }
        
        if (bestMatch)
        {
            int priority = (bestScore >= 1.03f) ? 3 : (bestScore >= 1.02f) ? 4 : (bestScore >= 1.01f) ? 5 : 6;
            spdlog::info("[ItemOffsets]  Priority {}: EXACT DIMS match for '{}' -> '{}'", 
                         priority, itemName, bestMatchName);
            spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                        bestMatch->position.x, bestMatch->position.y, bestMatch->position.z,
                        bestMatch->fingerDistance);
            ItemOffset result = *bestMatch;
            result.matchQuality = OffsetMatchQuality::Dimensions;
            result.matchedName = bestMatchName;
            return result;
        }
        
        spdlog::debug("[ItemOffsets] × No exact dimension match found");

        // Priority 7: Partial Name Match (score = 0.85)
        float bestPartialNameScore = 0.0f;
        std::string bestPartialName;
        for (const auto& [candName, candOffset] : _offsets)
        {
            float nameScore = GetNameMatchScore(itemName, candName);
            if (nameScore >= 0.70f)  // Threshold for partial match
            {
                if (nameScore > bestPartialNameScore) {
                    bestPartialNameScore = nameScore;
                    bestPartialName = candName;
                }
                float score = 0.85f;  // Fixed score for partial name match
                if (score > bestScore)
                {
                    bestScore = score;
                    bestMatch = &candOffset;
                    bestMatchName = candName;
                }
            }
        }
        
        if (bestMatch && bestScore >= 0.5f)
        {
            spdlog::info("[ItemOffsets]  Priority 7: PARTIAL NAME match for '{}' -> '{}'", 
                         itemName, bestMatchName);
            spdlog::info("[ItemOffsets]   -> Name similarity: {:.0f}%, score: {:.2f}", 
                        bestPartialNameScore * 100, bestScore);
            spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                        bestMatch->position.x, bestMatch->position.y, bestMatch->position.z,
                        bestMatch->fingerDistance);
            ItemOffset result = *bestMatch;
            result.matchQuality = OffsetMatchQuality::Partial;
            result.matchedName = bestMatchName;  // Track what was matched for left-hand lookup
            return result;
        }
        
        spdlog::debug("[ItemOffsets] × No partial name match found");

        // Priority 8-15: Similar Dims and Ratio-only matches
        spdlog::debug("[ItemOffsets] Checking fuzzy dimension matches...");
        for (const auto& [candName, candOffset] : _offsets)
        {
            if (candOffset.length <= 0 || candOffset.width <= 0)
                continue;
            
            float score = CalculateMatchScore(candOffset, candName, L, W, H, itemName, category, itemType);
            
            // Track top candidates for logging
            if (score >= 0.3f) {
                topCandidates.push_back({candName, score, 
                    std::abs(candOffset.length - L), 
                    std::abs(candOffset.width - W), 
                    std::abs(candOffset.height - H)});
            }
            
            if (score > bestScore)
            {
                bestScore = score;
                bestMatch = &candOffset;
                bestMatchName = candName;
            }
        }
        
        // Sort candidates by score for logging
        std::sort(topCandidates.begin(), topCandidates.end(), 
                  [](const MatchCandidate& a, const MatchCandidate& b) { return a.score > b.score; });
        
        // Log top 3 candidates
        if (!topCandidates.empty()) {
            spdlog::debug("[ItemOffsets] Top fuzzy candidates:");
            size_t numToLog = topCandidates.size() < 3 ? topCandidates.size() : 3;
            for (size_t i = 0; i < numToLog; i++) {
                const auto& c = topCandidates[i];
                spdlog::debug("[ItemOffsets]   #{}: '{}' score={:.2f} (dL={:.1f}, dW={:.1f}, dH={:.1f})",
                             static_cast<int>(i+1), c.name, c.score, c.dL, c.dW, c.dH);
            }
        }
        
        // Accept if score >= 0.5
        if (bestMatch && bestScore >= 0.5f)
        {
            spdlog::info("[ItemOffsets]  Priority 8-13: FUZZY DIMS match for '{}' -> '{}' (score: {:.2f})", 
                         itemName, bestMatchName, bestScore);
            spdlog::info("[ItemOffsets]   -> matched dims: L={:.2f} W={:.2f} H={:.2f}",
                        bestMatch->length, bestMatch->width, bestMatch->height);
            spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                        bestMatch->position.x, bestMatch->position.y, bestMatch->position.z,
                        bestMatch->fingerDistance);
            ItemOffset result = *bestMatch;
            result.matchQuality = OffsetMatchQuality::Fuzzy;
            result.matchedName = bestMatchName;
            return result;
        }
        
        // Score < 0.5 means ratio-only match (0.20-0.45) - use it but scale finger distance
        if (bestMatch && bestScore >= 0.2f)
        {
            spdlog::info("[ItemOffsets]  Priority 14-15: RATIO-ONLY match for '{}' -> '{}' (score: {:.2f})", 
                         itemName, bestMatchName, bestScore);
            
            // Scale finger distance for size difference
            ItemOffset result = *bestMatch;
            float refDims[3] = {result.length, result.width, result.height};
            std::sort(refDims, refDims + 3, std::greater<float>());
            float refLargest = refDims[0];
            
            float targetDims[3] = {L, W, H};
            std::sort(targetDims, targetDims + 3, std::greater<float>());
            float targetLargest = targetDims[0];
            
            float sizeRatio = 1.0f;
            if (refLargest > 0.01f) {
                sizeRatio = targetLargest / refLargest;
                result.fingerDistance *= sizeRatio;
            }
            
            spdlog::info("[ItemOffsets]   -> Size ratio: {:.2f}, scaled fingerDist: {:.2f}",
                        sizeRatio, result.fingerDistance);
            spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f})",
                        result.position.x, result.position.y, result.position.z);
            
            result.length = L;
            result.width = W;
            result.height = H;
            result.matchQuality = OffsetMatchQuality::Fuzzy;
            result.matchedName = bestMatchName;
            return result;
        }
        
        // No match - snap to hand (use archetype formula)
        spdlog::info("[ItemOffsets] × NO MATCH for '{}' (best score: {:.2f} < 0.2)", itemName, bestScore);
        spdlog::info("[ItemOffsets]   -> Using SNAP-TO-HAND formula for dims: {:.1f}x{:.1f}x{:.1f}", L, W, H);
        ItemOffset snapOffset = CalculateOffsetFromDimensions(L, W, H);
        snapOffset.matchQuality = OffsetMatchQuality::None;
        spdlog::info("[ItemOffsets]   -> Calculated: pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                    snapOffset.position.x, snapOffset.position.y, snapOffset.position.z,
                    snapOffset.fingerDistance);
        return snapOffset;
    }
    
    std::optional<ItemOffset> ItemOffsetManager::GetOffsetWithFallback(RE::TESObjectREFR* refr, bool isLeft) const
    {
        if (!refr) {
            return std::nullopt;
        }
        
        std::string itemName = GetItemName(refr);
        bool inPowerArmor = Utils::IsPlayerInPowerArmor();
        
        // =====================================================================
        // OFFSET PRIORITY ORDER (key format: BaseName[_L|_R][_PA]):
        // 1. Power Armor + hand-specific (e.g. "Laser Rifle_L_PA" / "_R_PA")
        // 2. Power Armor variant (e.g. "Laser Rifle_PA") - if in PA
        // 3. Hand-specific (e.g. "Laser Rifle_L" / "Laser Rifle_R")
        // 4. Generic (e.g. "Laser Rifle") - fallback to regular GetOffsetWithFallback
        // =====================================================================

        std::string handSuffix = isLeft ? "_L" : "_R";

        // Priority 0: For kNOTE items (holotapes), always use the shared NOTE_DEFAULT offset.
        // Holotapes are physically identical — per-item overrides are unwanted.
        {
            auto* baseObj = refr->GetObjectReference();
            auto formType = baseObj ? baseObj->GetFormType() : RE::ENUM_FORM_ID::kNONE;
            spdlog::debug("[ItemOffsets] Priority0 '{}': baseObj={} formType={:#x} isNOTE={}",
                         itemName, (void*)baseObj, (uint32_t)formType,
                         formType == RE::ENUM_FORM_ID::kNOTE);
            if (baseObj && formType == RE::ENUM_FORM_ID::kNOTE) {
                std::string noteDefault = std::string("__NOTE_DEFAULT") + handSuffix;
                auto it = _offsets.find(noteDefault);
                if (it == _offsets.end() && !isLeft) {
                    // No _R variant — use _L and let the mirror system handle right hand
                    noteDefault = "__NOTE_DEFAULT_L";
                    it = _offsets.find(noteDefault);
                }
                spdlog::debug("[ItemOffsets] Priority0 NOTE key='{}' found={}", noteDefault, it != _offsets.end());
                if (it != _offsets.end()) {
                    spdlog::info("[ItemOffsets]  NOTE default for '{}' ({})", itemName, noteDefault);
                    ItemOffset result = it->second;
                    result.matchedName = noteDefault;
                    result.matchQuality = OffsetMatchQuality::Exact;
                    return result;
                }
            }
        }

        // Priority 1: Power Armor + hand-specific (if in PA)
        if (inPowerArmor) {
            std::string paHandName = itemName + handSuffix + "_PA";
            auto it = _offsets.find(paHandName);
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found PA+{} hand offset for '{}'", isLeft ? "LEFT" : "RIGHT", itemName);
                spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                            it->second.position.x, it->second.position.y,
                            it->second.position.z, it->second.fingerDistance);
                ItemOffset result = it->second;
                result.matchedName = paHandName;
                return result;
            }
        }

        // Priority 2: Power Armor variant (if in PA)
        if (inPowerArmor) {
            std::string paName = itemName + "_PA";
            auto it = _offsets.find(paName);
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found POWER ARMOR offset for '{}'", itemName);
                spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                            it->second.position.x, it->second.position.y,
                            it->second.position.z, it->second.fingerDistance);
                ItemOffset result = it->second;
                result.matchedName = paName;
                return result;
            }
        }

        // Priority 3: Hand-specific offset (_L or _R)
        {
            std::string handName = itemName + handSuffix;
            auto it = _offsets.find(handName);
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found {} hand offset for '{}'", isLeft ? "LEFT" : "RIGHT", itemName);
                spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                            it->second.position.x, it->second.position.y,
                            it->second.position.z, it->second.fingerDistance);
                ItemOffset result = it->second;
                result.matchedName = handName;
                return result;
            }
        }
        
        // Priority 4: Generic offset - fall back to generic GetOffsetWithFallback
        spdlog::debug("[ItemOffsets] No {} hand{} offset for '{}', trying generic", 
                     isLeft ? "LEFT" : "RIGHT", inPowerArmor ? "/PA" : "", itemName);
        
        auto genericResult = GetOffsetWithFallback(refr);
        
        // If we got a partial/fuzzy match, check if a hand-specific variant of the matched name exists
        if (genericResult.has_value() && !genericResult->matchedName.empty()) {
            std::string handVariantName = genericResult->matchedName + handSuffix;
            auto handIt = _offsets.find(handVariantName);
            if (handIt != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found {} variant '{}' for matched '{}'",
                            isLeft ? "LEFT" : "RIGHT", handVariantName, genericResult->matchedName);
                spdlog::info("[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                            handIt->second.position.x, handIt->second.position.y,
                            handIt->second.position.z, handIt->second.fingerDistance);
                ItemOffset handResult = handIt->second;
                handResult.matchedName = handVariantName;
                return handResult;
            }
        }
        
        // Priority 5: Form-type default (e.g., all holotapes use a shared offset)
        if (!genericResult.has_value()) {
            auto* baseObj = refr->GetObjectReference();
            if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                std::string noteDefault = std::string("__NOTE_DEFAULT") + handSuffix;
                auto it = _offsets.find(noteDefault);
                if (it != _offsets.end()) {
                    spdlog::info("[ItemOffsets]  Priority 5: NOTE form-type default for '{}' ({})", itemName, noteDefault);
                    ItemOffset result = it->second;
                    result.matchedName = noteDefault;
                    result.matchQuality = OffsetMatchQuality::Fuzzy;
                    return result;
                }
            }
        }

        return genericResult;
    }

    std::optional<ItemOffset> ItemOffsetManager::GetThrowableOffset(const std::string& itemName, bool isLeft) const
    {
        bool inPowerArmor = Utils::IsPlayerInPowerArmor();
        
        // =====================================================================
        // THROWABLE OFFSET PRIORITY (key format: BaseName[_L][_PA][_T]):
        // 1. PA + Left + Throwable (e.g. "GrenadeFrag_L_PA_T") - if in PA and left hand
        // 2. PA + Throwable (e.g. "GrenadeFrag_PA_T") - if in PA
        // 3. Left + Throwable (e.g. "GrenadeFrag_L_T") - if left hand
        // 4. Throwable (e.g. "GrenadeFrag_T")
        // 5. nullopt - no throwable offset found
        // =====================================================================
        
        // Priority 1: PA + Left + Throwable
        if (inPowerArmor && isLeft) {
            std::string key = itemName + "_L_PA_T";
            auto it = _offsets.find(key);
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found PA+LEFT throwable offset for '{}'", itemName);
                return it->second;
            }
        }
        
        // Priority 2: PA + Throwable
        if (inPowerArmor) {
            std::string key = itemName + "_PA_T";
            auto it = _offsets.find(key);
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found PA throwable offset for '{}'", itemName);
                return it->second;
            }
        }
        
        // Priority 3: Left + Throwable
        if (isLeft) {
            std::string key = itemName + "_L_T";
            auto it = _offsets.find(key);
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found LEFT throwable offset for '{}'", itemName);
                return it->second;
            }
        }
        
        // Priority 4: Throwable only
        {
            std::string key = itemName + "_T";
            auto it = _offsets.find(key);
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  Found throwable offset for '{}'", itemName);
                return it->second;
            }
        }
        
        spdlog::debug("[ItemOffsets] No throwable offset for '{}'", itemName);
        return std::nullopt;
    }

    bool ItemOffsetManager::HasExactDimensionsMatch(RE::TESObjectREFR* refr) const
    {
        if (!refr) {
            return false;
        }

        // Get item's dimensions
        float L = 0, W = 0, H = 0;
        GetItemDimensions(const_cast<RE::TESObjectREFR*>(refr), L, W, H);
        
        if (L <= 0 || W <= 0 || H <= 0) {
            return false;  // Invalid dimensions
        }

        // Check for FormID exact match first (most reliable)
        std::string baseFormId = GetItemFormId(const_cast<RE::TESObjectREFR*>(refr));
        if (!baseFormId.empty() && baseFormId != "00000000") {
            auto formIdIt = _formIdToName.find(baseFormId);
            if (formIdIt != _formIdToName.end()) {
                auto offsetIt = _offsets.find(formIdIt->second);
                if (offsetIt != _offsets.end()) {
                    // FormID match = always exact
                    spdlog::debug("[ItemOffsets] HasExactDimensionsMatch: FormID match for {:08X}", refr->formID);
                    return true;
                }
            }
        }

        // Check for exact name match
        std::string itemName = GetItemName(refr);
        auto exactMatch = _offsets.find(itemName);
        if (exactMatch != _offsets.end()) {
            spdlog::debug("[ItemOffsets] HasExactDimensionsMatch: Exact name match for '{}'", itemName);
            return true;
        }

        // Check all offsets for EXACT dimensions match (no tolerance at all)
        for (const auto& [candName, candOffset] : _offsets) {
            if (candOffset.length <= 0 || candOffset.width <= 0 || candOffset.height <= 0) {
                continue;
            }
            
            // EXACT match - no tolerance whatsoever
            if (candOffset.length == L && candOffset.width == W && candOffset.height == H) {
                spdlog::debug("[ItemOffsets] HasExactDimensionsMatch: EXACT dims match for '{}' -> '{}' (L={:.1f} W={:.1f} H={:.1f})",
                            itemName, candName, L, W, H);
                return true;
            }
        }

        spdlog::debug("[ItemOffsets] HasExactDimensionsMatch: NO exact match for '{}' (L={:.1f} W={:.1f} H={:.1f})",
                    itemName, L, W, H);
        return false;
    }

    bool ItemOffsetManager::HasExactMatch(RE::TESObjectREFR* refr) const
    {
        if (!refr) {
            return false;
        }

        // Check for FormID exact match first (most reliable) - Priority 1
        std::string baseFormId = GetItemFormId(const_cast<RE::TESObjectREFR*>(refr));
        if (!baseFormId.empty() && baseFormId != "00000000") {
            auto formIdIt = _formIdToName.find(baseFormId);
            if (formIdIt != _formIdToName.end()) {
                auto offsetIt = _offsets.find(formIdIt->second);
                if (offsetIt != _offsets.end()) {
                    return true;  // FormID match = exact match
                }
            }
        }

        // Check for exact name match - Priority 2
        std::string itemName = GetItemName(refr);
        auto exactMatch = _offsets.find(itemName);
        if (exactMatch != _offsets.end()) {
            return true;  // Exact name match
        }

        // Check for hand-specific variants (_L or _R) - Priority 3
        if (_offsets.find(itemName + "_L") != _offsets.end() ||
            _offsets.find(itemName + "_R") != _offsets.end()) {
            return true;  // Hand-specific variant exists
        }

        // No exact match (FormID or name) - this item would get fuzzy/partial matching
        return false;
    }

}
