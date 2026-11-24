# Movement Full-Port Plan — cmangos movement + travelnodes into mod-playerbots (NewRpg)

Status: **DRAFT for owner review — no porting starts until approved.**
Evidence base: seven inventory reports in this directory (`inventory-*.md`), produced by
reading all three codebases function-by-function. Every port unit below cites them; the
appendices carry the full per-function deltas.

| Alias | Tree | Role |
|---|---|---|
| **OG** | `C:/Users/Admin/git/scratch/cmangos-playerbots/playerbot` (+ `mangos-classic` core `PathFinder`) | **Intent authority** — what the system is supposed to do |
| **VEN** | `modules/mod-cmangosbots/src/playerbot` (+ its AC core patches, `botcompat.h` shim) | **AC-adaptation authority** — its `mod-cmangosbots:` **marked** changes are sanctioned translations; **unmarked** deviations lose to OG |
| **TGT** | `modules/mod-playerbots` @ `feature/new_rpg_and_nav_5_bash` | The codebase being ported into |

---

## 1. Goal, scope, non-goals

**Goal.** The complete movement + travelnode mechanics beneath the NewRpg strategy layer
become **100% mechanically equal** to the references. NewRpg keeps deciding **WHERE**
(destinations, quest flow, statuses); the ported stack decides **HOW** (pathing, dispatch,
node routing, approach). Latent bugs found *in* the references are fixed in the ported code
with a marked comment — never silently.

**In scope (owner directive):**
1. Inner-zone A→B ground movement (MoveTo2 pipeline, TravelPath walker, WorldPosition path helpers).
2. Moving to targets (mob/NPC/GO approach; the NewRpg adapter seam).
3. Questing movement (POI travel, gather/kill approach, turn-in approach).
4. Travelnodes for **zone-to-zone** travel (incl. area-trigger/teleport/flightpath legs,
   PortalNode seeding) and **intra-zone guidance** through difficult terrain (caves, ramps, ridges).

