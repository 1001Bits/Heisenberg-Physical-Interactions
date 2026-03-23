/**
 * HeisenbergInterface.cpp - Implementation of the Heisenberg public API
 * 
 * This provides the concrete implementation of IHeisenbergInterface001 that
 * other plugins can use to interact with Heisenberg.
 */

#include "HeisenbergInterface001.h"
#include "Heisenberg.h"
#include "Grab.h"
#include "Hand.h"
#include "Config.h"
#include "DropToHand.h"
#include "SmartGrabHandler.h"
#include "ActivatorHandler.h"
#include "HandCollision.h"
#include "MenuChecker.h"
#include "VirtualHolstersAPI.h"
#include "F4VROffsets.h"

namespace HeisenbergPluginAPI {

    // Build number - increment when API changes
    constexpr unsigned int HEISENBERG_BUILD_NUMBER = 1;

    // =========================================================================
    // Callback storage
    // =========================================================================
    
    static std::vector<IHeisenbergInterface001::GrabbedCallback> g_grabbedCallbacks;
    static std::vector<IHeisenbergInterface001::DroppedCallback> g_droppedCallbacks;
    static std::vector<IHeisenbergInterface001::StashedCallback> g_stashedCallbacks;
    static std::vector<IHeisenbergInterface001::ConsumedCallback> g_consumedCallbacks;
    static std::vector<IHeisenbergInterface001::PulledCallback> g_pulledCallbacks;
    static std::vector<IHeisenbergInterface001::CollisionCallback> g_collisionCallbacks;
    static std::vector<IHeisenbergInterface001::PrePhysicsCallback> g_prePhysicsCallbacks;
    static std::vector<IHeisenbergInterface001::PostPhysicsCallback> g_postPhysicsCallbacks;
    static std::vector<IHeisenbergInterface001::ViewCasterTargetChangedCallback> g_viewCasterCallbacks;

    // Per-hand state
    static bool g_leftHandDisabled = false;
    static bool g_rightHandDisabled = false;

    // =========================================================================
    // Callback invocation functions (called from Heisenberg internals)
    // =========================================================================
    
    void InvokeGrabbedCallbacks(bool isLeft, RE::TESObjectREFR* refr)
    {
        for (auto& cb : g_grabbedCallbacks) {
            if (cb) cb(isLeft, refr);
        }
    }

    void InvokeDroppedCallbacks(bool isLeft, RE::TESObjectREFR* refr)
    {
        for (auto& cb : g_droppedCallbacks) {
            if (cb) cb(isLeft, refr);
        }
    }

    void InvokeStashedCallbacks(bool isLeft, RE::TESForm* form)
    {
        for (auto& cb : g_stashedCallbacks) {
            if (cb) cb(isLeft, form);
        }
    }

    void InvokeConsumedCallbacks(bool isLeft, RE::TESForm* form)
    {
        for (auto& cb : g_consumedCallbacks) {
            if (cb) cb(isLeft, form);
        }
    }

    void InvokePulledCallbacks(bool isLeft, RE::TESObjectREFR* refr)
    {
        for (auto& cb : g_pulledCallbacks) {
            if (cb) cb(isLeft, refr);
        }
    }

    void InvokeCollisionCallbacks(bool isLeft, float mass, float velocity)
    {
        for (auto& cb : g_collisionCallbacks) {
            if (cb) cb(isLeft, mass, velocity);
        }
    }

    void InvokePrePhysicsCallbacks(void* bhkWorld)
    {
        for (auto& cb : g_prePhysicsCallbacks) {
            if (cb) cb(bhkWorld);
        }
    }

    void InvokePostPhysicsCallbacks(void* bhkWorld)
    {
        for (auto& cb : g_postPhysicsCallbacks) {
            if (cb) cb(bhkWorld);
        }
    }

    void InvokeViewCasterTargetChangedCallbacks(bool isLeft, RE::TESObjectREFR* newTarget, RE::TESObjectREFR* oldTarget)
    {
        for (auto& cb : g_viewCasterCallbacks) {
            if (cb) cb(isLeft, newTarget, oldTarget);
        }
    }

    // =========================================================================
    // Interface implementation
    // =========================================================================
    
    class HeisenbergInterface001Impl : public IHeisenbergInterface001
    {
    public:
        // =====================================================================
        // VERSION INFO
        // =====================================================================
        
        unsigned int GetBuildNumber() override
        {
            return HEISENBERG_BUILD_NUMBER;
        }

