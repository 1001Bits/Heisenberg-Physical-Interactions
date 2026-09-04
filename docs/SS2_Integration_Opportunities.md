# Sim Settlements 2 x Heisenberg — integration opportunities

Analysis of SS2 **v3.6.1** (all 1869 compiled Papyrus scripts unpacked from `SS2 - Main.ba2`)
against Heisenberg's physical-interaction capabilities. 2026-08-01.

**Read this first:** everything marked *confirmed-signature* is a function name, type and
parameter list read out of compiled `.pex` bytecode. Compiled Papyrus gives you names and call
lists, **not control flow and not property VALUES**. So "this function exists and takes these
arguments" is proven; "calling it does X" is inference. Numeric constants
(`iPlotMenuID_Main`, `iASAMType_Mk2`, ...) live in the ESM, not the scripts, and must be read
at runtime.

---

## 1. The Papyrus VM bridge — status: EXISTS, COMPILED, AND LATENTLY FATAL ON F4VR

This gates most of the list below, so it comes first.

**A complete OG/VR-aware dispatch helper is already vendored** at
`external/ROCK/src/compat/RE/Bethesda/SendPapyrusEvent.h` — `DispatchStaticCall`,
`DispatchMethodCall`, `SendPapyrusExternalEvent`, with an explicit OG-vs-NG split (NG changed
the `DispatchMethodCall` signature; OG/VR reaches the function through vtable entries 44 and
46). It is **compiled into the shipping DLL**: `rock_support/Fo4VrRuntime.cpp` calls
`Papyrus::detail::DispatchStaticCall` for `Debug.Notification`.

**It has never actually run.** No log in any session contains `Show notification:` or
`Papyrus rejected`. That is fortunate, because:

### The blocker (verified)

| What | Resolves through | On F4VR |
|---|---|---|
| `RE::GameVM::GetSingleton()` | `REL::RelocationID(996227, 2689134)` | **no VR mapping — throws** |
| `VTABLE::BSScript__Internal__VirtualMachine` | `REL::ID(614600)` (+3 more) | **no VR mapping — throws** |

Both are SE/AE address-library IDs. This is the same failure that crashed every launch via the
`TESHitEvent` sink (`RelocationID(989868)`) — see `reference_commonlib_relocationid_not_vr`.
It does not crash at load only because both sit inside function-local statics, so they resolve
on **first call**. The first Papyrus dispatch would throw
`Failed to find the id within the address library: 996227`.

### The fix (both parts verified against the F4VR binary)

The VM is fully present in Fallout4VR.exe — Ghidra shows the complete `GameVM::*` symbol set
(`GameVM::Update` @ `0x13f6d60`, `GameVM::GameVM` @ `0x13f5970`, etc.) plus the RTTI strings
`.?AVVirtualMachine@Internal@BSScript@@` and `.?AVIVirtualMachine@BSScript@@`.

**1. Singleton pointer — raw VR offset `0x5935428`.** Taken from the call site of
`GameVM::Update` inside `Main::OnIdle` (`FUN_140d83de0`):

```
140d842d0  MOV  RCX, qword ptr [0x145935428]   ; <- the GameVM* singleton
140d842d7  XORPS XMM1,XMM1
140d842da  CALL 0x1413f6d60                    ; GameVM::Update(this, float)
```

so, following this project's existing convention for VR-only addresses:

```cpp
static REL::Relocation<RE::GameVM**> gameVM{ REL::Offset(0x5935428) };
auto* vm = (*gameVM) ? (*gameVM)->GetVM().get() : nullptr;
```

**2. Vtable — do not use the address library at all.** `GetVtableFunctionAddr()` currently does
`VTABLE::BSScript__Internal__VirtualMachine[0].address()`. It does not need to: once you hold a
live `IVirtualMachine*`, its vtable is at `*(void***)vm`, so entries 44/46 can be read straight
off the instance. That removes the second ID dependency entirely and is version-robust.

**Threading:** dispatch from the main game thread only. Heisenberg's gestures fire from hooks
that can run on render/physics threads; route SS2 calls through the existing game-thread defer
used elsewhere in the codebase.

**Cost:** roughly a day for the bridge + a smoke test. Everything in §3 that needs Papyrus is
cheap once it exists.

---

## 2. What needs no bridge at all

Two of the highest-value items are pure native calls:

- **ASAM sensor touch-to-activate.** `ASAMSensor.OnInit` calls `BlockActivation(True, False)` —
  blocks the *default* action but still fires `OnActivate`. So a native
  `ObjectReference::Activate(player)` from Heisenberg's existing touch-activate path is enough.
  `OnActivate` checks `akActivatedBy == Game.GetPlayer()`, resolves the plot, then runs
  `PrimaryActivation()`, which plays `Reset`/`TurnOff` animations and `ASAMConfirmationSFX` —
  authored feedback the physical press inherits for free.
- **Container inserts** (brewery boiler, and anything else with a real container ref):
  `TESObjectREFR::AddObjectToContainer`, already at `REL::Offset(0x3e9e90)`. SS2's
  `BreweryBoiler` uses `AddInventoryEventFilter` + `OnItemAdded`, so a physical insert is seen
  exactly like a menu transfer.

