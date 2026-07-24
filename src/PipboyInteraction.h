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

        // Holotape state
        bool HasHolotapeLoaded() const { return _holotapeLoaded; }
        std::uint32_t GetLoadedHolotapeFormID() const { return _loadedHolotapeFormID; }
        bool IsIntroSWFActive() const { return _introSWFActive; }
        bool IsProgramSWFActive() const { return _programSWFActive; }
        float GetFrikPipboyScale();
        // True if FRIK's holo Pip-Boy is enabled ([Fallout4VRBody] HoloPipBoyEnabled in
        // FRIK.ini). Holo mode requires a staged projected->wrist transition because FRIK
        // replaces the Pip-Boy root on the following frame. Cached until cell/game load.
        bool IsFrikHoloPipboyEnabled();

        // Writes FRIK.ini's HoloPipBoyEnabled directly and updates our own cached read of
        // it. FRIK has a live file-watcher on this exact file (thomasmonkman-filewatch)
        // that reloads on external changes, so this affects FRIK's live in-memory value
        // too - see BeginPipboyBootSequence's use of this for the boot-sequence's holo
        // workaround. Returns false (and changes nothing) if the file couldn't be read/written.
        bool SetFrikHoloPipboyEnabled(bool enabled);
        void SetTapREFVisible(bool visible);

        // Inventory activation of the intro tape while it is physically loaded means
        // "take it from the deck", not "play it again". Clears playback/deck state and
        // queues the inventory copy into the right hand.
        bool TakeLoadedIntroHolotapeToRightHand(std::uint32_t formID);

        // Tape deck animation — called from end-of-update hook (after all animation/skeleton updates)
        void UpdateTapeDeckAnimation();

        // Reset all state on save/load to prevent stale transforms
        void ClearState();

        // Queue intro holotape delivery (called from Heisenberg.cpp on game load)
        void QueueIntroHolotapeDelivery();

        // Signal that this is a new game (player starts in vault, no Pipboy yet)
        void SetNewGame();

        // Called from the activate hook when a terminal is being activated. If the player
        // is in projected Pipboy mode and bForceTerminalOnWrist is on, flips to wrist mode
        // BEFORE TerminalMenu opens so the terminal renders through the wrist Pipboy renderer
        // (not the projected VR overlay). Mirrors how the holotape override is applied before
        // playback opens the display. Auto-restores if no terminal opens shortly after.
        void PrepareProjectedTerminalOnWrist();

        // Vanilla Pip-Boy boot sequence (RobCo boot SWF) — VR never fires this because it is
        // triggered by an animation-graph event embedded in the flat-screen pickup animation,
        // which FRIK's controller-driven arms never play. Forces the Pip-Boy open on the wrist
        // (regardless of projected/Holo preference) on first pickup and sends the same native
        // event the vanilla animation would have sent.
        void BeginPipboyBootSequence();

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
        void BeginIntroProgramPlayback(std::uint32_t formID);
        void QueueProgramHolotapePlayback(std::uint32_t formID);
        void UpdateIntroHoloOverride();

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
        int             _logCooldown          = 0;
        bool            _dumpedNodes          = false;
        float           _buttonOriginalZ      = 0.0f;    // Captured from node on first find
        bool            _buttonOriginalZSet   = false;
        bool            _ejectContactLatched  = false;   // Sticky contact; resets outside release radius
        float           _holotapeGrabCooldown   = 0.0f;   // Insertion-only grace; prevents a removed/just-spawned tape re-entering the deck
        float           _holotapeRemovalCooldown = 0.0f;  // Separate short post-insert guard; ejecting never has to wait on insertion grace
        float           _closeAnimSpeed         = ANIM_SPEED; // Current close animation speed (variable for slam)
        float           _slamCooldown           = 0.0f;   // Prevent multiple slam detections
        float           _pushProgress           = 1.0f;   // Hand-driven progress during Pushing state (1=open, 0=closed)
        float           _frikPipboyScale        = -1.0f;  // Cached FRIK PipboyScale (-1 = not yet read)
        int             _frikHoloPipboy         = -1;     // Cached FRIK HoloPipBoyEnabled (-1=unread, 0=false, 1=true)
        bool            _tapREFForceHidden      = false;  // Set by removal, cleared by insertion — overrides per-frame visibility
        bool            _meshesInitialized      = false;  // True after first rotation init (reset on load)

        // Holotape finger pose — when holding a holotape near Pipboy, extend only the index finger
        bool            _holotapeFingerPoseActive   = false; // True when index finger is overridden to pointing
        bool            _holotapeFingerPoseIsLeft    = false; // Which hand has the override

        // Post-insertion open hand — force hand fully open until deck closes
        bool            _insertionOpenHandActive    = false;  // True = force open hand (cleared on deck close)
        bool            _insertionOpenHandIsLeft    = false;  // Which hand inserted the holotape
        bool            _deckOpenedByEject          = false;  // True = eject opened deck (removal pose), false = insertion/ceremony (open hand)

        // Track last held holotape to set insertion cooldown on new grabs
        std::uint32_t   _lastHeldHolotapeRefID      = 0;     // RefID of last detected holotape grab

        // One shared visible-fingertip motion sample for all Pip-Boy contacts this frame.
        // The previous point makes thin button/deck meshes sweep-safe at normal VR speeds.
        RE::NiPoint3    _interactionFingerPos{};
        RE::NiPoint3    _previousInteractionFingerPos{};
        bool            _interactionFingerValid = false;
        bool            _previousInteractionFingerValid = false;

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

        // Intro SWF sound event timeline (precomputed to match SWF animation timing)
        std::vector<std::pair<float, std::string>> _introSoundEvents;
        int             _introSoundEventIndex       = 0;     // Next event to play
        bool            _introAudioStarted          = false;  // True after intro WAV playback thread launched

        // ── Terminal-on-Pipboy redirect state ──
        bool            _pendingTerminalRedirect    = false; // True = TerminalMenu pending redirect to Pipboy (holotape)
        bool            _terminalRedirectActive     = false; // True = terminal is active with redirect
        bool            _isWorldTerminalRedirect    = false; // True = world terminal (Screen:0), false = holotape (FRIK wrist)
        bool            _terminalPatchesSuspended   = false; // True = binary patches reverted (PA/projected mode)
        bool            _hasReachedExterior         = false; // One-way latch: player has LEFT Vault 111 (exterior cell). Terminal redirect gated on this.
        bool            _pipboyOpenedSinceAcquire   = false; // One-way latch: player opened the Pip-Boy at least once after acquiring it (vanilla bootup). Intro ceremony waits for this + Pip-Boy closed.

        // ── Vanilla Pip-Boy boot sequence (auto-fired on first pickup) ──
        bool            _bootSequenceFired          = false; // One-way latch: never re-send the native event this game session
        bool            _bootWristOverrideOwned     = false; // True if the boot sequence itself started the projected->wrist override
        // Temporary FRIK HoloPipBoyEnabled override so the boot sequence renders on the
        // flat wrist model even when the player's real preference is Holo. Written to
        // FRIK.ini directly, before FRIK's own Pip-Boy model is ever constructed this
        // session (sidesteps needing FRIK's un-exported swapModel() sequence).
        bool            _bootHoloIniHandled          = false; // One-way latch: decided whether to override, for this pickup
        bool            _bootHoloIniOverrideActive   = false; // True from a successful false-write until restored back to true
        float           _bootHoloIniWaitSeconds      = 0.0f;  // Countdown after writing false, before forcing the Pip-Boy open (lets FRIK's debounced file-watcher reload)
        float           _bootHoloIniRestoreSeconds   = -1.0f; // Countdown after firing the boot event, before restoring true (-1 = not started)
        // Jul 24 boot redesign: completion is detected from the game's own
        // PipboyOpeningSequenceMenu open->closed transition (the boot SWF drives its
        // own teardown); the Holo/projected restores hang off that, not fixed timers.
        bool            _bootMenuSeen                = false; // PipboyOpeningSequenceMenu observed open since firing
        bool            _bootMenuFinished            = false; // Completion (or never-appeared fallback) handled — restores dispatched
        float           _bootMenuAppearWaitSeconds   = 0.0f;  // Ceiling for the menu to appear after firing before falling back
        bool            _bootScreenActivated         = false; // Wrist screen lit for the boot menu (ActivatePipboyScreen) — must be deactivated on completion/interruption
        bool            _bootPipboyMenuBlanked       = false; // PipboyMenu's Scaleform root hidden while the boot SWF owns the screen (restored on completion)
        float           _bootContentOpenSeconds      = 0.0f;  // Post-boot countdown before opening PipboyMenu so normal content shows (vanilla shows STATS after boot)
        bool            _bootSawPipboyAbsent         = false; // Armed only after observing the Pip-Boy NOT owned this session — boot fires on the 0->1 acquisition, never on loads
        // Intro ceremony always-on-wrist rule: FRIK Holo temporarily forced off (same
        // FRIK.ini override the boot sequence uses) while the intro plays on the wrist;
        // projected/HMD modes ride the existing projected->wrist override.
        bool            _introHoloIniOverrideActive  = false; // True from a successful HoloPipBoyEnabled=false write until restored
        int             _introHoloWaitFrames         = 0;     // Settle frames (FRIK watcher debounce + root reinstall) before the deferred playback starts
        std::uint32_t   _introHoloWaitFormID         = 0;     // Intro tape whose playback is deferred behind the settle wait
        bool            _introHoloSawPipOpen         = false; // Display-ended tracking for the Holo restore (open observed, then closed)
        int             _savedLayerLock             = 0;     // Saved render layer singleton lock value (+0x374)
        int             _savedModeLock              = 0;     // Saved render mode singleton lock value (+0x374)
        int             _savedLayerValue            = -1;    // Saved render layer singleton value (+0x36c)
        int             _savedModeValue             = -1;    // Saved render mode singleton value (+0x36c)
        bool            _radioWasEnabled            = false; // Radio was playing before Console opened
        int             _radioRestoreFrames         = 0;     // Countdown to re-enable radio after audio system settles
        bool            _consoleOpenedForTerminal   = false; // True = we opened console to suppress darkening
        bool            _worldTerminalChecked       = false; // Prevent re-checking same world terminal
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
        bool                    _introPostLoadBaselinePending = false;  // Classify already-owned Pip-Boy after LoadingMenu closes
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
        static constexpr float EJECT_FINGER_RADIUS    =  1.5f;   // Fingertip flesh around the real EjectButton mesh
        static constexpr float CONTACT_RELEASE_PAD    =  2.0f;   // Mesh-contact hysteresis
        static constexpr float EJECT_Z_MIN            = -0.2f;   // Max button depression (FRIK light: -0.2)
        static constexpr float EJECT_SKIN_RADIUS      =  0.45f;  // Visible fingertip flesh around the tracked bone point — the button starts moving exactly when the SKIN touches it, not when the padded contact bubble does
        static constexpr float EJECT_COOLDOWN_TIME    =  0.5f;   // Seconds between presses
        // Holotape slot
        static constexpr float TAPE_GRAB_RADIUS       = 15.0f;   // Distance from controller to tape deck tray for holotape removal
        static constexpr float TAPE_GRAB_COOLDOWN     =  1.5f;   // Seconds after removal before allowing that hand-held tape to reinsert
        static constexpr float TAPE_REMOVAL_CONTACT_RADIUS = 5.0f; // Grip fingertip padding around the visible TapREF mesh
        static constexpr float TAPE_INSERT_RADIUS     = 5.0f;    // Distance to detect held holotape for insertion
        static constexpr float TAPE_POINTING_RANGE    = 12.0f;   // Distance at which held holotape triggers index-finger pointing toward deck
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
