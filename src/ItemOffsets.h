#pragma once

#include <string>
#include <unordered_map>
#include <optional>

namespace heisenberg
{
    /**
     * Match quality for offset selection - indicates how the offset was matched
     */
    enum class OffsetMatchQuality
    {
        Exact,      // Priority 1-2: FormID or exact name match
        Dimensions, // Priority 3-6: Exact dimensions match
        Partial,    // Priority 7: Partial name match (e.g., "Tire Iron" matched to "Tire")
        Fuzzy,      // Priority 8-15: Fuzzy dimension or ratio-based match
        None        // No match - calculated snap-to-hand offset
    };

    /**
     * Item grab offset data - stores position and rotation offsets for grabbed items
     */
    struct ItemOffset
    {
        RE::NiPoint3 position{0.0f, 0.0f, 0.0f};  // Position offset in hand-local space
        RE::NiMatrix3 rotation;                    // Rotation offset
        float scale = 1.0f;                        // Scale multiplier
        
        // Item dimensions (bounding box) - recorded from the object when grabbed
        float length = 0.0f;   // X dimension (left-right)
        float width = 0.0f;    // Y dimension (forward-back)  
        float height = 0.0f;   // Z dimension (up-down)
        
        // Distance from finger tips to item center (for automatic finger curl calculation)
        // Used with FRIK's setHandPoseFingerPositions to align fingers with item
        float fingerDistance = 0.0f;  // Distance from index finger tip to item position
        
        // Individual finger curl values (0.0 = fully extended, 1.0 = fully curled)
        // Set during repositioning mode with thumbstick clicks
        float thumbCurl = 0.5f;
        float indexCurl = 0.5f;
        float middleCurl = 0.5f;
        float ringCurl = 0.5f;
        float pinkyCurl = 0.5f;
        bool hasFingerCurls = false;  // True if user has set custom finger curls

        // Per-joint curl values (15 values: 5 fingers x 3 joints)
        // [thumb_prox, thumb_med, thumb_dist, index_prox..dist, middle..., ring..., pinky...]
        // Values: 0.0 = bent, 1.0 = straight
        float jointCurls[15] = {1,1,1, 1,1,1, 1,1,1, 1,1,1, 1,1,1};
        bool hasJointCurls = false;
        
        // Item metadata
        std::string itemType;  // Form type (e.g., "MISC", "WEAP", "ALCH", etc.)
        std::string formId;    // Form ID as hex string for reference
        
        // Coordinate system flag
        // true = offset stored in RIGHT hand space (FRIK weapon offsets)
        // false = offset stored in LEFT hand space (original embedded offsets)
        bool isRightHandSpace = false;
        
        // FRIK offset flag - true if this offset came from FRIK weapon offset database
        // Used to apply FRIK-specific parent node transforms during grab
        bool isFRIKOffset = false;
        
        // Variant flags - used for selecting correct offset based on context
        bool isLeftHanded = false;   // true = left-handed variant (for bLeftHandedMode:VR)
        bool isPowerArmor = false;   // true = power armor variant (when player is in PA)
        bool isThrowable = false;    // true = throwable variant (for grenades/mines when equipped)
        
        // Match quality - indicates how this offset was selected (set by GetOffsetWithFallback)
        OffsetMatchQuality matchQuality = OffsetMatchQuality::None;
        
        // Matched name - the lookup key that was matched (e.g., "Baton" for partial match)
        // Used to check for left-handed variants after partial name matching
        std::string matchedName;
        
        ItemOffset()
        {
            // Initialize rotation to identity
            rotation.MakeIdentity();
        }
    };

    /**
     * Manager for item grab offsets
     * Loads/saves offsets from JSON files for proper hand positioning when grabbing items
     */
    class ItemOffsetManager
    {
    public:
        static ItemOffsetManager& GetSingleton()
        {
            static ItemOffsetManager instance;
            return instance;
        }

        // Initialize - load offsets from JSON files
        void Initialize();

        // Get offset for an item by form ID or name
        std::optional<ItemOffset> GetOffset(RE::TESObjectREFR* refr) const;
        std::optional<ItemOffset> GetOffset(const std::string& itemName) const;
        
        // Get offset for an item with per-hand support
        // If a hand-specific offset exists (e.g., "ItemName_L" or "ItemName_R"), use it
        // Otherwise fall back to the generic offset
        std::optional<ItemOffset> GetOffset(RE::TESObjectREFR* refr, bool isLeft) const;
        std::optional<ItemOffset> GetOffset(const std::string& itemName, bool isLeft) const;

