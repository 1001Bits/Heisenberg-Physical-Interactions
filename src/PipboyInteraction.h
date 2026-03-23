#pragma once
// Pipboy Physical Interaction for Heisenberg F4VR
// Handles eject button press, tape deck animation, and holotape insertion.
//
// Tape deck animation uses per-frame lerp on the main thread (matching FRIK approach).
// Each frame, _tapeDeckAnimProgress moves toward target (0.0=closed, 1.0=open) by animSpeed.
// Absolute rotation is set via MatrixUtils::getMatrixFromEulerAngles — no incremental multiply.
//
// Animated nodes (under LArm_ForeArm3):
//   TapeDeck01_mesh:1   - Tray mesh, rotates on X axis (-20° when open)
//   TapeDeckLid_mesh:1  - Lid mesh, rotates on X axis (+18° when open)
//   TapREF              - Holotape mesh inside deck, shown/hidden based on load state
//   EjectButton_mesh:0  - Button mesh, translates Z based on finger proximity

#include "RE/Fallout.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace heisenberg
{
    enum class TapeDeckState
    {
        Closed,
        Opening,
        Open,
        Pushing,   // Hand is physically pushing the deck closed (tracks hand distance)
        Closing    // Committed to closing (auto-animation)
    };

    class PipboyInteraction
    {
    public:
        static PipboyInteraction& GetSingleton()
        {
            static PipboyInteraction instance;
            return instance;
        }

        // Call from main update loop (Heisenberg.cpp OnFrameUpdate)
        void OnFrameUpdate(float deltaTime);

        // Tape deck state queries
        bool IsTapeDeckOpen() const { return _tapeDeckOpen; }
        TapeDeckState GetTapeDeckState() const { return _tapeDeckState; }

        // Force close tape deck
        void CloseTapeDeck();

        // Insert holotape (stores world ref, marks loaded, closes deck)
        bool InsertHolotape(RE::TESObjectREFR* holotapeRef);

        // Check if a world position is near the tape deck slot
        bool IsInTapeDeckSlotZone(const RE::NiPoint3& pos) const;

        // Holotape state
        bool HasHolotapeLoaded() const { return _holotapeLoaded; }
        std::uint32_t GetLoadedHolotapeFormID() const { return _loadedHolotapeFormID; }
        bool IsIntroSWFActive() const { return _introSWFActive; }
        bool IsProgramSWFActive() const { return _programSWFActive; }
        float GetFrikPipboyScale();
        void SetTapREFVisible(bool visible);
        void EjectCurrentHolotape();

        // Tape deck animation — called from end-of-update hook (after all animation/skeleton updates)
        void UpdateTapeDeckAnimation();

        // Reset all state on save/load to prevent stale transforms
        void ClearState();

        // Queue intro holotape delivery (called from Heisenberg.cpp on game load)
        void QueueIntroHolotapeDelivery();

        // Signal that this is a new game (player starts in vault, no Pipboy yet)
        void SetNewGame();

        // Check if a form ID is the Heisenberg intro holotape
        bool IsIntroHolotape(std::uint32_t formID) const;

    private:
        PipboyInteraction() = default;
        ~PipboyInteraction() = default;
        PipboyInteraction(const PipboyInteraction&) = delete;
        PipboyInteraction& operator=(const PipboyInteraction&) = delete;

        // Helpers
        RE::NiAVObject* GetPipboyArmNode();
        RE::NiPoint3    GetFingerPosition();
        RE::NiAVObject* GetCachedTapeDeckNode();
        void            InvalidateFrameCache();
        void            PlaySound(std::uint32_t formID);
        void            PlayWavSound(const char* filename, std::uint32_t fallbackFormID = 0);

        // Intro Holotape
        void InitIntroHolotape();
        void TryDeliverIntroHolotape();
        void StartIntroPlayback();
        void UpdateIntroPlayback(float deltaTime);
        void StopIntroPlayback();

        // Mesh diffuse SRV helpers (used by terminal screen redirect)
        static uintptr_t GetMeshDiffuseSRV(RE::NiAVObject* mesh);
        static void      SetMeshDiffuseSRV(RE::NiAVObject* mesh, uintptr_t srv);

        // Per-frame logic
        void OperateEjectButton(float deltaTime);
        void CheckHolotapeRemoval();   // Detect grip near open deck to physically remove holotape
        void CheckHolotapeInsertion(); // Detect held holotape near open deck to insert it
        void CheckHandPush();          // Track hand pushing deck closed (replaces slam)
        void UpdateHolotapeFingerPose(); // Extend index finger when holding holotape near Pipboy

        // Debug
        void DumpNodesContaining(RE::NiAVObject* node, const std::string& indent);

        // ── State ──
        TapeDeckState   _tapeDeckState       = TapeDeckState::Closed;
        bool            _tapeDeckOpen         = false;    // Target: true=open, false=closed
        float           _tapeDeckAnimProgress = 0.0f;     // 0.0=fully closed, 1.0=fully open
        float           _ejectCooldown        = 0.0f;
        bool            _holotapeLoaded       = false;
        std::uint32_t   _loadedHolotapeFormID = 0;
        std::uint32_t   _pendingHolotapeRefrID = 0;  // World ref to ActivateRef when deck closes (deferred playback)
        int             _logCooldown          = 0;
        bool            _dumpedNodes          = false;
        float           _buttonOriginalZ      = 0.0f;    // Captured from node on first find
        bool            _buttonOriginalZSet   = false;
        RE::NiPoint3    _prevEjectFingerPos   = {};      // Previous frame finger position for velocity calc
        bool            _prevEjectFingerValid = false;   // True after first frame of tracking
        bool            _tapeRefInitialHideDone = false;  // True once we've hidden the default tape on first update
        float           _holotapeGrabCooldown   = 0.0f;   // Cooldown to prevent instant re-grab after removal
        float           _closeAnimSpeed         = ANIM_SPEED; // Current close animation speed (variable for slam)
        float           _slamCooldown           = 0.0f;   // Prevent multiple slam detections
        float           _pushStartDistance      = 0.0f;   // Hand distance when push started (for mapping)
        float           _pushProgress           = 1.0f;   // Hand-driven progress during Pushing state (1=open, 0=closed)
        float           _frikPipboyScale        = -1.0f;  // Cached FRIK PipboyScale (-1 = not yet read)
        bool            _tapREFForceHidden      = false;  // Set by removal, cleared by insertion — overrides per-frame visibility
        bool            _meshesInitialized      = false;  // True after first rotation init (reset on load)
        bool            _holsteredWeaponForHolotape = false; // True = we holstered weapon for holotape removal, re-draw when done

        // Holotape finger pose — when holding a holotape near Pipboy, extend only the index finger
        bool            _holotapeFingerPoseActive   = false; // True when index finger is overridden to pointing
        bool            _holotapeFingerPoseIsLeft    = false; // Which hand has the override

        // Post-insertion open hand — force hand fully open until deck closes
        bool            _insertionOpenHandActive    = false;  // True = force open hand (cleared on deck close)
        bool            _insertionOpenHandIsLeft    = false;  // Which hand inserted the holotape
        bool            _deckOpenedByEject          = false;  // True = eject opened deck (removal pose), false = insertion/ceremony (open hand)

        // Track last held holotape to set insertion cooldown on new grabs
        std::uint32_t   _lastHeldHolotapeRefID      = 0;     // RefID of last detected holotape grab

        // Deferred Disable() for inserted holotape world refs — prevents crash in
        // Inventory3DManager::FinishItemLoadTask when async 3D load races with Disable.
        RE::ObjectRefHandle _deferredDisableHandle{};   // Handle to world ref pending disable
        int                 _deferredDisableFrames = 0; // Frames remaining before calling Disable()

        // Delayed holotape playback (let slam sound finish before starting audio)
        float           _pendingPlaybackDelay       = 0.0f;  // Countdown; when reaches 0, trigger playback
        std::uint32_t   _pendingPlaybackFormID      = 0;     // FormID of holotape waiting to play

        // Pending audio holotape playback (deferred until PipboyMenu is open)
        std::uint32_t   _pendingAudioFormID         = 0;     // FormID of kVoice/kScene holotape waiting for Pipboy open
        int             _pendingAudioWaitFrames     = 0;     // Frame delay after PipboyMenu opens before triggering

        // Pending program holotape playback (deferred until PipboyMenu is open)
        std::uint32_t   _pendingProgramFormID       = 0;     // FormID of kProgram holotape waiting for Pipboy open
        int             _pendingProgramWaitFrames   = 0;     // Frame delay after PipboyMenu opens before triggering
        int             _holotapePauseClearFrames   = 0;     // Countdown: keep clearing kPausesGame from holotape menus
        bool            _introSWFActive             = false; // True while intro SWF showing; audio starts on Pipboy close
        bool            _programSWFActive           = false; // True while game holotape SWF is playing on Pipboy
        bool            _introSWFMenuSeen           = false; // True once PipboyMenu was seen open (prevents false trigger)
        std::chrono::steady_clock::time_point _introSWFStartTime{};  // Real wall-clock start (matches SWF getTimer())
        int             _introSWFLastLogSec         = 0;     // Last logged second (for periodic debug logging)
        bool            _introSWFCloseStep2Done     = false; // (legacy, unused — kept for binary compat)
        bool            _introBootSoundPlayed       = false; // (legacy, unused)

        // Intro SWF sound event timeline (precomputed to match SWF animation timing)
        std::vector<std::pair<float, std::string>> _introSoundEvents;
        int             _introSoundEventIndex       = 0;     // Next event to play
        bool            _introAudioStarted          = false;  // True after intro WAV playback thread launched

        // ── Terminal-on-Pipboy redirect state ──
        bool            _pendingTerminalRedirect    = false; // True = TerminalMenu pending redirect to Pipboy (holotape)
        bool            _terminalRedirectActive     = false; // True = terminal is active with redirect
        bool            _isWorldTerminalRedirect    = false; // True = world terminal (Screen:0), false = holotape (FRIK wrist)
        bool            _terminalPatchesSuspended   = false; // True = binary patches reverted (PA/projected mode)
        int             _savedLayerLock             = 0;     // Saved render layer singleton lock value (+0x374)
        int             _savedModeLock              = 0;     // Saved render mode singleton lock value (+0x374)
        int             _savedLayerValue            = -1;    // Saved render layer singleton value (+0x36c)
        int             _savedModeValue             = -1;    // Saved render mode singleton value (+0x36c)
        bool            _radioWasEnabled            = false; // Radio was playing before Console opened
        int             _radioRestoreFrames         = 0;     // Countdown to re-enable radio after audio system settles
        bool            _consoleOpenedForTerminal   = false; // True = we opened console to suppress darkening
        int             _consoleOpenDelayFrames     = 0;     // Delay console open to let initial terminal sounds play
        bool            _terminalSoundPending       = false; // True = play boot sound after console opens
        int             _terminalSoundDelay         = 0;     // Frames to wait after console opens before playing sound
        bool            _worldTerminalChecked       = false; // Prevent re-checking same world terminal
        int             _wtWaitFrames               = 0;     // Frames waiting for InitRenderer to complete
        RE::NiAVObject* _savedHmdScreenNode         = nullptr; // Original HMD Screen node (hidden during redirect)
        RE::NiNode*     _savedHmdScreenParent       = nullptr; // Original parent of HMD Screen node
        void*           _savedRendererPtr            = nullptr; // PipboyMenu I3D renderer for cleanup
        std::uintptr_t  _savedOrigWorldRoot          = 0;      // Original worldRoot pointer to restore
        RE::NiAVObject* _terminalScreenNode          = nullptr; // World terminal Screen:0 node (for SRV swap)
        std::uintptr_t  _savedDiffuseSRV             = 0;      // Original diffuse texture SRV pointer (restored on close)

        // ── In-world terminal screen redirect state (experimental) ──
        bool            _worldScreenRedirectActive   = false;   // True = terminal UI is rendering onto world terminal mesh
        bool            _worldScreenChecked          = false;   // Prevent re-checking same terminal activation
        RE::NiAVObject* _worldScreenTerminalNode     = nullptr; // In-world terminal's screen mesh node
        std::uintptr_t  _worldScreenOrigSRV          = 0;      // Original diffuse SRV of world terminal screen (restore on close)

        // ── Intro Holotape state ──
        struct IntroLine {
            std::string filename;
            float durationSeconds;
            std::string subtitle;
        };
        std::vector<IntroLine>  _introLines;
        int                     _introCurrentLine       = -1;  // Currently-playing line index (-1 = none)
        std::chrono::steady_clock::time_point _introLineEndTime{};  // Wall-clock time when next line fires
        int                     _lastDisplayedSubLine   = -1;
        bool                    _introPlaybackActive    = false;
        std::uint32_t           _introHolotapeFormID    = 0;
        bool                    _introDeliveryQueued    = false;
        float                   _introDeliveryDelay     = 0.0f;
        bool                    _introInitDone          = false;
        bool                    _isNewGame              = false;  // True on kNewGame — waiting for vault exit
        bool                    _newGameExteriorReached = false;  // Player has reached exterior after new game


        // Per-frame cache (invalidated each frame)
        RE::NiAVObject* _cachedArmNode = nullptr;
        RE::NiAVObject* _cachedTapeDeckNode = nullptr;
        RE::NiPoint3    _cachedFingerPos{};
        bool            _frameCacheValid = false;
        bool            _fingerPosCached = false;

        // Persistent node caches (invalidated in ClearState / on skeleton change)
        RE::NiAVObject* _cachedEjectButton     = nullptr;
        RE::NiAVObject* _cachedEjectButtonMesh = nullptr;
        RE::NiAVObject* _cachedTapeDeckLid     = nullptr;
        RE::NiAVObject* _cachedTapeRef         = nullptr;
        RE::NiAVObject* _cachedTapeDeckMesh1   = nullptr;
        RE::NiAVObject* _cachedTapeDeckLidMesh1= nullptr;
        bool            _nodesCached           = false;

        // ── Constants ──
        // Tape deck mesh rotation angles (degrees)
        static constexpr float TAPE_DECK_OPEN_ANGLE  = 16.0f;   // Tray rotation degrees (TapeDeck01_mesh:1)
        static constexpr float TAPE_LID_OPEN_ANGLE   = 18.0f;   // Lid rotation degrees (TapeDeckLid_mesh:1)
        // Animation speed (progress per frame, matching FRIK)
        static constexpr float ANIM_SPEED             = 0.05f;
        // Eject button
        static constexpr float EJECT_BUTTON_RANGE     =  2.0f;   // Animate button + fast-trigger radius (FRIK light: 2.0)
        static constexpr float EJECT_BUTTON_RANGE_SLOW=  0.6f;   // Trigger radius when finger is moving slowly
        static constexpr float EJECT_SPEED_THRESHOLD  =  0.5f;   // Finger speed (units/frame) above which full range is used
        static constexpr float EJECT_TRIGGER_Z        = -0.14f;  // Z threshold to trigger press (FRIK light: -0.14)
        static constexpr float EJECT_Z_MIN            = -0.2f;   // Max button depression (FRIK light: -0.2)
        static constexpr float EJECT_COOLDOWN_TIME    =  0.5f;   // Seconds between presses
        // Holotape slot
        static constexpr float TAPE_SLOT_RADIUS       =  8.0f;
        static constexpr float TAPE_GRAB_RADIUS       = 15.0f;   // Distance from controller to tape deck tray for holotape removal
        static constexpr float TAPE_GRAB_COOLDOWN     =  1.5f;   // Seconds after removal before allowing re-grab
        static constexpr float TAPE_INSERT_RADIUS     = 5.0f;    // Distance to detect held holotape for insertion
        static constexpr float TAPE_POINTING_RANGE    = 12.0f;   // Distance at which held holotape triggers index-finger pointing toward deck
        static constexpr float PUSH_ENTER_DISTANCE    = 5.0f;    // Finger enters push zone = fully open position (wider catch zone)
        static constexpr float PUSH_EXIT_DISTANCE     = 7.0f;    // Finger exits push zone (hysteresis)
        static constexpr float PUSH_CLOSED_DISTANCE   = 1.0f;    // Finger at this distance = fully closed (don't go to 0 — hand clips through pipboy)
        static constexpr float PUSH_COMMIT_PROGRESS   = 0.35f;   // Below this progress (65% closed), auto-commit to close
        static constexpr float PUSH_SPRING_SPEED      = 0.08f;   // Speed of spring-back to open
        static constexpr float SLAM_COOLDOWN_TIME     = 0.5f;    // Seconds between slam detections
        // Intro holotape
        static constexpr float INTRO_LINE_GAP          = 0.5f;   // Seconds gap between intro lines
        static constexpr float INTRO_DELIVERY_DELAY    = 3.0f;   // Seconds after load before delivering holotape
        static constexpr float INTRO_NEWGAME_DELAY     = 120.0f; // Seconds after vault exit before delivering (new game)

        // Sound form IDs (from Creation Kit)
        static constexpr std::uint32_t SOUND_HOLOTAPE_UP     = 0x000BBFA8;  // ITMHolotapeUp - deck opening
        static constexpr std::uint32_t SOUND_HOLOTAPE_DOWN   = 0x000BBFA9;  // ITMHolotapeDown - deck closing
        static constexpr std::uint32_t SOUND_HOLOTAPE_INSERT = 0x0002287E;  // UIPipBoyHolotapeInsert - holotape insertion
    };
}
