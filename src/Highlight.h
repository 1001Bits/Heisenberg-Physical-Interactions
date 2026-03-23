#pragma once

namespace heisenberg
{
    /**
     * Handles visual highlighting of selected/grabbable objects.
     * Uses Fallout 4's native effect shader system via TESObjectREFR::ApplyEffectShader.
     * 
     * The highlight appears as a glowing outline on objects that can be grabbed,
     * providing visual feedback to the player about what they're pointing at.
     */
    class Highlighter
    {
    public:
        static Highlighter& GetSingleton()
        {
            static Highlighter instance;
            return instance;
        }

        /**
         * Initialize the highlighter - must be called after game data is loaded.
         * Finds appropriate effect shaders for highlighting.
         * @return true if initialization succeeded
         */
        bool Initialize();

        /**
         * Apply highlight effect to an object.
         * Safe to call multiple times - will not double-highlight.
         * @param refr The reference to highlight
         */
        void StartHighlight(RE::TESObjectREFR* refr);

        /**
         * Remove highlight effect from an object.
         * Safe to call even if object isn't highlighted.
         * @param refr The reference to stop highlighting
         */
        void StopHighlight(RE::TESObjectREFR* refr);

        /**
         * Remove all active highlights.
         * Call on cell change or when disabling the mod.
         */
        void StopAllHighlights();

        /**
         * Clear all highlight tracking without calling Detach().
         * Use this on save/load when ShaderReferenceEffect pointers are already invalid.
         * CRITICAL: The old pointers are dangling after load - do NOT call Detach() on them.
         */
        void ClearAllHighlights()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _activeHighlights.clear();
            spdlog::info("[HIGHLIGHT] Cleared all highlight tracking (save/load cleanup)");
        }

        /**
         * Check if a specific object is currently highlighted.
         * @param refr The reference to check
         * @return true if the object has an active highlight
         */
        bool IsHighlighted(RE::TESObjectREFR* refr) const;

        /**
         * Check if highlighting is available (initialized with valid shader).
         * @return true if StartHighlight will work
         */
        bool IsAvailable() const { return _initialized && _highlightShader != nullptr; }

        /**
         * Get the current highlight shader being used.
         * @return The TESEffectShader, or nullptr if not initialized
         */
        RE::TESEffectShader* GetShader() const { return _highlightShader; }

        /**
         * Set a custom highlight shader.
         * @param shader The effect shader to use (nullptr to disable)
         */
        void SetShader(RE::TESEffectShader* shader) { _highlightShader = shader; }

    private:
        Highlighter() = default;
        ~Highlighter() = default;

        Highlighter(const Highlighter&) = delete;
        Highlighter& operator=(const Highlighter&) = delete;

        // Try to find a suitable highlight shader from game data
        RE::TESEffectShader* FindHighlightShader();

        bool _initialized = false;
        RE::TESEffectShader* _highlightShader = nullptr;
        
        // Track currently highlighted objects: formID -> ShaderReferenceEffect*
        // We store the effect pointer so we can call Detach() on it to stop the effect
        std::unordered_map<RE::TESFormID, RE::ShaderReferenceEffect*> _activeHighlights;
        
        // Lock for thread safety
        mutable std::mutex _mutex;
    };

    // =========================================================================
    // F4VR Offset for ReferenceEffect::Detach() - Status 4 (Verified)
    // This is the reliable way to stop an effect
    // =========================================================================
    using _ReferenceEffect_Detach = void(*)(RE::ReferenceEffect* effect);
    inline REL::Relocation<_ReferenceEffect_Detach> ReferenceEffect_Detach{ REL::Offset(0xcc2cd0) };

}  // namespace heisenberg