        // Save offset for an item
        void SaveOffset(RE::TESObjectREFR* refr, const ItemOffset& offset);
        void SaveOffset(const std::string& itemName, const ItemOffset& offset);
        
        // Save offset for a specific hand
        void SaveOffset(RE::TESObjectREFR* refr, const ItemOffset& offset, bool isLeft);
        void SaveOffset(const std::string& itemName, const ItemOffset& offset, bool isLeft);

        // Check if we have an offset for this item
        bool HasOffset(const std::string& itemName) const;
        
        // Get the item name for saving (uses base form name)
        static std::string GetItemName(RE::TESObjectREFR* refr);

        // Get the item type as a string (e.g., "MISC", "WEAP", "ALCH")
        static std::string GetItemType(RE::TESObjectREFR* refr);

        // Get the form ID as a hex string
        static std::string GetItemFormId(RE::TESObjectREFR* refr);

        // Get item dimensions from its bounding box
        static void GetItemDimensions(RE::TESObjectREFR* refr, float& outLength, float& outWidth, float& outHeight);

        // Find a similar item offset based on dimensions/aspect ratio
        // Calculates normalized shape ratios and finds the closest match from stored offsets
        // Returns the best matching offset, or nullopt if no offsets are loaded
        std::optional<ItemOffset> FindSimilarOffset(float length, float width, float height) const;

        // Get offset with dimension-based fallback matching
        // If no exact match exists, tries to find a similar item by dimensions
        std::optional<ItemOffset> GetOffsetWithFallback(RE::TESObjectREFR* refr) const;
        
        // Get offset with per-hand support and dimension-based fallback matching
        // First checks for hand-specific offset (e.g., "ItemName_L"), then falls back to generic
        std::optional<ItemOffset> GetOffsetWithFallback(RE::TESObjectREFR* refr, bool isLeft) const;
        
        // Get throwable offset for an item (grenades, mines, etc.)
        // Used when player has a grenade equipped and ready to throw
        // Priority: 1. PA+throwable (if in PA), 2. throwable, 3. nullopt
        std::optional<ItemOffset> GetThrowableOffset(const std::string& itemName, bool isLeft = false) const;

        // Check if an item has an EXACT dimensions match (for armor grab filtering)
        // Returns true only if there's an offset with EXACTLY the same L/W/H dimensions (no tolerance)
        // This is used to allow armor grabbing ONLY when we have a perfect offset for it
        bool HasExactDimensionsMatch(RE::TESObjectREFR* refr) const;
        
        // Check if an item has an EXACT match (Priority 1-2: FormID or name match)
        // Used to determine if extended natural grab distance should be used
        bool HasExactMatch(RE::TESObjectREFR* refr) const;
        
        // Get EXACT offset match only (FormID or exact name, with hand-specific support)
        // Does NOT use dimension-based fallback or partial name matching
        // Used for MSTT items where only stored offsets should be used
        std::optional<ItemOffset> GetExactOffset(RE::TESObjectREFR* refr, bool isLeft) const;

        // Get default offset (used when no specific offset is saved)
        const ItemOffset& GetDefaultOffset() const { return _defaultOffset; }

    private:
        ItemOffsetManager() = default;
        ~ItemOffsetManager() = default;

        ItemOffsetManager(const ItemOffsetManager&) = delete;
        ItemOffsetManager& operator=(const ItemOffsetManager&) = delete;

        // Load embedded offsets (250 pre-configured items)
        void LoadEmbeddedOffsets();

        // Load offsets from JSON files in the config folder (user overrides)
        void LoadOffsetsFromFilesystem();

        // Load a single JSON file
        void LoadOffsetJsonFile(const std::string& filePath);

        // Save offset to JSON file
        void SaveOffsetToJsonFile(const std::string& itemName, const ItemOffset& offset);

        // Path to offsets folder
        static std::string GetOffsetsPath();

        // Stored offsets by item name
        std::unordered_map<std::string, ItemOffset> _offsets;
        
        // Secondary index by form ID (hex string like "000211C4")
        // Allows faster lookup when we have the form ID
        std::unordered_map<std::string, std::string> _formIdToName;

        // Default offset for items without specific settings
        ItemOffset _defaultOffset;

        bool _initialized = false;
    };
}
