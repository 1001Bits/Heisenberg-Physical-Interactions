#include "ItemOffsets.h"
#include "ShapeReferences.h"
#include "EmbeddedOffsets.h"
#include "F4VROffsets.h"
#include "SharedUtils.h"  // ContainsCI — mirrored-armour-piece guard in GetArmorDimensionalDonorOffset
#include "Utils.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
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

    static std::string NormalizeFormIdIdentity(std::string formId)
    {
        if (formId.starts_with("0x") || formId.starts_with("0X")) {
            formId.erase(0, 2);
        }
        formId.erase(
            std::remove_if(
                formId.begin(),
                formId.end(),
                [](unsigned char ch) { return std::isspace(ch) != 0; }),
            formId.end());
        std::transform(
            formId.begin(),
            formId.end(),
            formId.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
        if (!formId.empty() && formId.size() < 8) {
            formId.insert(formId.begin(), 8 - formId.size(), '0');
        }
        return formId;
    }

    static std::string NormalizeEditorIdentity(const std::string& editorId)
    {
        return ToLower(editorId);
    }

    static std::string NormalizeProfileKeyIdentity(
        std::string_view value)
    {
        std::string normalized;
        normalized.reserve(value.size());
        for (const unsigned char ch : value) {
            if (std::isalnum(ch) != 0) {
                normalized.push_back(
                    static_cast<char>(std::tolower(ch)));
            }
        }
        return normalized;
    }

    static void StripTrailingOffsetSuffix(
        std::string& name,
        std::string_view suffix)
    {
        if (name.size() > suffix.size() &&
            name.compare(
                name.size() - suffix.size(),
                suffix.size(),
                suffix) == 0) {
            name.erase(name.size() - suffix.size());
        }
    }

    static std::string BaseOffsetLookupName(
        std::string name,
        bool isLeftHanded,
        bool isPowerArmor,
        bool isThrowable)
    {
        // Strip in reverse construction order. This accepts both generated
        // entries whose name is already suffixed and entries whose variant
        // metadata is stored separately, without ever producing `_L_L`.
        if (isThrowable) {
            StripTrailingOffsetSuffix(name, "_T");
        }
        if (isPowerArmor) {
            StripTrailingOffsetSuffix(name, "_PA");
        }
        StripTrailingOffsetSuffix(
            name,
            isLeftHanded ? "_L" : "_R");
        return name;
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
            std::string baseName = BaseOffsetLookupName(
                cleanName,
                data.isLeftHanded,
                data.isPowerArmor,
                data.isThrowable);
            std::string lookupKey = baseName;
            if (data.isLeftHanded) lookupKey += "_L";
            if (data.isPowerArmor) lookupKey += "_PA";
            if (data.isThrowable) lookupKey += "_T";
            
            _offsets[lookupKey] = offset;
            IndexNormalizedProfileAlias(baseName);
            
            // Update stable identity indexes only for the unsuffixed base
            // entry. Variants are selected after resolving this base key.
            if (!offset.formId.empty() && offset.formId != "00000000" && 
                !data.isLeftHanded && !data.isPowerArmor && !data.isThrowable) {
                IndexStableIdentity(baseName, offset.formId);
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
                if (value.contains("editorId"))
                {
                    offset.editorId = value["editorId"].get<std::string>();
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

                // Update form ID index (formId is already loaded from JSON above).
                // SaveOffset's base-name invariant (see its own "use base name, not
                // suffixed" comment) means the RUNTIME index built during THIS session
                // always maps formId -> the base item name. But the JSON top-level key
                // (itemName here) is the SUFFIXED name (SaveOffsetToJsonFile is always
                // called with the _L/_R/_PA/_T-suffixed variant), so re-deriving the index
                // from disk after a restart mapped formId -> a suffixed name instead
                // (e.g. "Nuka-Cola_R") - GetOffsetWithFallback/GetExactOffset's Priority 1
                // formId lookup then returned that right-hand-space offset as an EXACT
                // match for the LEFT hand too, with none of the hand-variant/mirror
                // resolution a same-session left-hand grab would have gone through. Strip
                // the known suffixes (using the variant flags just loaded above) before
                // indexing, matching SaveOffset's base-name invariant. Safe no-op for
                // entries that never had a given suffix (exact-match compare only).
                std::string formIdIndexName =
                    BaseOffsetLookupName(
                        cleanName,
                        offset.isLeftHanded,
                        offset.isPowerArmor,
                        offset.isThrowable);
                IndexNormalizedProfileAlias(formIdIndexName);

                if ((!offset.formId.empty() &&
                     offset.formId != "00000000") ||
                    !offset.editorId.empty()) {
                    IndexStableIdentity(
                        formIdIndexName,
                        offset.formId,
                        offset.editorId);
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
            if (!offset.editorId.empty()) {
                j[itemName]["editorId"] = offset.editorId;
            }

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

    std::string ItemOffsetManager::GetItemEditorId(RE::TESObjectREFR* refr)
    {
        if (!refr) {
            return {};
        }
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm) {
            return {};
        }
        const char* editorId = baseForm->GetFormEditorID();
        return editorId ? std::string(editorId) : std::string{};
    }

    void ItemOffsetManager::IndexStableIdentity(
        const std::string& lookupName,
        const std::string& formId,
        const std::string& editorId)
    {
        if (lookupName.empty()) {
            return;
        }
        IndexNormalizedProfileAlias(lookupName);

        const std::string normalizedFormId =
            NormalizeFormIdIdentity(formId);
        if (!normalizedFormId.empty() &&
            normalizedFormId != "00000000") {
            _formIdToName[normalizedFormId] = lookupName;
        }

        std::string resolvedEditorId = editorId;
        if (resolvedEditorId.empty() &&
            !normalizedFormId.empty() &&
            normalizedFormId != "00000000") {
            try {
                const auto numericFormId =
                    static_cast<RE::TESFormID>(
                        std::stoul(normalizedFormId, nullptr, 16));
                if (auto* form = RE::TESForm::GetFormByID(numericFormId)) {
                    if (const char* runtimeEditorId =
                            form->GetFormEditorID();
                        runtimeEditorId && runtimeEditorId[0] != '\0') {
                        resolvedEditorId = runtimeEditorId;
                    }
                }
            } catch (const std::exception&) {
                // The FormID index remains useful even when malformed
                // third-party metadata cannot be resolved to a live form.
            }
        }

        if (!resolvedEditorId.empty()) {
            _editorIdToName[
                NormalizeEditorIdentity(resolvedEditorId)] =
                lookupName;
        }
    }

    void ItemOffsetManager::IndexNormalizedProfileAlias(
        const std::string& lookupName)
    {
        const std::string normalized =
            NormalizeProfileKeyIdentity(lookupName);
        if (normalized.empty() ||
            _ambiguousNormalizedProfileKeys.contains(normalized)) {
            return;
        }

        const auto [it, inserted] =
            _normalizedProfileKeyToName.emplace(
                normalized,
                lookupName);
        if (!inserted && it->second != lookupName) {
            spdlog::warn(
                "[ItemOffsets] Normalized profile identity '{}' is "
                "ambiguous between '{}' and '{}'; alias disabled",
                normalized,
                it->second,
                lookupName);
            _normalizedProfileKeyToName.erase(it);
            _ambiguousNormalizedProfileKeys.insert(normalized);
        }
    }

    std::optional<std::string>
    ItemOffsetManager::FindStableLookupName(
        RE::TESObjectREFR* refr) const
    {
        if (!refr) {
            return std::nullopt;
        }

        const std::string formId =
            NormalizeFormIdIdentity(GetItemFormId(refr));
        if (!formId.empty() && formId != "00000000") {
            if (const auto formIt = _formIdToName.find(formId);
                formIt != _formIdToName.end()) {
                return formIt->second;
            }
        }

        const std::string rawEditorId = GetItemEditorId(refr);
        const std::string editorId =
            NormalizeEditorIdentity(rawEditorId);
        if (!editorId.empty()) {
            if (const auto editorIt =
                    _editorIdToName.find(editorId);
                editorIt != _editorIdToName.end()) {
                return editorIt->second;
            }

            const std::string normalizedEditorId =
                NormalizeProfileKeyIdentity(rawEditorId);
            if (!normalizedEditorId.empty() &&
                !_ambiguousNormalizedProfileKeys.contains(
                    normalizedEditorId)) {
                if (const auto aliasIt =
                        _normalizedProfileKeyToName.find(
                            normalizedEditorId);
                    aliasIt !=
                    _normalizedProfileKeyToName.end()) {
                    return aliasIt->second;
                }
            }

            // User-authored databases may use the editor ID itself as their
            // top-level key. Accept that stable key before a translated FULL
            // name without requiring a separate alias record.
            if (_offsets.contains(rawEditorId)) {
                return rawEditorId;
            }
        }
        return std::nullopt;
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

        // Stable base FormID/editor ID wins over the localized display name.
        if (const auto stableName = FindStableLookupName(refr)) {
            if (const auto offsetIt = _offsets.find(*stableName);
                offsetIt != _offsets.end()) {
                spdlog::info(
                    "[ItemOffsets] Stable identity match {:08X} -> '{}'",
                    refr->formID,
                    *stableName);
                return offsetIt->second;
            }
        }

        // Localized display name is the compatibility fallback.
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
        
        // Create a copy of offset to add stable identities if not already set.
        ItemOffset offsetWithId = offset;
        if (offsetWithId.formId.empty()) {
            offsetWithId.formId = GetItemFormId(refr);
        }
        if (offsetWithId.editorId.empty()) {
            offsetWithId.editorId = GetItemEditorId(refr);
        }
        
        SaveOffset(itemName, offsetWithId);
        
        IndexStableIdentity(
            itemName,
            offsetWithId.formId,
            offsetWithId.editorId);
    }

    void ItemOffsetManager::SaveOffset(const std::string& itemName, const ItemOffset& offset)
    {
        if (itemName.empty())
            return;

        _offsets[itemName] = offset;
        
        IndexStableIdentity(itemName, offset.formId, offset.editorId);
        
        SaveOffsetToJsonFile(itemName, offset);
    }
    
    void ItemOffsetManager::SaveOffset(RE::TESObjectREFR* refr, const ItemOffset& offset, bool isLeft)
    {
        std::string itemName = GetItemName(refr);
        
        // Create a copy of offset to add stable identities if not already set.
        ItemOffset offsetWithId = offset;
        if (offsetWithId.formId.empty()) {
            offsetWithId.formId = GetItemFormId(refr);
        }
        if (offsetWithId.editorId.empty()) {
            offsetWithId.editorId = GetItemEditorId(refr);
        }
        
        SaveOffset(itemName, offsetWithId, isLeft);
        
        IndexStableIdentity(
            itemName,
            offsetWithId.formId,
            offsetWithId.editorId);
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
        
        // Stable identity always maps to the unsuffixed base name.
        IndexStableIdentity(itemName, offset.formId, offset.editorId);
        
        SaveOffsetToJsonFile(suffixedName, offsetWithFlags);
        
        spdlog::info("[ItemOffsets] Saved {} hand{} offset for '{}'", 
                     isLeft ? "LEFT" : "RIGHT", 
                     inPowerArmor ? " (Power Armor)" : "",
                     itemName);
    }
    
    std::optional<ItemOffset> ItemOffsetManager::GetOffset(RE::TESObjectREFR* refr, bool isLeft) const
    {
        if (!refr) {
            return std::nullopt;
        }
        if (const auto stableName = FindStableLookupName(refr)) {
            if (auto stableOffset = GetOffset(*stableName, isLeft)) {
                return stableOffset;
            }
        }
        return GetOffset(GetItemName(refr), isLeft);
    }
    
    std::optional<ItemOffset> ItemOffsetManager::GetExactOffset(RE::TESObjectREFR* refr, bool isLeft) const
    {
        if (!refr) return std::nullopt;
        
        std::string handSuffix = isLeft ? "_L" : "_R";

        // Priority 1: stable base FormID/editor ID. Resolve the unsuffixed
        // database identity first, then select a hand variant.
        if (const auto stableName = FindStableLookupName(refr)) {
            const std::string handName = *stableName + handSuffix;
            if (const auto handIt = _offsets.find(handName);
                handIt != _offsets.end()) {
                spdlog::info(
                    "[ItemOffsets] Exact stable identity -> '{}' ({} hand)",
                    handName,
                    isLeft ? "LEFT" : "RIGHT");
                ItemOffset result = handIt->second;
                result.matchedName = handName;
                return result;
            }

            if (const auto offsetIt = _offsets.find(*stableName);
                offsetIt != _offsets.end()) {
                spdlog::info(
                    "[ItemOffsets] Exact stable identity -> '{}'",
                    *stableName);
                ItemOffset result = offsetIt->second;
                result.matchedName = *stableName;
                return result;
            }
        }

        // Compatibility fallback: localized display name.
        const std::string itemName = GetItemName(refr);
        const std::string handItemName = itemName + handSuffix;
        if (const auto handIt = _offsets.find(handItemName);
            handIt != _offsets.end()) {
            spdlog::info(
                "[ItemOffsets] Exact display-name match '{}' ({} hand)",
                itemName,
                isLeft ? "LEFT" : "RIGHT");
            ItemOffset result = handIt->second;
            result.matchedName = handItemName;
            return result;
        }

        if (const auto nameIt = _offsets.find(itemName);
            nameIt != _offsets.end()) {
            spdlog::info(
                "[ItemOffsets] Exact display-name match '{}'",
                itemName);
            ItemOffset result = nameIt->second;
            result.matchedName = itemName;
            return result;
        }

        // Priority 4: Holotape form-type default — every kNOTE item shares
        // the __NOTE_DEFAULT offset since they're physically identical.
        // The embedded entry has isLeftHanded=true so it lands in the map
        // under "__NOTE_DEFAULT_L". Try the hand-specific variant first;
        // for the right hand, fall back to "_L" and let the mirror system
        // flip it (HIGGS-style — the offset works for either hand).
        if (auto* baseObj = refr->GetObjectReference();
            baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
            std::string noteDefault = std::string("__NOTE_DEFAULT") + handSuffix;
            auto it = _offsets.find(noteDefault);
            if (it == _offsets.end()) {
                noteDefault = "__NOTE_DEFAULT_L";
                it = _offsets.find(noteDefault);
            }
            if (it != _offsets.end()) {
                spdlog::info("[ItemOffsets]  NOTE default for '{}' ({})", itemName, noteDefault);
                ItemOffset result = it->second;
                result.matchedName = noteDefault;
                return result;
            }
        }

        // No exact match found - do NOT use dimension-based fallback
        spdlog::debug(
            "[ItemOffsets] No exact match for '{}' (FormID: {}, EditorID: '{}')",
            itemName,
            GetItemFormId(refr),
            GetItemEditorId(refr));
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
        
        // Priority 1: stable base FormID/editor ID. The editor fallback also
        // survives plugin load-index changes in user-authored databases.
        if (const auto stableName = FindStableLookupName(refr)) {
            if (const auto offsetIt = _offsets.find(*stableName);
                offsetIt != _offsets.end()) {
                spdlog::info(
                    "[ItemOffsets]  Priority 1: stable identity {} / '{}' -> '{}'",
                    baseFormId,
                    GetItemEditorId(refr),
                    *stableName);
                spdlog::info(
                    "[ItemOffsets]   -> pos=({:.2f}, {:.2f}, {:.2f}) fingerDist={:.2f}",
                    offsetIt->second.position.x,
                    offsetIt->second.position.y,
                    offsetIt->second.position.z,
                    offsetIt->second.fingerDistance);
                ItemOffset result = offsetIt->second;
                result.matchQuality = OffsetMatchQuality::Exact;
                result.matchedName = *stableName;
                return result;
            }
        }
        
        // Priority 2: localized display-name exact match.
        
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
        const auto stableName = FindStableLookupName(refr);
        const std::string& identityName =
            stableName ? *stableName : itemName;
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
                if (it == _offsets.end()) {
                    // Embedded entry is stored as "__NOTE_DEFAULT_L"; for the
                    // right hand fall back to it and let the mirror system flip.
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
            std::string paHandName =
                identityName + handSuffix + "_PA";
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
            std::string paName = identityName + "_PA";
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
            std::string handName = identityName + handSuffix;
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

    std::optional<ItemOffset> ItemOffsetManager::GetExactDimensionsOffset(RE::TESObjectREFR* refr, bool isLeft) const
    {
        if (!refr) {
            return std::nullopt;
        }

        // Item bounds (int16 OBND extents). Two items built from the same mesh share identical
        // bounds, so a strict equality test reliably groups visual duplicates together.
        float L = 0, W = 0, H = 0;
        GetItemDimensions(const_cast<RE::TESObjectREFR*>(refr), L, W, H);
        if (L <= 0 || W <= 0 || H <= 0) {
            return std::nullopt;  // need valid dimensions to match
        }

        std::string itemName = GetItemName(refr);
        std::string itemType = GetItemType(const_cast<RE::TESObjectREFR*>(refr));

        const ItemOffset* best = nullptr;
        std::string bestName;
        int bestRank = -1;  // higher = better: same-type and non-variant base entries win

        for (const auto& [candName, candOffset] : _offsets) {
            if (candOffset.length <= 0 || candOffset.width <= 0 || candOffset.height <= 0) {
                continue;
            }
            // 100% EXACT dimensions — no tolerance whatsoever
            if (candOffset.length != L || candOffset.width != W || candOffset.height != H) {
                continue;
            }
            // Rank candidates: prefer same form type (ALCH<->ALCH, avoids a chem inheriting a
            // MISC item that happens to share bounds), and prefer the non-hand/PA/throwable base
            // entry so the mirror logic in StartGrab behaves predictably.
            //
            // BASE-GAME PREFERENCE (highest weight). Several authored entries can share one
            // bound box - two stimpak-shaped items both measure (11, 24, 2) - and before this
            // the winner was whichever the unordered_map happened to yield first, i.e. neither
            // deterministic nor principled. Live report: the Sim Settlements 2 item
            // 'SS2_C2_DiseaseCureAllKnown' borrowed the MOD entry 'Used Stimpak' (formId
            // 4800896E) instead of vanilla '[Stimpak] Stimpak' (00023736) and was held wrongly.
            // A base-game donor is the safer default: its FormID is stable across load orders
            // (a mod entry's plugin index means something different on every rig), and its pose
            // was authored against the canonical mesh.
            // WEIGHTING ORDER MATTERS, and getting it wrong is a live bug, not a nicety.
            // Same form type must DOMINATE, because it is the strongest signal that two items
            // are held the same way; base-game origin only breaks ties among equally-suitable
            // donors. Shipping base-game at a higher weight than type produced exactly the
            // failure the owner reported: an ALCH Nuka-Cola variant borrowed the base-game
            // MISC '[Bottle] Nuka-Cola Bottle{{{Glass}}}' (the EMPTY bottle, 0004835A) instead
            // of an ALCH drink, because +4 base-game beat +2 same-type. Sixteen authored
            // entries share the 8x8x20/21 bottle bound box, so this collision is routine here.
            int rank = 0;
            if (!itemType.empty() && candOffset.itemType == itemType) rank += 8;
            bool isVariant = candOffset.isLeftHanded || candOffset.isPowerArmor || candOffset.isThrowable;
            if (!isVariant) rank += 1;
            const bool isBaseGameForm =
                candOffset.formId.size() == 8 &&
                (candOffset.formId.compare(0, 2, "00") == 0 ||
                 candOffset.formId.compare(0, 2, "01") == 0);
            if (isBaseGameForm) rank += 4;

            // Deterministic tiebreak. Equal-ranked candidates must resolve the same way on
            // every machine and every launch, so fall back to the lexicographically smaller
            // name instead of hash order.
            if (rank > bestRank || (rank == bestRank && best && candName < bestName)) {
                bestRank = rank;
                best = &candOffset;
                bestName = candName;
            }
        }

        if (best) {
            spdlog::info("[ItemOffsets] EXACT-DIMS offset for '{}' -> '{}' (L={:.1f} W={:.1f} H={:.1f}, type={}) pos=({:.2f},{:.2f},{:.2f})",
                        itemName, bestName, L, W, H, itemType,
                        best->position.x, best->position.y, best->position.z);
            ItemOffset result = *best;
            result.length = L;
            result.width = W;
            result.height = H;
            result.matchQuality = OffsetMatchQuality::Dimensions;
            result.matchedName = bestName;
            return result;
        }

        spdlog::debug("[ItemOffsets] GetExactDimensionsOffset: no 100%% dims match for '{}' (L={:.1f} W={:.1f} H={:.1f})",
                    itemName, L, W, H);
        return std::nullopt;
    }

    // ------------------------------------------------------------------
    // MESH-IDENTITY DONOR
    //
    // Why this exists: the offset database is keyed on FormID, editor ID and
    // localized display name. A modded item that reuses a vanilla mesh matches
    // none of those, so it fell through to generated placement even though the
    // correct authored pose was sitting right there under the vanilla item.
    // Reported live: a Spanish-locale aid item (Elzee_Wet_Ingestible) that
    // renders as a Stimpak was held wrongly, while "[Stimpak] Stimpak" has an
    // authored pose.
    //
    // Why it is safe where the ARMO dimensional donor is a guess: the same NIF
    // means the same geometry and the same pivot, so the donor's translation,
    // rotation and finger curls all apply verbatim. There is no tolerance and
    // no scoring - it either is the same model or it is not. The existing
    // __NOTE_DEFAULT rule is the same idea keyed on form type.
    // ------------------------------------------------------------------
    static std::string NormalizeModelPathKey(const char* rawPath)
    {
        if (!rawPath) {
            return {};
        }
        std::string path(rawPath);
        // Bethesda paths appear with mixed case and either slash; some are
        // written with a leading "meshes\" and some without.
        std::transform(path.begin(), path.end(), path.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(path.begin(), path.end(), '/', '\\');
        while (!path.empty() && (path.front() == '\\' || path.front() == ' ')) {
            path.erase(path.begin());
        }
        constexpr const char* kMeshesPrefix = "meshes\\";
        if (path.rfind(kMeshesPrefix, 0) == 0) {
            path.erase(0, std::char_traits<char>::length(kMeshesPrefix));
        }
        return path;
    }

    static const char* GetFormModelPath(RE::TESForm* form)
    {
        if (!form) {
            return nullptr;
        }
        if (auto* asModel = form->As<RE::TESModel>()) {
            return asModel->GetModel();
        }
        return nullptr;
    }

    void ItemOffsetManager::BuildModelPathDonorIndex() const
    {
        if (_modelPathIndexBuilt) {
            return;
        }
        _modelPathIndexBuilt = true;  // set first: a failed build must not retry every grab

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            spdlog::warn("[ItemOffsets] Model-path donor index: TESDataHandler unavailable");
            return;
        }

        std::size_t authored = 0;
        std::size_t collisions = 0;

        // Only classes where an identical mesh implies an identical hold.
        // Weapons and armour are deliberately excluded: weapons carry grip
        // semantics resolved elsewhere, and armour already has its own donor.
        const auto indexArray = [&](auto& formArray) {
            for (auto* form : formArray) {
                if (!form) {
                    continue;
                }
                const std::string modelKey = NormalizeModelPathKey(GetFormModelPath(form));
                if (modelKey.empty()) {
                    continue;
                }

                // A form contributes ONLY if it already owns an authored
                // offset, looked up through the same stable identity path the
                // normal lookup uses.
                std::string offsetKey;
                char buf[16];
                snprintf(buf, sizeof(buf), "%08X", static_cast<uint32_t>(form->formID));
                const std::string formId = NormalizeFormIdIdentity(std::string(buf));
                if (const auto it = _formIdToName.find(formId); it != _formIdToName.end()) {
                    offsetKey = it->second;
                } else {
                    const char* rawEditorId = form->GetFormEditorID();
                    if (rawEditorId) {
                        const std::string editorId = NormalizeEditorIdentity(rawEditorId);
                        if (!editorId.empty()) {
                            if (const auto eit = _editorIdToName.find(editorId);
                                eit != _editorIdToName.end()) {
                                offsetKey = eit->second;
                            }
                        }
                    }
                }
                if (offsetKey.empty()) {
                    continue;
                }

                auto [it, inserted] = _modelPathToName.emplace(modelKey, offsetKey);
                if (inserted) {
                    ++authored;
                } else if (it->second != offsetKey) {
                    // Two authored items share one mesh. Keep the
                    // lexicographically smaller key so the choice is stable
                    // across sessions instead of depending on load order.
                    ++collisions;
                    if (offsetKey < it->second) {
                        it->second = offsetKey;
                    }
                }
            }
        };

        indexArray(dataHandler->GetFormArray<RE::AlchemyItem>());
        indexArray(dataHandler->GetFormArray<RE::TESObjectMISC>());
        indexArray(dataHandler->GetFormArray<RE::IngredientItem>());
        indexArray(dataHandler->GetFormArray<RE::TESKey>());
        indexArray(dataHandler->GetFormArray<RE::TESObjectBOOK>());

        spdlog::info("[ItemOffsets] Model-path donor index built: {} authored meshes ({} shared-mesh collisions resolved)",
                     authored, collisions);
    }

    std::optional<ItemOffset> ItemOffsetManager::GetSharedModelDonorOffset(
        RE::TESObjectREFR* refr,
        bool isLeft) const
    {
        if (!refr) {
            return std::nullopt;
        }

        auto* baseForm = refr->GetObjectReference();
        if (!baseForm) {
            return std::nullopt;
        }

        const std::string modelKey = NormalizeModelPathKey(GetFormModelPath(baseForm));
        if (modelKey.empty()) {
            return std::nullopt;
        }

        BuildModelPathDonorIndex();

        const auto it = _modelPathToName.find(modelKey);
        if (it == _modelPathToName.end()) {
            return std::nullopt;
        }

        // Same form type only. A shared mesh between different classes (a MISC
        // prop reusing a weapon model, say) does not imply a shared hold.
        const std::string targetType = GetItemType(refr);
        if (const auto donorIt = _offsets.find(it->second); donorIt != _offsets.end()) {
            if (!donorIt->second.itemType.empty() &&
                !targetType.empty() &&
                donorIt->second.itemType != "UNKNOWN" &&
                targetType != "UNKNOWN" &&
                donorIt->second.itemType != targetType) {
                return std::nullopt;
            }
        }

        auto donated = GetOffset(it->second, isLeft);
        if (!donated.has_value()) {
            return std::nullopt;
        }
        donated->matchedName = it->second;
        return donated;
    }

    std::optional<ItemOffset> ItemOffsetManager::GetArmorDimensionalDonorOffset(
        RE::TESObjectREFR* refr,
        bool isLeft) const
    {
        if (!refr) {
            return std::nullopt;
        }

        // ARMO only. Every other class ties its authored pose to a specific
        // model's pivot, so borrowing across items would be a guess.
        if (GetItemType(const_cast<RE::TESObjectREFR*>(refr)) != "ARMO") {
            return std::nullopt;
        }

        float L = 0, W = 0, H = 0;
        GetItemDimensions(const_cast<RE::TESObjectREFR*>(refr), L, W, H);
        if (L <= 0 || W <= 0) {
            return std::nullopt;
        }

        // SLAB GATE — the donor is only sound between GARMENTS.
        //
        // Garments are authored around a shared body origin, so their poses
        // transfer. Chunky pieces (helmets, chest plates, gauntlets, limb
        // armour) are not, and bounds alone cannot tell them apart from a
        // folded garment of similar footprint. Leave-one-out over the embedded
        // ARMO profiles found the damage this does when unguarded: a T-45 Chest
        // Piece (34x43x34) would take 'Black Vest and Slacks' (34x42x12) at an
        // XZ distance of 1.00 and land 41.7 units out; 'Cage Armor' would take
        // 'Super Mutant Heavy Gauntlets' and land 52.1 units and 178 degrees
        // out. Requiring BOTH sides to be slab-shaped removes that whole class,
        // and a non-garment simply keeps the generated seat.
        const auto isSlabShaped = [](float l, float w, float h) {
            if (!(l > 0.0f) || !(w > 0.0f) || !(h > 0.0f)) {
                return false;
            }
            const float largest = (std::max)(l, (std::max)(w, h));
            const float smallest = (std::min)(l, (std::min)(w, h));
            const float middle = l + w + h - largest - smallest;
            return middle > 0.0f &&
                   (smallest / middle) <= kArmorDonorMaxThicknessRatio;
        };
        if (!isSlabShaped(L, W, H)) {
            return std::nullopt;
        }

        const std::string targetName = GetItemName(refr);
        const bool targetIsLeftPiece = ContainsCI(targetName.c_str(), "left");
        const bool targetIsRightPiece = ContainsCI(targetName.c_str(), "right");

        const std::string handSuffix = isLeft ? "_L" : "_R";
        const bool inPowerArmor = Utils::IsPlayerInPowerArmor();
        const ItemOffset* best = nullptr;
        std::string bestName;
        float bestDistance = kArmorDonorMaxXZDistance;
        int bestRank = -1;

        for (const auto& [candName, candOffset] : _offsets) {
            if (candOffset.itemType != "ARMO") {
                continue;
            }
            if (candOffset.length <= 0 || candOffset.width <= 0) {
                continue;
            }
            if (!isSlabShaped(candOffset.length, candOffset.width, candOffset.height)) {
                continue;
            }
            // A power-armor pose is authored in a different hand frame, so it
            // must never reach a normal grab. The reverse is only a preference:
            // no embedded ARMO profile carries a PA variant, so hard-rejecting
            // non-PA donors while in power armor would silently disable the
            // whole feature there. Rank instead (below).
            if (candOffset.isPowerArmor && !inPowerArmor) {
                continue;
            }
            // Mirrored armour PIECES have identical bounds but opposite poses
            // ("Metal Left Leg" vs "Metal Right Leg" measured 177 degrees apart
            // in the embedded data). Bounds alone cannot separate them, so
            // never let one side donate to the other.
            const bool candIsLeftPiece = ContainsCI(candName.c_str(), "left");
            const bool candIsRightPiece = ContainsCI(candName.c_str(), "right");
            if ((targetIsLeftPiece && candIsRightPiece) ||
                (targetIsRightPiece && candIsLeftPiece)) {
                continue;
            }

            // Footprint (L/W) only, matching how garments actually differ: a
            // folded garment's HEIGHT is drape, not identity, and that is the
            // exact reason the strict all-three-axes test missed these items
            // (BoS Uniform 34x42x10 vs the authored 34x42x12). Measured over
            // the embedded profiles, an L/W bound leaves fewer catastrophic
            // (>15u) transfers than folding height into the distance.
            const float dL = std::abs(candOffset.length - L);
            const float dW = std::abs(candOffset.width - W);
            const float xzDistance = std::sqrt(dL * dL + dW * dW);
            if (!std::isfinite(xzDistance) || xzDistance > bestDistance) {
                continue;
            }

            // Prefer this hand's authored variant, then a power-armor variant
            // when in power armor, when two donors are equally close.
            int rank = 0;
            if (candName.size() > handSuffix.size() &&
                candName.compare(
                    candName.size() - handSuffix.size(),
                    handSuffix.size(),
                    handSuffix) == 0) {
                rank += 2;
            }
            if (inPowerArmor && candOffset.isPowerArmor) {
                rank += 1;
            }
            // _offsets is an unordered_map, so ties must be broken by the data
            // and not by bucket order — otherwise which garment donates could
            // change between builds for no visible reason.
            if (xzDistance < bestDistance ||
                (xzDistance == bestDistance &&
                    (rank > bestRank ||
                        (rank == bestRank && (!best || candName < bestName))))) {
                bestDistance = xzDistance;
                bestRank = rank;
                best = &candOffset;
                bestName = candName;
            }
        }

        if (!best) {
            return std::nullopt;
        }

        spdlog::info(
            "[ItemOffsets] ARMOR donor for '{}' -> '{}' (XZ distance {:.2f}; "
            "target L={:.1f} W={:.1f} H={:.1f}, donor L={:.1f} W={:.1f} H={:.1f}) "
            "pos=({:.2f},{:.2f},{:.2f})",
            GetItemName(refr),
            bestName,
            bestDistance,
            L, W, H,
            best->length, best->width, best->height,
            best->position.x, best->position.y, best->position.z);

        ItemOffset result = *best;
        result.length = L;
        result.width = W;
        result.height = H;
        result.matchQuality = (bestDistance < 0.1f)
            ? OffsetMatchQuality::Dimensions
            : OffsetMatchQuality::Fuzzy;
        result.matchedName = bestName;
        result.armorDonorXZDistance = bestDistance;
        return result;
    }

    bool ItemOffsetManager::HasExactMatch(RE::TESObjectREFR* refr) const
    {
        if (!refr) {
            return false;
        }

        // Stable FormID/editor identity is authoritative and independent of
        // the language used for the display name.
        if (const auto stableName = FindStableLookupName(refr)) {
            if (_offsets.contains(*stableName) ||
                _offsets.contains(*stableName + "_L") ||
                _offsets.contains(*stableName + "_R")) {
                return true;
            }
        }

        // Localized display name remains a backwards-compatible fallback.
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