        // =====================================================================
        // GRAB STATE QUERIES
        // =====================================================================
        
        bool IsHoldingObject(bool isLeft) override
        {
            return heisenberg::GrabManager::GetSingleton().IsGrabbing(isLeft);
        }

        bool IsPulling(bool isLeft) override
        {
            return heisenberg::GrabManager::GetSingleton().IsPulling(isLeft);
        }

        bool CanGrabObject(bool isLeft) override
        {
            // Check if hand is disabled
            if (isLeft ? g_leftHandDisabled : g_rightHandDisabled) {
                return false;
            }
            
            // Check if already holding
            if (IsHoldingObject(isLeft)) {
                return false;
            }
            
            // Check if in a menu (game stopped)
            if (heisenberg::MenuChecker::GetSingleton().IsGameStopped()) {
                return false;
            }
            
            return true;
        }

        RE::TESObjectREFR* GetGrabbedObject(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.GetRefr();
        }

        const char* GetGrabbedNodeName(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            if (state.active && state.node) {
                return state.node->name.c_str();
            }
            return "";
        }

        // =====================================================================
        // VIEWCASTER / SELECTION QUERIES
        // =====================================================================

        RE::TESObjectREFR* GetViewCasterTarget(bool isLeft) override
        {
            // In F4VR: primary wand (isLeft=true typically) = left hand
            // secondary wand = right hand
            // Note: bLeftHandedMode swaps these
            return isLeft ? GetPrimaryWandTarget() : GetSecondaryWandTarget();
        }

        RE::TESObjectREFR* GetPrimaryWandTarget() override
        {
            RE::ObjectRefHandle handle = heisenberg::GetPrimaryViewCasterTarget();
            if (handle) {
                RE::NiPointer<RE::TESObjectREFR> ptr = handle.get();
                return ptr.get();
            }
            return nullptr;
        }

        RE::TESObjectREFR* GetSecondaryWandTarget() override
        {
            RE::ObjectRefHandle handle = heisenberg::GetSecondaryViewCasterTarget();
            if (handle) {
                RE::NiPointer<RE::TESObjectREFR> ptr = handle.get();
                return ptr.get();
            }
            return nullptr;
        }

        RE::TESObjectREFR* GetSelectedObject(bool isLeft) override
        {
            // Get from Heisenberg's own selection system (may differ from ViewCaster)
            auto& modInst = heisenberg::Heisenberg::GetSingleton();
            auto* hand = isLeft ? modInst.GetLeftHand() : modInst.GetRightHand();
            if (hand) {
                return hand->GetSelectedObject();
            }
            return nullptr;
        }

        // =====================================================================
        // GRAB CONTROL
        // =====================================================================

        bool GrabObject(RE::TESObjectREFR* object, bool isLeft) override
        {
            if (!object || !CanGrabObject(isLeft)) {
                return false;
            }
            
            return heisenberg::GrabManager::GetSingleton().StartGrabOnRef(object, isLeft, false, false);
        }

        void DropObject(bool isLeft, const RE::NiPoint3* throwVelocity) override
        {
            if (IsHoldingObject(isLeft)) {
                heisenberg::GrabManager::GetSingleton().EndGrab(isLeft, throwVelocity, false);
            }
        }

        void ForceEndGrab(bool isLeft) override
        {
            if (IsHoldingObject(isLeft)) {
                heisenberg::GrabManager::GetSingleton().EndGrab(isLeft, nullptr, true);
            }
        }

        // =====================================================================
        // HAND STATE CONTROL
        // =====================================================================

        void DisableHand(bool isLeft) override
        {
            if (isLeft) {
                g_leftHandDisabled = true;
            } else {
                g_rightHandDisabled = true;
            }
            spdlog::debug("[HeisenbergAPI] {} hand disabled", isLeft ? "Left" : "Right");
        }

        void EnableHand(bool isLeft) override
        {
            if (isLeft) {
                g_leftHandDisabled = false;
            } else {
                g_rightHandDisabled = false;
            }
            spdlog::debug("[HeisenbergAPI] {} hand enabled", isLeft ? "Left" : "Right");
        }

        bool IsHandDisabled(bool isLeft) override
        {
            return isLeft ? g_leftHandDisabled : g_rightHandDisabled;
        }

        // =====================================================================
        // FINGER TRACKING
        // =====================================================================