---

## 3. Ranked opportunities

Ranked by player-visible value ÷ implementation cost.

| # | Opportunity | Needs VM bridge | Effort |
|---|---|---|---|
| 1 | Language-independent consumable identity (prerequisite) | property read | small |
| 2 | Touch an ASAM sensor to open the plot menu | **no** | small |
| 3 | Inject a disease cure into a settler by touching them | yes | medium |
| 4 | Press a stimpak into a downed settler | yes | medium |
| 5 | Touch a settler to read their vitals | no (AV reads) | small |
| 6 | Repair a smashed ASAM by pressing a part into it | yes | small |
| 7 | Slot a Mark II ASAM into the dome by hand | yes | medium |
| 8 | Pour mash/hops into the brewery boiler | partly | medium |
| 9 | Physical "alternate activation" layer (~20 interactions) | yes | medium |
| 10 | Place foundation / power pole by hand | yes | large |

### 1. Fix consumable identity — do this first regardless
Heisenberg's injection whitelist matches **English display names**
(`stimpak`/`radaway`/`med-x`/...). A Spanish Stimpak is `Estimulante`, so self-injection was
**silently broken in every localised install**, and every mod-added medical item missed too.
Partially fixed already (now uses `SmartGrabHandler::CategorizeItem`, which reads magic-effect
archetypes). **Remaining risk:** SS2's cure runs through a *script*-archetype effect, so
`kCureDisease` may not fire on it. The robust identity is to read SS2's own forms:
`NPC_RPGManager.DiseaseCureForm` (`PROPERTY potion`), `NPC_RPGManager.Stimpak` (`PROPERTY form`),
and the `potion` field of each `DefaultDiseases[]` entry. Property reads are cheaper and safer
than calls. Cache once per game load.

### 2. ASAM sensor touch — the gateway to all of SS2
The single most repeated interaction in the mod. Aiming the VR crosshair at a small dome is
fiddly; pressing it with your hand is not. Identify candidates via the linked-ref keyword
`TESDataHandler::LookupForm(0x014AFD, "SS2.esm")` (a literal integer inside the compiled
script, byte-verified at offset 1671), or enumerate `PlotManager.CurrentSettlementASAMSensors`
— every loaded ASAM `AddRef`s itself in `ASAMSensor.OnLoad`.
Plot resolution: `UtilityFunctions.GetLinkedPlot(objectreference, keyword) -> SimPlot`
(global/static, no quest handle needed).

### 3. Disease cure injection — the live case
`NPC_RPGManager.TryToUseCureItemOnActor(actor akActorRef, Bool abFromPlayerInventory)` —
pass `true`. This is exactly what SS2's own alternate-activation perk calls, so item
consumption, medical costs and eligibility checks all stay SS2's.
**Important:** SS2 does **not** listen for `OnItemAdded` anywhere for cures. `NPC_RPGManager`
registers an inventory filter on the **PlayerRef** and handles `OnItemRemoved`, reading
`akDestContainer` — the signature of a *trade*. The currently shipped implementation synthesises
exactly that event pair (add to player, then `RemoveItem` with `a_otherContainer = settler`,
reason `kStoreTeammate`), which should work; `TryToUseCureItemOnActor` is the cleaner
replacement once the bridge exists.
Related: `TryToTreatPatient(actor, Bool, Bool, Bool abUseMenu)` and `TryToTreatPatientV2(...)`
— note `abUseMenu`, so SS2 already supports a **menuless** treatment path by design.

### 4. Stimpak a downed settler
`NPC_RPGManager.TryToApplyStimpak(actor akActorRef)`. Which settler is downed:
`NPC_RPGManager.InjuryAV`. Re-entrancy guard `PROPERTY Bool bIsStimpaking` exists. Feedback:
`UseStimpakSound`.

### 5. Touch a settler to read vitals
`NPC_RPGManager.ShowViewNPCBook(actor akActorRef)` — **one** parameter, the actor.
For a fully native readout with **no VM call**, read the actor values directly once the AV forms
are resolved: `SicknessAV`, `InjuryAV`, `DaysWithCurrentDiseasesAV`, `ActivePatientCapacityAV`,
`PollutionVsHygeineAV`.

### 6–10
- **Repair ASAM:** `PlotManager.ShowRepairASAMSensorMenu(simplot) -> Bool` (keeps SS2's confirm
  + cost), or bypass the box: consume `PlotManager.ASAMSensorComponent`, then
  `SimPlot.GetASAMSensor().ClearDestruction()`.
- **Mark II ASAM:** `SimPlot.ChangeASAM(Int aiType)` with `PlotManager.iASAMType_Mk2`; gate on
  `PlotManager.Unlock_ASAMMarkII` and `SimPlot.bModelSupportsMarkIIASAM`.
- **Brewery:** native `AddObjectToContainer` onto the boiler's container ref, then
  `BreweryManager.StartBrew(simplot, objectreference) -> Bool`.