**Non-goals / deferred (each gets a stub-free boundary — the feature is absent, not half-present):**
- Boats/zeppelins moving-transport pathing — AC lacks GO navmesh (`transport-navmesh-gap` memory); blocked on `getObjectHitPos` core export (inventory-worldposition §7).
- Vehicle movement / spell-click boarding.
- FlyDirect / free flight — bots are ground travelers here; OG FlyDirect stays unported (documented, not stubbed into the active path).
- Battleground movement tactics.
- Deep-water swim polish beyond what the walker needs (`setAtWaterSurface` on legs IS in scope; it is already used by getRoute legs).
- Purpose-travel destinations (vendor/repair/AH/mail/bank/trainer cross-zone errands — OG's Request*TravelTarget purpose ladder). NewRpg's WanderNpc covers local interaction; cross-zone errands are a separate feature decision (E14.1).
- Group travel propagation (leader→follower target copy). NewRpg is wired solo-only (AiFactory); porting FreeMove leashes keeps grouped bots sane, but shared destinations are deferred (E14.9).

---

## 2. Authority & verification rules

Per function: (1) read OG; (2) read VEN and classify each delta *marked-adaptation* vs
*unmarked-deviation*; (3) port OG intent through VEN's marked adaptations; (4) line-diff the
ported function against BOTH references and record the diff verdict in the unit's commit
message; (5) compile probe; (6) behavioral checklist from the unit table.

- **No stubs in active paths.** A dependency is ported with its consumer or the consumer is out of scope entirely.
- **One unit per commit**, build-green per phase.
- **In-world regression suite** (owner-run, phase boundaries only):
  R1 Shadowthread Cave egg (intra-zone node guidance) · R2 "A Good Friend" ledge (steep edge)
  · R3 Dolanaar delivery (long same-map route) · R4 Hyacinth mushrooms (gather GO approach)
  · R5 Grellkin/Fel Moss (hunt + drop-source kills) · R6 Ayanna wander-npc (indoor NPC from range)
  · R7 "A Friend in Need" turn-in (efficient route choice) · R8 Etched Sigil treehouse (elevated ender)
  · R9 hostile-camp traversal (ClipPath enemy clip, post-Phase 3).
- Debug whisper system (`EmitDebugMove`, ledger #11/#12/#29) is retained everywhere and extended to ported functions.

---

## 3. Headline adjudication — dispatch architecture

**Finding (ledger #0, scope A):** OG dispatches ONE bounded hop per action
(`TravelPath::getNextPoint ≤ maxDist(=sightDistance)` + `WaitForReach(hop)`); TGT dispatches
the whole resolved spline via `MoveSplinePath`. Four branch commits patched around the
difference.

**Decision:** Keep **`MoveSplinePath` as the dispatch primitive** — it is VEN's *marked*
adaptation (`usePath` false→true, the documented frozen-bot fix; AC's `MovePoint(back,
generatePath)` re-ran PathGenerator and froze bots). Restore **OG's pacing semantics around
it**: the dispatched window is bounded by a **correct ClipPath** (reactDistance window from
path start, enemy/hazard clips) and committed via **faithful WaitForReach** — i.e., OG's
"bounded commitment, frequent re-evaluation" behavior with AC's dispatch mechanism.
Additionally the **SHORT/BuildShortcut acceptance hole** is closed (scope C §7): reject
`PATHFIND_SHORT`/`NOT_USING_PATH` products in `getPathStepFrom`, restoring OG's effective
guarantees that no geometry-ignoring leg ever enters a chain.

**Consequences for ledger items:** #4a bounded raw hop → FOLD (extra guard on the rare raw
branch); #5a/#14 density guard → FOLD (cheap invariant); #3 WaitForReach form → absorbed by
faithful port; #9 50y node threshold → FOLD as config key (below); #25 10y probe accept →
KEEP while per-tick resolution stands (revisit only if the whole OG action loop ever returns).

---

## 4. Disposition ledger (from `inventory-target-divergence-ledger.md`, adopted verbatim)

**KEEP (11)** — the steep-mmaps extension family + diagnostics; these exist because TGT runs
steep-tagged mmaps the references never had: #6 unstick-step, #15 soft-steep fallback +
`BotSteepTravelCost`, #24a/b/d trust-the-nodes fallback + flat-only probe + unstick LOS
guard, #25 10y accept, #9 threshold decoupling (as config), #4b null-mover local link repair
(race-free improvement over OG's shared-graph mutation), #19 closest-attacker + health-flee,
debug system #11/#12/#29.

**FOLD (10)** — correct fixes that survive *inside* the ported code: #1 quest-status guards,
#5a+#14 density guard, #5c repair tolerance, #17 foePower accumulate, #18 flee activation,
#23 approach-ring inset, **#26a ClipPath iterator-before-cut fix (the canonical FOLD — do NOT
copy the references' broken order)**, #26b probe-beats-node guard, #26c LOS-preference
passes, #28a no-progress-empty, #28b unstick 2y minimum.

**REPLACE-BY-PORT (9)** — inventions superseded by faithful mechanisms: #2 gather-yield &
#16 quest-mob gate exemption (→ OG ungated relevance; keep until Phase 5 lands), #3
WaitForReach, #4a raw-hop, #5b quest-first pick (→ purpose ordering; NewRpg keeps
`autoDoQuests` behavior — document that it bypasses `RpgStatusProbWeight`), #7 turn-in Z (→
spawn-based destinations; FOLD while POI centroids remain), #8 undirected reachability
(KEEP-as-approximation, documented false-positive tradeoff vs OG lazy directed closure), #13
corridor reuse (absorbed), #20 leash (→ full FreeMoveValues port), #21/#27/#28c
hunt/POI-rotation/unreachable-memo (→ retry/expiry lifecycle absorbed into the NewRpg seam,
Phase 5).

---

## 5. Port phases

### Phase 0 — Correctness pre-fixes (small, independent, ship first)

| # | Unit | Evidence | Action |
|---|---|---|---|
| 0.1 | **GO GuidPosition ctor bug** — `ObjectGuid(HighGuid::GameObject, goData.id)` puts the ENTRY in the COUNTER field → entry reads 0, DB-guid lookups resolve the wrong object | worldposition §8 (**CONFIRMED BUG**, TravelMgr.cpp:1187) | `ObjectGuid(HighGuid::GameObject, goData.id, goData.spawnId)`; regression: GO destinations resolve (R4) |
| 0.2 | **SHORT/shortcut acceptance hole** — budget-saturated legs splice a 2-point straight line through terrain | worldposition §7 (both AC ports affected) | `getPathStepFrom` accept rule: `(type & (NORMAL\|INCOMPLETE)) && !(type & (NOT_USING_PATH\|SHORT))`; evaluate findPath half-budget as follow-up |
| 0.3 | **Area costs 12/13 + water=10** missing from the travel chain | ledger #13 note, worldposition §7 | `getPathFromPath`/`getPathStepFrom`: `SetNavTerrainCost(NAV_WATER,10)` + costs 12=5, 13=20 (ids identical in all trees) |
| 0.4 | `NewRpgInfo::ChangeToOutdoorPvp` misses `Reset()` → stale travelPlan | consumers E15 | add Reset() |
| 0.5 | Fragile `.at()` on quest status map in `ChooseTargetActions.cpp:129` | ledger #16 note | harden to `.find()` |
| 0.6 | `generateTaxiPaths` endpoint fix-ups missing (flight legs start/stop mid-air) | travelpath-nodes | port OG prepend/append; KEEP TGT walk-path-preserve guard |

### Phase 1 — Values & types closure (the dependency floor)

| # | Unit | OG / VEN | TGT state | Notes |
|---|---|---|---|---|
| 1.1 | **LastMovement full semantics**: `Set` variants, `lastMoveShort`, `moveEvent` field, priority interplay; **MoveTo2 must arm the priority system** (today `IsWaitingForLastMove` never sees MoveTo2 movement) | values §1; scope A MoveTo2 notes | DIVERGED / field absent | Port field + arm `lastMove.Set(...)` at dispatch; keeps TGT `MovementPriority` (target-native, KEEP per E12) |
| 1.2 | **FreeMoveValues family** — `FreeMoveCenterValue`/`FreeMoveRangeValue`/`CanFreeMoveValue::{CanFreeMove,CanFreeMoveTo,CanFreeTarget,CanFreeAttack}` registered values | values §2 (ABSENT), consumers E10 | ABSENT (helper-subset only, ledger #20) | Full value triple + ValueContext registration; grind call sites switch to it; movers gain `CanFreeMoveTo` gates |
| 1.3 | **Hazards subsystem** — `HazardPosition`, `HazardsValue`, publishers | values §7 (ABSENT) | ABSENT | Blocks 3.1 ClipPath scans + 4.6 |
| 1.4 | **"possible attack targets"** (`PossibleAttackTargetsValue` + `RemoveNonThreating`) | values §6 | ABSENT (raw "attackers" only) | Blocks 3.1; keep TGT closest-attacker sort in grind (#19 KEEP) |
| 1.5 | **StuckValues** ("time since last change" / "distance moved since" MemoryValue) | gapcheck §M/N, consumers E4 | ABSENT | Powers Phase 5 stall bypasses (10s/60s) |
| 1.6 | Config keys block: `wanderMinDistance`, `proximityDistance`, `maxFreeMoveDistance`, `freeMoveDelay`, `raidFollowDistance`, gathering-distance family + `TravelNodeThreshold` (formalizing #9) | gapcheck §W | MISSING 8 keys | Defaults from OG; conf.dist documented |
| 1.7 | PositionValue/Formations/Stances **verification pass** (TGT has lineage versions) | values §3–5 | present, verify | diff-only unit; fixes if drift found |

### Phase 2 — WorldPosition / path-helper parity

| # | Unit | Evidence | Action |
|---|---|---|---|
| 2.1 | `getPathStepFrom`/`getPathFromPath` full parity: one-PathFinder chain (already restored, #13), acceptance rule (0.2), per-call area costs (0.3), underwater extension verify, **keep `allowSteepFallback` (KEEP #15/#24b)** | worldposition §§1–7 | port/verify against OG WorldPosition.cpp:1070-1110 |
| 2.2 | Per-step grid/tile availability: OG preloads tiles across the leg; TGT ensures endpoints only | worldposition dep §5 | port per-step segment grid ensuring (bounded), or document accepted truncation — **decide during unit, default: port** |
| 2.3 | `ClosestCorrectPoint` / `setAtWaterSurface` / `currentHeight` verify + `GetReachableRandomPointOnGround` real implementation | worldposition; memory `closest-correct-point` | port `currentHeight`; finish `GetReachableRandomPointOnGround` |
| 2.4 | `mapTransfer` distance machinery decision (OG approximation vs TGT linear) — hot path for every cross-map `distance()`/`fDist` | worldposition dep §3; travelpath calcMapOffset (TGT `i++` fix KEEP) | verify TGT semantics; keep TGT fix |
| 2.5 | **Mob-avoid nav-areas** (`MarkNavArea`+`SetMobAvoidArea`+`SetAvoidAreaAction`, areas 12/13) | worldposition §7 (VEN core patch), gapcheck §V | **OPEN QUESTION Q3 (core change)** — costs (0.3) are module-side; poly *marking* needs a core API like VEN's. Ship costs now; marking awaits owner call |
| 2.6 | `WorldSquare` spatial index | gapcheck §V | DEFERRED with purpose-travel (its consumer); note in non-goals |

### Phase 3 — TravelPath walker + TravelNode graph parity

| # | Unit | Evidence | Action |
|---|---|---|---|
| 3.1 | **ClipPath complete**: enemy scan + hazard scan (unblocked by 1.3/1.4) + reactDistance window + walkability/discontinuity clips, with **#26a iterator fix (FOLD)** | travelpath; scope A | the flagship unit; regression R9 |
| 3.2 | `getNextPoint`/`shouldMoveToNextPoint`/`cutTo`/`makeShortCut` byte-parity pass; **Q2: OG coin-flip `urand(0,1)` in makeShortCut** | travelpath; ledger #0 | port faithfully incl. coin-flip unless owner vetoes (Q2) |
| 3.3 | **GetNodeRoute restorations**: PortalNode seeding (hearthstone/teleport spells; fix OG's PortalNode leak on failed combos), gold-aware costs via "free money for", **preserving TGT lazy-deletion heap + 3000 cap (FOLD #10)** | travelpath; ledger #10/#0 | |
| 3.4 | `getCost` gates: Outland/Northrend level gates, staticPortal faction gate, spell-click flightpath fallback | travelpath top findings §2 | |
| 3.5 | `getRoute`/`getFullPath`: keep KEEPs (#24a trust-nodes + #26b guard, flat-only probe, fail-reason diagnostics); convert manual lock/unlock to scoped guard (OG leak class); keep `PrecomputeReachability` (documented approximation, #8) | travelpath | |
| 3.6 | Generation parity (intra-zone guidance quality): dock/portal/manual/helper nodes, `removeLow/removeUseless` always-prune + `clearRoutes` fix, `cropUselessLinks` both-direction, `isUselessLink` AT logic; **BuildPath reverse-mirror → OG one-way semantics**; loadNodeStore hotfixes **iff consuming cmangos-era dumps (data provenance: yes — port both)**; saveNodeStore deterministic ordering (keep TGT chunking) | travelpath | elevators excluded (transport non-goal) |

### Phase 4 — MovementActions core (the clean unit)

**File layout:** ported stack moves to a new translation unit
`src/Ai/Base/Actions/TravelMovement.cpp/h` (class stays `MovementAction` — methods relocate;
native combat movement remains in `MovementActions.cpp` untouched). The boundary between
"faithful port" and "modpb-native" becomes structural.

| # | Unit | Evidence | Action |
|---|---|---|---|
| 4.1 | `MoveTo2` (+coord overload): full parity incl. `lastMove.Set` arming (1.1), `setAtWaterSurface` waypoint snap (removing PORT-TODO), underwater/generatePath terms; KEEP SafeBotTeleport + unstick (#6, with #28b) | scope A | |
| 4.2 | `ResolveMovePath`: keep VEN no-fabrication (marked), no-progress-empty (#28a FOLD), map-609 unconditional, threshold via 1.6 config | scope A | |
| 4.3 | `DispatchMovement`: MoveSplinePath primitive (adjudication §3), density guard (FOLD), bounded raw hop (FOLD), ground-conform loop | scope A | |
| 4.4 | `HandleSpecialMovement`: **reinstate dropped gates** — `IsSpellReady`, reagent check, mount-speed check on NODE_TELEPORT (scope A found them replaced by weaker checks) | scope A | zone-to-zone legs = in scope |
| 4.5 | `WaitForReach` both overloads faithful (#3 absorbed) + `WaitForTransport`, `UpdateMovementState`, `IsMovingAllowed`, `IsDuplicateMove`, `IsWaitingForLastMove` verify | scope A | |
| 4.6 | `GeneratePathAvoidingHazards` + `CalculatePerpendicularPoint` + `IsValidPosition` (unblocked by 1.3) | scope A | removes the last movement PORT-TODO stub |
| 4.7 | `MoveNear`-class approach parity feeding `MoveWorldObjectTo`: keep 4-pass LOS-preference + ring inset (FOLD #23/#26c) + VEN's marked hold-distance fix; **resolve VEN's inverted walk-flag vs OG (Q4)** | scope A; consumers E8 | |

### Phase 5 — NewRpg adapter seam (absorb the reference consumer responsibilities, E14)

NewRpg remains the brain; these lifecycle behaviors move INTO the seam
(`MoveFarTo`/`MoveWorldObjectTo`/`MoveRandomNear` + rpgInfo):

| # | Responsibility | Reference mechanism | Seam implementation |
|---|---|---|---|
| 5.1 | Move-retry ladder | `IncRetry`+2 / `DecRetry`−1, `move>10` → COOLDOWN (E1/E14.5) | per-destination retry counter in rpgInfo; escalation: rotate POI → cooldown destination → (unwatched) teleport ladder (VEN's marked addition) |
| 5.2 | Stall bypass | "current position" LastChangeDelay 10s/60s (E4/E8) | via 1.5 StuckValues; re-issue movement even while `isMoving()` when position frozen |
| 5.3 | Arrival semantics | `radiusMin` → WORK at 5.5y (E1 CheckStatus) | per-destination-type arrival radius replacing hardcoded 10y latches |
| 5.4 | Cooldown valves | per-dest cooldownDelay 1s–5m + reset valve (E14.4) | replace permanent `lowPriorityQuest` with timed cooldowns + session reset valve |
| 5.5 | Unreachable/drop valves | MoveToRpgTarget's 7 drop valves + ignore list + crowd cap (E14.6) | extend `MarkUnreachable` memo → per-target retry counts; add crowd cap to WanderNpc; moving-NPC drop valve |
| 5.6 | Free-move leash | CanFreeMoveTo gates on movers (E14.7) | wire 1.2 values into seam movers (grouped-bot safety even while NewRpg stays solo-wired) |
| 5.7 | rpgDelay semantics | `SetActionDuration(rpgDelay)`, followers /5 (E12) | reconcile TGT rpgDelay=10000 vs OG 3000 (config note); keep TGT priority mechanism |

### Phase 6 — Cleanup & deletions

Remove superseded target inventions per ledger REPLACE list once their replacements land
(#2/#16 gates, #21 hunt, #27 rotation-as-workaround, #28c memo); delete dead legacy travel
chain (TravelStrategy never installed, `TravelAction::isUseful` dead code, TravelPlan
executor + capital `GetFullPath`/`FindRouteNearestNodes` — travelpath "Phase-C removal"
targets); final full-stack diff of every ported function against both references; update
`docs/` and memory.

---

## 6. Config reconciliation (drifts to document in conf.dist)

`rpgDelay` 3000→10000 · `sitDelay` 30000→20000 · `maxWaitForMove` 3000→5000 · `rpgDistance`
80→200 · `spellDistance` OG/VEN 25 vs TGT 28.5 · water cost 20 (filter) vs 10 (travel
override — restored by 0.3) · missing keys per 1.6 · TGT-only keys KEEP:
`disableMoveSplinePath`, `dynamicReactDelay`, `botSteepTravelCost`, `guardDistance`,
`wanderMaxDistance`, `botTaxiDelayMin/Max`, `RpgStatusProbWeight.*`, `AutoDoQuests`.

## 7. Reference bugs fixed in the port (marked in code, never copied)

ClipPath cut-before-end-check (OG+VEN) · OG getFullPath shared-lock leak · OG
`getTotalDistance` size-2 underflow · OG `calcMapOffset` missing `i++` (TGT fix KEEP) · OG
PortalNode leak on failed combos · OG A* sort-every-iteration heap (TGT lazy-deletion KEEP) ·
OG MoveToRpgTarget hold-distance overshoot (VEN marked fix adopted) · VEN unmarked:
`GetQuestRewardStatus` wrong-player, `IsInUse→GO_ACTIVATED` proxy, `isMoving` vs
`IsMovingIgnoreFlying`, dropped ENABLE_PLAYERBOTS 148 (their core) — documented, not
inherited.

## 8. Open questions for the owner (only genuine policy calls)

- **Q1 — steep-mmaps family**: all 11 KEEPs assume steep-tagged mmaps stay. Confirm the mmap
  policy is final (walkableSlopeAngle 60 + steep tagging 50–60°); if it ever changes, the
  whole family re-evaluates together.
- **Q2 — makeShortCut coin-flip** (`urand(0,1)` skips shortcutting 50% of the time — spreads
  bot paths): port faithfully (default) or omit deterministically?
- **Q3 — mob-avoid poly marking** (`MarkNavArea`) needs a **core** API (VEN patched its
  core). Module-side costs ship in 0.3 regardless. Approve the core addition, or keep
  mob-avoidance costs-only?
- **Q4 — walk-mode flag**: OG walks *indoors* (immersion), VEN walks *outdoors* (marked but
  semantically flipped). Which is intended? (Default: OG.)
- **Q5 — purpose-travel & group propagation** stay deferred (non-goals). Confirm.

## 9. Execution protocol

Phases strictly in order; units within a phase may interleave where dependencies allow. One
unit per commit; message carries the OG/VEN line refs + diff verdict. Build-green per phase.
Owner runs the regression suite (R1–R9) at phase boundaries; failures freeze the next phase
until root-caused (no workaround patches — fix the ported unit). The debug whisper stream is
the shared diagnostic language.