        void GetFingerCurls(bool isLeft, float values[5]) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            if (state.hasItemOffset && state.itemOffset.hasFingerCurls) {
                values[0] = state.itemOffset.thumbCurl;
                values[1] = state.itemOffset.indexCurl;
                values[2] = state.itemOffset.middleCurl;
                values[3] = state.itemOffset.ringCurl;
                values[4] = state.itemOffset.pinkyCurl;
            } else {
                // Default - return current single curl value for all fingers
                float curl = heisenberg::Heisenberg::GetSingleton().GetFingerCurlValue(isLeft);
                for (int i = 0; i < 5; i++) {
                    values[i] = curl;
                }
            }
        }

        void SetFingerCurls(bool isLeft, const float values[5]) override
        {
            // Store in the grab state's item offset
            auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            state.itemOffset.thumbCurl = values[0];
            state.itemOffset.indexCurl = values[1];
            state.itemOffset.middleCurl = values[2];
            state.itemOffset.ringCurl = values[3];
            state.itemOffset.pinkyCurl = values[4];
            state.itemOffset.hasFingerCurls = true;
            state.hasItemOffset = true;
            
            // Also set the single curl value used by Heisenberg for FRIK
            float avgCurl = (values[0] + values[1] + values[2] + values[3] + values[4]) / 5.0f;
            heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, avgCurl);
        }

        // =====================================================================
        // ZONE DETECTION
        // =====================================================================

        bool IsInStorageZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInStorageZone;
        }

        bool IsInEquipZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInEquipZone;
        }

        bool IsInMouthZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInMouthZone;
        }

        bool IsInVHZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInVHZone;
        }

        const char* GetCurrentZoneName(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            if (state.isInStorageZone) return "STORAGE";
            if (state.isInMouthZone) return "MOUTH";
            if (state.isInVHZone) return "VH_HOLSTER";
            if (state.isInEquipZone) return state.currentZoneName;
            return "";
        }

        int GetVHZoneIndex(bool /*isLeft*/) override
        {
            auto* vhApi = VirtualHolsters::RequestVirtualHolstersAPI();
            if (!vhApi) return 0;
            return static_cast<int>(vhApi->GetCurrentHolster());
        }

        // =====================================================================
        // INVENTORY INTEGRATION
        // =====================================================================

        bool DropToHand(RE::TESForm* form, bool isLeft) override
        {
            if (!form) return false;
            // Queue the item to drop to hand
            heisenberg::DropToHand::GetSingleton().QueueDropToHand(form->formID, isLeft, 1, true, false);
            return true;
        }

        bool IsInActivationZone(bool isLeft) override
        {
            return heisenberg::ActivatorHandler::GetSingleton().IsHandInPointingRange(isLeft);
        }

        // =====================================================================
        // TRANSFORM CONTROL
        // =====================================================================

        RE::NiTransform GetGrabTransform(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            RE::NiTransform transform;
            transform.translate = state.grabOffsetLocal;
            transform.rotate = state.grabRotationLocal;
            transform.scale = 1.0f;
            return transform;
        }

        void SetGrabTransform(bool isLeft, const RE::NiTransform& transform) override
        {
            auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            state.grabOffsetLocal = transform.translate;
            state.grabRotationLocal = transform.rotate;
        }

        // =====================================================================
        // CALLBACKS
        // =====================================================================

        void AddGrabbedCallback(GrabbedCallback callback) override
        {
            if (callback) g_grabbedCallbacks.push_back(callback);
        }

        void AddDroppedCallback(DroppedCallback callback) override
        {
            if (callback) g_droppedCallbacks.push_back(callback);
        }

        void AddStashedCallback(StashedCallback callback) override
        {
            if (callback) g_stashedCallbacks.push_back(callback);
        }

        void AddConsumedCallback(ConsumedCallback callback) override
        {
            if (callback) g_consumedCallbacks.push_back(callback);
        }

        void AddPulledCallback(PulledCallback callback) override
        {
            if (callback) g_pulledCallbacks.push_back(callback);
        }

        void AddCollisionCallback(CollisionCallback callback) override
        {
            if (callback) g_collisionCallbacks.push_back(callback);
        }

        void AddPrePhysicsCallback(PrePhysicsCallback callback) override
        {
            if (callback) g_prePhysicsCallbacks.push_back(callback);
        }

        void AddPostPhysicsCallback(PostPhysicsCallback callback) override
        {
            if (callback) g_postPhysicsCallbacks.push_back(callback);
        }

        void AddViewCasterTargetChangedCallback(ViewCasterTargetChangedCallback callback) override
        {
            if (callback) g_viewCasterCallbacks.push_back(callback);
        }

        // =====================================================================
        // SETTINGS
        // =====================================================================

        bool GetSettingDouble(const char* name, double& out) override
        {
            // TODO: Implement setting lookup from Config
            // For now, just return false
            (void)name;
            (void)out;
            return false;
        }

        bool SetSettingDouble(const char* name, double val) override
        {
            // TODO: Implement setting modification
            (void)name;
            (void)val;
            return false;
        }

        // =====================================================================
        // HAND COLLISION
        // =====================================================================

        bool IsHandCollisionEnabled() override
        {
            return heisenberg::g_config.enableHandCollision;
        }

        void* GetHandRigidBody(bool isLeft) override
        {
            if (!IsHandCollisionEnabled()) return nullptr;
            const auto& body = heisenberg::HandCollision::GetSingleton().GetHandBody(isLeft);
            if (!body.IsValid()) return nullptr;
            // Return the hknpWorld pointer - caller can use bodyId from GetHandBody() for full access
            // This is somewhat limited but avoids exposing internal structures
            return body.hknpWorld;
        }

        bool IsHandInContact(bool isLeft) override
        {
            return heisenberg::HandCollision::GetSingleton().IsInContact(isLeft);
        }

        RE::TESObjectREFR* GetHandContactObject(bool isLeft) override
        {
            return heisenberg::HandCollision::GetSingleton().GetContactObject(isLeft);
        }
    };

    // =========================================================================
    // Singleton instance
    // =========================================================================
    
    static HeisenbergInterface001Impl g_interfaceImpl;

    // =========================================================================
    // API FUNCTION CALLBACK (for messaging system)
    // =========================================================================
    
    /**
     * Returns the interface pointer for the requested revision.
     * Called by other plugins via the HeisenbergMessage callback.
     * 
     * @param revisionNumber 1 for IHeisenbergInterface001
     * @return Interface pointer, or nullptr if revision not supported
     */
    void* GetApi(unsigned int revisionNumber)
    {
        switch (revisionNumber)
        {
        case 1:
            spdlog::info("[HeisenbergAPI] GetApi(1) - returning IHeisenbergInterface001");
            return static_cast<void*>(&g_interfaceImpl);
        default:
            spdlog::warn("[HeisenbergAPI] GetApi({}) - unknown revision", revisionNumber);
            return nullptr;
        }
    }

    /**
     * Handle interface request messages from other plugins.
     * Call this from your F4SE message handler when receiving kMessage_GetInterface.
     * 
     * @param message The HeisenbergMessage struct from the dispatch
     */
    void HandleInterfaceRequest(HeisenbergMessage* message)
    {
        if (message)
        {
            message->GetApiFunction = &GetApi;
            spdlog::info("[HeisenbergAPI] Interface request handled, callback set");
        }
    }

    // =========================================================================
    // CLIENT-SIDE: Get interface from messaging
    // =========================================================================

    IHeisenbergInterface001* GetHeisenbergInterface001(
        const F4SE::PluginHandle& pluginHandle,
        F4SE::MessagingInterface* messagingInterface)
    {
        // If already fetched, return cached pointer
        static IHeisenbergInterface001* s_cachedInterface = nullptr;
        if (s_cachedInterface)
        {
            return s_cachedInterface;
        }

        if (!messagingInterface)
        {
            spdlog::warn("[HeisenbergAPI] GetHeisenbergInterface001: messagingInterface is null");
            return nullptr;
        }

        // Dispatch message to Heisenberg to get the API callback
        HeisenbergMessage msg{};
        bool dispatched = messagingInterface->Dispatch(
            HeisenbergMessage::kMessage_GetInterface,
            &msg,
            sizeof(HeisenbergMessage),
            "Heisenberg_F4VR");

        if (!dispatched || !msg.GetApiFunction)
        {
            spdlog::warn("[HeisenbergAPI] GetHeisenbergInterface001: Heisenberg not loaded or dispatch failed");
            return nullptr;
        }

        // Get the interface for revision 1
        s_cachedInterface = static_cast<IHeisenbergInterface001*>(msg.GetApiFunction(1));
        if (s_cachedInterface)
        {
            spdlog::info("[HeisenbergAPI] GetHeisenbergInterface001: obtained interface build {}",
                         s_cachedInterface->GetBuildNumber());
        }
        
        return s_cachedInterface;
    }

} // namespace HeisenbergPluginAPI

// Global interface pointer (for internal use)
HeisenbergPluginAPI::IHeisenbergInterface001* g_heisenbergInterface = nullptr;