- **Alternate activation layer:** do **not** try to fire the perk entry points (perk records are
  in the ESM, not cleanly reachable, and their VR binding is invisible in Papyrus). Call the
  destinations directly, classifying the touched ref with `GetLinkedPlot`.
- **Foundation / power pole:** `SimPlot.AttachFoundation(foundation, Bool abManual)` and
  `AttachPowerPole(powerpole, Bool abManual)`.

---

## 4. Integration surface (the durable asset)

- **Form resolution:** `TESDataHandler::LookupForm(localFormID, "SS2.esm")` — **never** a
  runtime FormID. SS2 does the same internally via `Game.GetFormFromFile` +
  `UtilityFunctions.GetPluginName()`.
- **Universal target resolver:** `UtilityFunctions.GetLinkedPlot(objectreference, keyword) -> SimPlot`.
- **Medical entry points** (`SimSettlementsV2:Quests:NPC_RPGManager`, extends
  `WorkshopFramework:Library:SlaveQuest`): `TryToUseCureItemOnActor`, `TryToApplyStimpak`,
  `TryToTreatPatient`, `TryToTreatPatientV2`, `ShowViewNPCBook`, `CanPlayerTreatDisease(s)`.
- **Plot entry points** (`PlotManager`): `ShowPlotMenu(simplot, Int)`, `ShowASAMSensor`,
  `ShowBuildingPlanSelect`, `ShowFoundationSelect`, `ShowRepairASAMSensorMenu`.
- **Plot mutators** (`SimPlot`, extends `WorkshopObjectScript`): `ChangeASAM`,
  `AttachFoundation`, `AttachPowerPole`, `TryToAssignAct...`.
- **Custom events to subscribe to:** `NPC_RPGManager_SettlerInjured`,
  `_SettlerRecoveredFromInjury`, `_SettlerCuredOfDisease`; `PlotManager_PlotLevelChanged`,
  `_PlotReset`; `CityPlanManager_CityPlanLayoutBuilt`.
- **Ref collections** (cheap enumeration, no FormID whitelists):
  `PlotManager.CurrentSettlementASAMSensors`, `NPC_RPGManager.MedicalTerminalCollection`.
- **Menu-availability globals** (decide which physical affordances to light up):
  `MenuControl_StartConstruction`, `_CancelConstruction`, `_PayOperatingCosts`,
  `_UpgradeAvailable`.
- **HUD:** `HUDManager.PlayerHoveredOverWorkshopObject(objectreference) -> Bool`,
  `BuildInfoBoxMessage(objectreference, Int)`.

---

## 5. Risks

- **Numeric property values are not in the scripts.** Every `iPlotMenuID_*`, `iASAMType_*`
  constant must be read at runtime from the quest object, not guessed.
- **Branch logic is invisible.** Compiled `.pex` gives call lists, not control flow. "Function X
  calls Y" proves reachability, never that it happens on your path.
- **SS2 version churn.** SS2 versions functions aggressively (`TryToTreatPatient` vs `V2`,
  `CureAllDiseases` vs `V2`). Resolve by name at runtime and fail soft if absent.
- **Load order / ESL.** SS2 addon packs are frequently ESL-flagged, which changes index layout.
  Only `LookupForm(localID, "SS2.esm")` is safe.
- **Localisation.** Never match display names. This is the bug that started the investigation.
- **Menu conflicts.** `ShowPlotMenu`, `ShowBuildingPlanSelect`, `ShowRepairASAMSensorMenu` open
  MessageBox/BarterMenu — screen-space menus that will fight MenuChecker and the VR input
  layers. Prefer the menuless variants (`abUseMenu=false`, direct mutators).
- **Save safety is good:** Heisenberg adds no Papyrus scripts, so nothing bakes into the save
  and uninstalling leaves no orphaned instances.
- **F4SE DLLs load only at launch** — every SS2 integration test needs a full relaunch.

---

## 6. Do NOT

- **Do not** hardcode FormID `1104B384` (or any runtime FormID) — its high byte is one user's
  load order.
- **Do not** match items by display name, ever, not even as a fallback.
- **Do not** call `CureAllDiseases` / `CureAllSettlers` from a gesture — they bypass item
  consumption, cost processing and eligibility, turning a touch into a cheat.
- **Do not** try to fire SS2's alternate-activation **perk** entry points; call the destinations.
- **Do not** implement physical pick-up-and-carry of whole plots.
  `SimPlot.OnWorkshopObjectGrabbed` only fires through the engine's workshop-grab path; a raw
  Havok grab bypasses it.
- **Do not** ship a recompiled SS2 `.pex` to change behaviour — breaks on every SS2 update and
  silently breaks addon packs compiled against the original.
- **Do not** reimplement SS2's cost/eligibility/skill-check logic on the Heisenberg side.

---

## 7. Tooling kept

- `.pex` string dumper + full signature dump of all 1869 scripts (scratchpad).
- BA2 (BTDX/GNRL) reader: header + 36-byte file records + trailing name table; zlib per file.
