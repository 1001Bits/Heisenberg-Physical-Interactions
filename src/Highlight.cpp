#include "PCH.h"
#include "Highlight.h"
#include "Config.h"

namespace heisenberg
{
    bool Highlighter::Initialize()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_initialized) {
            return _highlightShader != nullptr;
        }
        _initialized = true;

        // Try to find a suitable highlight shader
        _highlightShader = FindHighlightShader();
        
        if (_highlightShader) {
            spdlog::info("[HIGHLIGHT] Initialized with shader FormID {:08X}", _highlightShader->formID);
            return true;
        } else {
            spdlog::info("[HIGHLIGHT] No suitable effect shader found - highlighting disabled");
            return false;
        }
    }

    RE::TESEffectShader* Highlighter::FindHighlightShader()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            spdlog::debug("[HIGHLIGHT] TESDataHandler not available");
            return nullptr;
        }

        // Get all effect shaders
        auto& shaders = dataHandler->GetFormArray<RE::TESEffectShader>();
        spdlog::debug("[HIGHLIGHT] Found {} effect shaders in game data", shaders.size());

        // Known good shader FormIDs to try (from base game):
        // These are common highlight/glow shaders used by the vanilla game
        const std::vector<RE::TESFormID> preferredShaders = {
            0x00023D72,  // DefaultPowerArmorBrightEffect (good glow)
            0x00021EC5,  // WorkshopBuildableHighlight
            0x0023E5D1,  // DefaultHighlight (if exists)
            0x001E9A94,  // VATSTargetFXShader
            0x00023D6F,  // PowerArmorActivateShader
        };

        // Try preferred shaders first
        for (RE::TESFormID formID : preferredShaders) {
            auto* form = RE::TESForm::GetFormByID(formID);
            if (form && form->formType == RE::ENUM_FORM_ID::kEFSH) {
                auto* shader = static_cast<RE::TESEffectShader*>(form);
                spdlog::debug("[HIGHLIGHT] Found preferred shader FormID {:08X}", formID);
                return shader;
            }
        }

        // Fallback: use any valid effect shader we can find
        for (auto* shader : shaders) {
            if (shader) {
                spdlog::debug("[HIGHLIGHT] Using fallback shader FormID {:08X}", shader->formID);
                return shader;
            }
        }

        return nullptr;
    }

    void Highlighter::StartHighlight(RE::TESObjectREFR* refr)
    {
        if (!refr || !_highlightShader) {
            return;
        }

        std::lock_guard<std::mutex> lock(_mutex);

        // Check if already highlighted
        if (_activeHighlights.contains(refr->formID)) {
            return;  // Already highlighted
        }

        // Apply the effect shader using native API
        // Parameters:
        //   shader: The effect shader to apply
        //   duration: Use finite duration (5 seconds) instead of infinite (-1.0f)
        //             CRITICAL: Infinite duration effects persist in saves and can cause
        //             Scaleform crashes on load! HIGGS Skyrim uses 60 second duration.
        //             We use 5 seconds and would need to refresh, but at least this
        //             prevents orphaned infinite effects in saves.
        //   facingRef: nullptr (not aiming at anything)
        //   attachToCamera: false
        //   inheritRotation: false
        //   attachNode: nullptr (apply to whole object)
        //   interfaceEffect: false
        auto* effect = refr->ApplyEffectShader(
            _highlightShader,
            5.0f,       // 5 second duration (NOT infinite - prevents save corruption)
            nullptr,    // No facing reference
            false,      // Don't attach to camera
            false,      // Don't inherit rotation
            nullptr,    // Apply to whole object (no specific node)
            false       // Not an interface effect
        );

        if (effect) {
            // Store the effect pointer so we can Detach() it later
            _activeHighlights[refr->formID] = effect;
            spdlog::debug("[HIGHLIGHT] Started highlight on FormID {:08X}, effect at {:X}", refr->formID, (uintptr_t)effect);
        } else {
            spdlog::debug("[HIGHLIGHT] Failed to apply effect shader to FormID {:08X}", refr->formID);
        }
    }

    void Highlighter::StopHighlight(RE::TESObjectREFR* refr)
    {
        if (!refr || !_highlightShader) {
            return;
        }

        std::lock_guard<std::mutex> lock(_mutex);

        // Check if actually highlighted and get the effect pointer
        auto it = _activeHighlights.find(refr->formID);
        if (it == _activeHighlights.end()) {
            return;  // Not highlighted
        }

        // Get the effect and detach it
        RE::ShaderReferenceEffect* effect = it->second;
        if (effect) {
            // Use ReferenceEffect::Detach() to stop the effect
            ReferenceEffect_Detach(effect);
            spdlog::debug("[HIGHLIGHT] Stopped highlight on FormID {:08X} via Detach()", refr->formID);
        }

        _activeHighlights.erase(it);
    }

    void Highlighter::StopAllHighlights()
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_activeHighlights.empty()) {
            return;
        }

        // Detach each active effect
        for (auto& [formID, effect] : _activeHighlights) {
            if (effect) {
                ReferenceEffect_Detach(effect);
            }
        }

        spdlog::debug("[HIGHLIGHT] Stopped all {} active highlights", _activeHighlights.size());
        _activeHighlights.clear();
    }

    bool Highlighter::IsHighlighted(RE::TESObjectREFR* refr) const
    {
        if (!refr) {
            return false;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        return _activeHighlights.contains(refr->formID);
    }

}  // namespace heisenberg
