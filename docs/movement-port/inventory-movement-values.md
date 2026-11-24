All reads complete. Below is the Scope D inventory.

**Path abbreviations**
- OG = `C:/Users/Admin/git/scratch/cmangos-playerbots/playerbot`
- VEN = `C:/Users/Admin/git/azerothcore-wotlk/modules/mod-cmangosbots/src/playerbot`
- TGT = `C:/Users/Admin/git/main/azerothcore-wotlk/modules/mod-playerbots/src`

For identical-layout files VEN line numbers equal OG line numbers.

---

## 1. LastMovement + LastMovementValue

**LastMovement (class)** | OG `strategy/values/LastMovementValue.h:7-58` | VEN same path `:7-58` | delta: NONE (byte-identical) | TGT `Ai/Base/Value/LastMovementValue.h:26-74` + `.cpp:10-77` DIVERGED: adds `MovementPriority` enum (h:17-24), `operator=` (h:32), `Set(mapId,x,y,z,ori,delayTime,priority)` (cpp:57-69), `setShort()` (cpp:71-75), fields `lastMoveToMapId/X/Y/Z/Ori`, `lastdelayTime`, `msTime`, `priority`, `std::future<TravelPath> future`; **drops OG's `Event moveEvent`**; keeps `taxiNodes/taxiMaster/lastFollow/lastAreaTrigger/lastFlee/lastPath/lastMoveShort/nextTeleport/lastTransportEntry` | deps: TravelPath (TravelNode.h), WorldPosition, ObjectGuid, Event (OG only), `getMSTime()` (TGT only) | notes: TGT's `msTime/lastdelayTime/priority` are load-bearing for `MovementAction::IsDuplicateMove` (TGT `MovementActions.cpp:1112-1122`, uses `lastMoveShort.GetExactDist` + `sPlayerbotAIConfig.maxWaitForMove`) and `IsWaitingForLastMove` (`:1124-1136`, priority comparison). OG instead throttles via `nextTeleport` + `moveEvent` + `SetDuration` (OG `MovementActions.cpp:220,524-527,643,1078-1088,1148,2047`). A wholesale port of OG LastMovement would break every TGT `MoveTo(..., MovementPriority)` caller; the fields must be merged, not replaced. OG writes `lastMoveShort` by direct assignment (OG `MovementActions.cpp:2176`), `Set(Unit*)` for follow (`:2441`), `lastFlee` for flee-loop throttle (`:2780-2938`).

**LastMovementValue** | OG h:60-66 | VEN h:60-66 | NONE | TGT h:76-83 PORTED-FAITHFUL (`ManualSetValue<LastMovement&>`, registered "last movement" `ValueContext.h:179/389`) | deps: LastMovement | notes: consumers in TGT: MovementActions, AttackAction, CombatActions, FollowActions, StayActions, UseMeetingStoneAction, TargetValue, NewRpgAction/NewRpgBaseAction, PlayerbotAI (debug print :5253), raid scripts.

**StayTimeValue ("stay time")** | OG h:68-72 | VEN same | NONE | TGT h:85-89 PORTED-FAITHFUL | deps: none | notes: cleared by OG `MovementAction::ClearIdleState` (OG MovementActions.cpp:2989-2993) together with `position["random"].Reset()` — TGT equivalent exists.

**LastLongMoveValue ("last long move")** | OG h:74-80 + `strategy/values/TargetValue.cpp:122-130` | VEN same (rename-only) | NONE | TGT `Ai/Base/Value/TargetValue.h:76-82` + `TargetValue.cpp:123-130` PORTED-FAITHFUL | deps: `"last movement"`.lastPath.getBack() | notes: interval OG `30` (framework units seconds) vs TGT `30*1000` ms — equivalent. Consumed by TGT `ChooseRpgTargetAction.cpp:340` and `MovementActions.cpp:1411`; by OG `FreeMoveCenterValue`.

**HomeBindValue ("home bind")** | OG h:83-91 + TargetValue.cpp:132-144 | VEN same | NONE | TGT TargetValue.h:84-89 + cpp:132-135 PORTED-FAITHFUL except `Format()` override ABSENT | deps: player homebind fields | notes: TGT reads `m_homebindMapId/X/Y/Z` directly vs OG `GetHomebindLocation()`.

---

## 2. FreeMoveValues — **entire value family ABSENT in TGT**

**FreeMoveCenterValue ("free move center")** | OG `strategy/values/FreeMoveValues.h:6-11`, `.cpp:10-53` | VEN same lines | delta: mechanical only (`GetGUID`, `m_mapId`) — NONE semantically | TGT ABSENT | deps: `"follow target"` value (**ABSENT in TGT** — TGT uses "group leader"), `"formation"` + `Formation::GetLocation/IsNullLocation`, `AI_VALUE2(PositionEntry,"pos","stay"/"guard")`, `"last long move"`, GuidPosition, `IsSafe`, `IsBeingTeleported` | notes: center = formation location shifted by follow-target's own long-move destination; falls back to stay/guard pos, else bot. `loc.mapid == -1` sentinel check.

**FreeMoveRangeValue ("free move range")** | OG h:13-18, cpp:55-79 | VEN same | NONE | TGT ABSENT | deps: `GetRange("wandermax"/"follow"/"guard")`, `"follow target"`, INTERACTION_DISTANCE for stay | notes: returns 0 (= unlimited) when no follow target.

**CanFreeMoveValue ("can free move", qualified)** + statics `CanFreeMove` (cpp:81-93), `CanFreeMoveTo` (95-99), `CanFreeTarget` (101-107), `CanFreeAttack` (109-112), `Calculate` (114-124) | OG h:20-32 | VEN same | NONE | TGT ABSENT as a value; one functional splinter exists: `static bool CanFreeTarget()` in TGT `Ai/Base/Value/GrindTargetValue.cpp:14-42` (explicitly commented as a cmangos CanFreeTarget port; anchor = master-if-following else self, range = `wanderMaxDistance` or `followDistance+wanderMaxDistance`, BG-exempt) used at `:86` and `:159-160` | deps: "free move center", "free move range", `GetRange("spell"/"attack"/qualifier)` | notes: `CanFreeAttack` uses `GetRange("attack")` which defaults to **0 = leash off** unless user manually sets `range attack`; `CanFreeTarget` clamps to spell range under "stay". OG consumers: ChooseRpgTargetAction:159,540-544; ChooseTargetActions:37; ChooseTravelTargetAction:103; FollowActions:47,189; MoveToRpgTargetAction:67; MoveToTravelTargetAction:246; PositionAction:137; PartyMemberValue:23,132; GrindTargetValue:48,162; PossibleTargetsValue::IsValid:125; wander triggers; OutOfFreeMoveRangeTrigger. **If ported vs adapted:** porting the value trio verbatim requires "follow target", "formation", "pos" entries and GetRange keys first; adapting per-call-site (as TGT did in GrindTargetValue) loses the stay/guard-anchored leash and the spell-range clamp, and ChooseRpgTarget/MoveToTravelTarget/PartyMember gates stay unleashed (TGT `ChooseRpgTargetAction.cpp:284` has the OG call commented out).

---

## 3. PositionValue family

**PositionEntry / PositionInfo** | OG `strategy/values/PositionValue.h:6-23` | VEN same (member renames only) | NONE | TGT `Ai/Base/Value/PositionValue.h:15-42` DIVERGED-minor: missing `PositionEntry(WorldPosition)` ctor, `Set(WorldPosition)`, and `Get()->WorldPosition` | deps: WorldPosition | notes: TGT consumers construct WorldPosition manually; porting OG code that calls `pos.Get()`/`pos.Set(WorldPosition)` needs these 3 one-liners added.

**PositionValue ("position")** | OG h:27-36, cpp:7-52 | VEN identical | NONE | TGT h:46-56, cpp:10-63 PORTED-FAITHFUL (Save/Load `^`-separated identical) | deps: split() | notes: keys used: "stay", "guard", "random", "return", "follow" (CustomFormation).

**SinglePositionValue ("pos", qualified)** | OG h:38-45, cpp:54-72 | VEN identical | NONE | TGT h:76-83, cpp:67-87 PORTED-FAITHFUL | deps: "position" map | notes: both keep OG's copy-then-overwrite no-op quirk in `Set`. TGT "pos" registered `ValueContext.h:207`; guard/stay flows exist via `GuardAction`/`ReturnToStayPositionAction` (TGT `PositionAction.h:37-47`) — so "pos" stay/guard semantics survive in TGT.

**CurrentPositionValue ("current position")** | OG h:47-53 | VEN same | NONE | TGT h:58-74 + cpp:65 PORTED-FAITHFUL (LogCalculatedValue, minChangeInterval 60, logLength 30, EqualToLast `fDist < tooCloseDistance`) | deps: tooCloseDistance | notes: the stuck triggers' data source in both trees.

**MasterPositionValue ("master position")** | OG h:55-61 | VEN same | NONE | TGT ABSENT | deps: MemoryCalculatedValue, proximityDistance | notes: no TGT equivalent.

**CustomPositionValue ("custom position", qualified)** | OG h:63-68 | VEN same | NONE | TGT ABSENT | notes: OG uses it for "fish spot" (FishAction.cpp:30-171) and RTSC; TGT covers RTSC via `RTSCSavedLocationValue` (`RTSCValues.h:40`) and reimplemented fishing internally (`FishingAction.cpp`) — port only needed if porting OG FishAction verbatim.

---

## 4. Formations (OG `strategy/values/Formations.cpp` 711 lines; VEN 710; TGT `Ai/Base/Value/Formations.cpp` 702)

VEN global delta: mechanical renames + ONE marked adaptation — NearFormation's `GetHitPosition` terrain/LoS clamp deferred (VEN cpp ~167-170, "mod-cmangosbots:" comment) and `GetObjectSize` for `GetObjectBoundingRadius`. Everything else NONE.

- **IsSameLocation** | OG cpp:13-16 | VEN :15 renames | TGT cpp:16-20 PORTED-FAITHFUL.
- **Formation::IsNullLocation** | OG cpp:23-26 | VEN same | TGT cpp:22 PORTED-FAITHFUL.
- **Formation::GetMaxDistance** | OG cpp:18-21 (`GetRange("follow")`) | VEN same | TGT h:24 DIVERGED: returns `sPlayerbotAIConfig.followDistance` (ignores per-bot `range follow` override + raid follow).
- **Formation::GetAngle** | OG cpp:28-40 | VEN same | TGT **ABSENT** — TGT Formation base has no GetAngle.
- **Formation::GetOffset** | OG cpp:42-53 | VEN same | TGT **ABSENT**. Both are required by OG `UpdateFollowTrigger` and OG follow dispatch (chase angle/offset comparison) — porting OG follow needs these plus ServerFacade chase getters.
- **Formation::GetFollowAngle** | OG cpp:459-518 | VEN same (GetSource renames) | TGT cpp:419-504 DIVERGED: master-based instead of "follow target"; drops `IsSafe`+`IsAlive` facade checks (uses IsAlive+same-map); roster build order subtly differs (OG: three passes — DPS middle-insert, then healers middle-insert, then tanks alternating ends; TGT: single pass with if/else so healers interleave with DPS as encountered); total counting differs (OG `roster.size()+1`, TGT increments per member).
- **FollowFormation::GetLocation** | OG cpp:55-69 | VEN same | TGT **ABSENT** (TGT FollowFormation is a pure GetTargetName marker).
- **MoveAheadFormation::GetLocation** | OG cpp:71-119 (returns GetLocationInternal; rest dead code) | VEN same | TGT cpp:50-84 DIVERGED: adds `ValidateTargetContext(master,bot)` gate (TGT-only helper cpp:24-48), re-wraps into master's mapId; OG dead block kept as comments.
- **MeleeFormation** | OG cpp:123-130 (target "master target", angle=GetFollowAngle, offset=follow range) | VEN same | TGT cpp:86-92 DIVERGED: target "group leader", no angle/offset.
- **QueueFormation** | OG cpp:132-139 | VEN same | TGT cpp:94-100 same target "line target"; no angle/offset (relies on follow default).
- **NearFormation** | OG cpp:141-183 | VEN: terrain clamp deferred (marked) | TGT cpp:102-131 DIVERGED: master-based; `CheckCollisionAndGetValidCoords` + `GetHoverHeight` replaces GetHeight/GetHitPosition/UpdateAllowedPositionZ; no swim/fly branch.
- **ChaosFormation** | OG cpp:186-233 (3s jitter ±tooCloseDistance/2) | VEN same | TGT cpp:133-183 PORTED-FAITHFUL logic with the AC collision API; GetMaxDistance followDistance+dr.
- **CircleFormation** | OG cpp:235-270 (radius = follow range around current target, fallback follow target) | VEN same | TGT cpp:185-238 DIVERGED: class-based radius (casters/heal use `GetRange("flee")`, else 2.0f); falls back to master; **quirk: validates `master` context but positions around `target`** — with a valid master and dead/absent target it still dereferences target for coords only after `!target||target==bot → target=master`, so safe, but a live non-master target on another map isn't checked.
- **LineFormation** | OG cpp:272-306 (range=follow) | VEN same | TGT cpp:240-277 DIVERGED: range hardcoded 2.0f, master-based, no IsSafe filter.
- **ShieldFormation** | OG cpp:308-369 | VEN same | TGT cpp:279-343 PORTED-FAITHFUL (master-based, followDistance).
- **FarFormation** | OG cpp:371-405 (a FollowFormation: GetAngle correction toward behind via chase angle; offset=follow range) | VEN same | TGT cpp:345-417 COMPLETELY DIVERGED: own GetLocation using `farDistance` (config 20.0f) + ground-height fan search (π/16 steps) + collision clamp. Different algorithm, different intent (OG drifts angle; TGT teleport-style far anchor).
- **CustomFormation** | OG cpp:407-456 (persists relative offset in `position["follow"]`, rotateXY math) | VEN same | TGT **ABSENT** | deps: WorldPosition::rotateXY, PositionEntry(WorldPosition).
- **FormationValue ctor/Reset** | OG cpp:520-536 (loads `sPlayerbotAIConfig.defaultFormation`, fallback "near") | VEN same | TGT cpp:506-518 DIVERGED: hardcoded `ChaosFormation` default, **no Reset() override**, no `AiPlayerbot.DefaultFormation` config (OG PlayerbotAIConfig.cpp:543).
- **FormationValue::Save/Load** | OG cpp:538-598 | VEN same | TGT cpp:520-591 DIVERGED: "default"→chaos (OG "default"→near), no "custom" branch.
- **SetFormationAction::Execute** | OG cpp:600-641 | VEN same | TGT cpp:593-629 DIVERGED-minor: no "default/reset" branch; no requester (TellMaster only).
- **FormationPositionValue ("formation position")** | OG Formations.h:61-75 | VEN same | TGT **ABSENT** (needed by OG UpdateFollowTrigger).
- **MoveFormation::MoveLine** | OG cpp:643-669 | VEN same | TGT cpp:631-659 PORTED-FAITHFUL.
- **MoveFormation::MoveSingleLine** | OG cpp:671-711 | VEN same | TGT cpp:661-702 DIVERGED-minor: AC collision API + in-world guards; drops INVALID_HEIGHT bail.
- **ArrowFormation (Arrow.cpp/h)** | OG `strategy/values/Arrow.cpp:1-176` | VEN mechanical only | TGT `Ai/Base/Value/Arrow.cpp` DIVERGED: master-based; **healer lines commented out (`//@TODO Implement Healer Lines`)**; spacing `followDistance + tooCloseDistance/2` per block vs OG `range × lines`; MultiLineUnitPlacer lost its range parameter.

---

## 5. Stances

- **Stance::GetTarget** | OG `strategy/values/Stances.cpp:10-19` | VEN same | TGT `Ai/Base/Value/Stances.cpp:12-23` DIVERGED-minor: fallback value is "pull target" (OG: "attack target") | notes: **shared bug in all three**: the fallback `GetUnit(...)` result is discarded — function returns NULL whenever the primary target name misses. Port should fix (`return ai->GetUnit(attackTarget);`).
- **Stance::GetLocation** | OG :21-28 | VEN same | TGT :25-32 PORTED-FAITHFUL.
- **Stance::GetNearLocation** | OG :30-42 (`IsWithinLOS(x,y,z+GetCollisionHeight(), true)`) | VEN drops the `true` 4th arg (unmarked minor — AC signature differs; M2 models no longer ignored) | TGT :34-46 DIVERGED-minor: `IsWithinLOS(x,y,z)` — drops collision-height offset AND flag (LOS from feet).
- **Stance::GetTargetName/GetMaxDistance** | OG h:20-21 ("current target", contactDistance) | VEN same | TGT cpp:57-59 PORTED-FAITHFUL.
- **MoveStance::GetLocationInternal** | OG :44-51 (`max(meleeDistance, GetObjectBoundingRadius())`) | VEN `GetObjectSize()` | TGT :48-55 `GetCombatReach()` — AC-equivalent, fine.
- **NearStance** | OG :55-106 | VEN mechanical | TGT :70-115 PORTED-FAITHFUL minus `IsSafe(member)` filters in the behind-count loop.
- **TankStance** | OG :108-118 | VEN same | TGT :117-127 PORTED-FAITHFUL.
- **TurnBackStance** | OG :120-147 | VEN same | TGT :129-158 PORTED-FAITHFUL minus IsSafe.
- **BehindStance** | OG :149-177 | VEN same | TGT :160-193 PORTED-FAITHFUL minus IsSafe.
- **StanceValue ctor/Reset/Save/Load** | OG :180-220 | VEN same | TGT :195-229 PORTED-FAITHFUL except **no Reset() override** (OG resets to NearStance).
- **SetStanceAction::Execute** | OG :222-255 | VEN same | TGT :231-266 PORTED-FAITHFUL (TellMaster instead of requester).

---

## 6. Attackers / possible attack targets

**AttackersValue::Calculate** | OG `strategy/values/AttackersValue.cpp:10-168` | VEN same file (shifted +1 for Pet.h include, marked) | VEN delta: ADAPTATION(marked ×3): Pet.h include; `CallForAllControlledUnits` → flat `unit->m_Controlled` loop (VEN :292-296 — loses recursion/mask filtering, inserts unvalidated controlled units — marked-deliberate); `duel->Opponent` etc. mechanical | TGT `Ai/Base/Value/AttackersValue.cpp:15-64` DIVERGED (different architecture): threat-list driven + "prioritized targets" + skull icon + duel + arena workaround; no qualifier/getOne, no shareTargets, no flying/teleport/CLIENT_CONTROL_LOST gates, no "focus rti targets" short-circuit | deps OG: "possible targets" (multi-qualified range:ignoreValidate), "current/old target", "attack target", "pull target", "nearest friendly players", PossibleTargetsValue::IsValid, EnemyPlayerValue::GetMaxAttackDistance, sightDistance | notes: **OG bug preserved in VEN** at :66 — `std::string valueName = "attackers" + !qualifier.empty() ? "::" + qualifier : "";` operator-precedence makes valueName always `"::" + qualifier`, so the `shareTargets` copy path can never find the value and is effectively dead code. Port should either fix or drop.

**AttackersValue::AddTargetsOf(Group)** | OG :170-193 | VEN mechanical | TGT AddAttackersOf(Group) :67-79 PORTED-FAITHFUL in spirit (alive/same-map/sightDistance gates).

**AttackersValue::AddTargetsOf(Player)** | OG :195-306 | VEN as above | TGT AddAttackersOf(Player) :90-105 DIVERGED-major: only `GetThreatMgr().GetThreatenedByMeList()` — no possible-targets union, no current/old/attack/pull targets, no `getAttackers()`, no pet attackers, no guardian expansion. TGT bot awareness of adds comes solely from threat + PossibleAddsValue.

**AttackersValue::InCombat** | OG :308-332 | VEN mechanical | TGT ABSENT (nearest analogue `hasRealThreat` :123-128: alive/in-world/!polymorphed/!friendly — different semantics).

**AttackersValue::IsValid** | OG :334-412 | VEN adds **marked** geometric-LOS gate (VEN :413-419) | TGT split into `IsPossibleTarget` (:130-237, AC-native: visibility, flags, damage-immunity, PvP-prohibited-zone incl. pet attack-stop side effect, critter, evade, tap/canAttack leader-threat logic) + `IsValidTarget` (:239-242 = IsPossibleTarget + `IsWithinLOSInMap`) | notes: VEN's LOS gate was added specifically to mirror TGT — so on this point TGT is the reference.

**AttackersValue::IgnoreTarget** | OG :414-487 | VEN: `target->AI()->IsPreventingDeath()` disabled (marked); rest mechanical | TGT **ABSENT** | deps: "travel target traveling", `AI_VALUE2(float,"distance","travel target")`, `AI_VALUE2(bool,"trigger active","out of react range")`, GetFixedBotNumber, training-dummy entries | notes: this is the "don't aggro +5-level mobs while long-distance traveling" guard — losing it makes traveling bots pick fights; a port of OG travel should bring it.

**AttackersValue::Format** | OG :489-507 | VEN `.ToString()` | TGT ABSENT.

**AttackersTargetingMeValue ("attackers targeting me")** | OG :509-524, h:50-55 | VEN mechanical | TGT **ABSENT** (only count-style `MyAttackerCountValue` exists).

**PossibleAttackTargetsValue ("possible attack targets")** | OG `strategy/values/PossibleAttackTargetsValue.cpp` | VEN same | TGT **ABSENT** — explicitly named in PORT-TODOs: TGT `Mgr/Travel/TravelNode.cpp:1092-1105` (ClipPath) and referenced in `MovementActions.cpp` stub. Functions:
- Calculate | OG :16-47 (getOne fast path via `AI_VALUE2(...,"attackers",1)`) | VEN same | TGT ABSENT.
- **RemoveNonThreating** | OG :49-104 (三-bucket CC logic: invalid→drop, breakable-CC→bucket, unbreakable-CC→bucket, else keep; if nothing left, fall back to unbreakable then breakable) | VEN same | TGT: a method of the SAME NAME exists at `AttackersValue.cpp:107-121` but is unrelated (map/threat/valid filter, no CC buckets).
- HasIgnoreCCRti | OG :106-110 (skull icon 7) | VEN mechanical | TGT ABSENT (skull logic inlined in AttackersValue::Calculate :40-44 with opposite sense — skull forcibly ADDED).
- HasBreakableCC | OG :112-144 (polymorph/frozen/sap/gouge/shackle) | VEN mechanical | TGT ABSENT.
- HasUnBreakableCC | OG :146-164 (stun/fear/root) | VEN `HasUnitState(UNIT_STATE_STUNNED)` | TGT ABSENT.
- IsImmuneToDamage | OG :166-199 | VEN unmarked deviation: charm check `target->IsInTeam(player,true)` → `(GetTypeId()==player's && IsFriendlyTo)` approximation | TGT: different immunity check inside IsPossibleTarget (:158-160, school-mask based).
- Format | OG :201-219 | VEN mechanical | TGT ABSENT.
- IsTapped | OG :221-262 | VEN **marked reorder fix** (untapped-first, VEN :237-253) — deliberate deviation matching mod_playerbots | TGT: equivalent logic inlined in IsPossibleTarget `canAttack` block :223-230.
- IsValid | OG :264-294 | VEN same+LOS | TGT ABSENT.
- IsPossibleTarget | OG :296-333 | VEN **marked LOS gate** :307-318 | TGT: same-named function on AttackersValue (different content).
- PossibleAddsValue | OG :335-361, h:38-53 | VEN mechanical | TGT `AttackersValue.cpp:244-276` PORTED-FAITHFUL except compares against "attackers" (TGT has no "possible attack targets") and uses `GetThreatMgr().GetLastVictim()`.
- **Counts**: "attackers count"/"possible attack targets count"/"has attackers"/"has possible attack targets" | OG `AttackerCountValues.h:6-32` | VEN same | TGT: only "attackers count", "my attackers count", "has aggro", "balance percentage" (`AttackerCountValues.h`) — possible-attack-target variants ABSENT.

**Ordering note**: OG "attackers" materializes a `std::set<Unit*>` (pointer-ordered) → list; "possible attack targets" preserves that order and target selection uses FindTargetStrategy scoring, not list order. TGT uses `unordered_set` → GuidVector with prioritized/skull/duel/arena appended at the END. No meaningful ordering contract to preserve either way.

---

## 7. Hazards — **entire subsystem ABSENT in TGT**

- **Hazard (class)** | OG `strategy/values/HazardsValue.h:7-36`, `.cpp:9-104` (ctors set `hazardNavmeshArea=14`, `previousNavmeshArea=11`; `==` by guid-else-position; `<` by radius; GetPosition resolves live object first; IsValid = (object||position) && !expired) | VEN identical (only `PathGenerator.h` include swap) | TGT ABSENT | deps: WorldPosition, ObjectGuid, ai->GetUnit/GetGameObject.
- **HazardPosition** (`typedef std::pair<WorldPosition,float>`) | OG h:39 | VEN same | TGT ABSENT (named in stub comments TGT `MovementActions.cpp:3602-3605`, `TravelNode.cpp:1095`).
- **StoredHazardsValue ("stored hazards")** | OG h:42-46 | VEN same | TGT ABSENT.
- **AddHazardValue ("add hazard")** | OG h:49-56, cpp:106-118 (Set dedups into stored list) | VEN same | TGT ABSENT.
- **HazardsValue ("hazards", interval 1)** | OG h:59-66, cpp:120-162 (prunes invalid/expired, writes back, returns position/radius pairs) | VEN same | TGT ABSENT.
- **Publishers** (all OG+VEN, none in TGT): `CloseToHazardTrigger::IsActive` OG `strategy/triggers/DungeonTriggers.cpp:87-127` — caches EVERY possible hazard each tick via `SET_AI_VALUE(Hazard,"add hazard",...)` and suppresses trigger while last action was a MovementAction; subclasses `CloseToGameObjectHazardTrigger`/`CloseToCreatureHazardTrigger`/`CloseToHostileCreatureHazardTrigger` (DungeonTriggers.h:92-129). Concrete instances: `MagmadarLavaBombTrigger` (MoltenCoreDungeonTriggers.h:31-34; GO 177704, r=5, 60s), `VoidZoneTooCloseTrigger` (KarazhanDungeonTriggers.h:31-34; creature 16697, r=5, 99999999s).
- **Consumers**: `MoveAwayFromHazard::Execute` (OG DungeonActions.cpp:11-100; perpendicular escape, 10 angle attempts, avoids other hazards); `MoveAwayFromCreature` (:154 uses hazards during placement); `MovementAction::IsValidPosition` (OG MovementActions.cpp:2995-3002), `IsHazardNearPosition` (:3004-3028), `GeneratePathAvoidingHazards` (:3030-~3130, perpendicular re-route + midpoint insertion, maxPointsInserted 20); `TravelPath::ClipPath` hazard scan (OG TravelNode.cpp:1132, 1165-1176, sq-distance early-clip).
- **VEN**: all consumers ported and live — `CalculatePerpendicularPoint` VEN MovementActions.cpp:1627, endpoint-shift :1703-1737, IsValidPosition :2117, IsHazardNearPosition :2120, GeneratePathAvoidingHazards :2146.
- **TGT**: `GeneratePathAvoidingHazards` STUBBED no-op (TGT MovementActions.cpp:3600-3607, still called at :3943), ClipPath hazard scan STUBBED (TravelNode.cpp:1092-1114 with verbatim cmangos block preserved in comments), `MoveAwayFromHazard`/`IsValidPosition`/`IsHazardNearPosition`/`CalculatePerpendicularPoint` ABSENT. TGT has its own `MoveAwayFromCreatureAction` (MovementActions.cpp:2974, gated by `bot->CanFreeMove()` :3052) — unrelated implementation.
- **Port note**: TGT ClipPath also contains a **deliberate fix vs OG** — `startP==end` checked BEFORE `cutTo` (TGT TravelNode.cpp:1078-1087, iterator-invalidation bug latent in OG); keep the fix when filling in the hazard/enemy scans.

---

## 8. Movement triggers

**Stuck family** (OG `strategy/triggers/StuckTriggers.h`, all inline; `.cpp` empty):
- **MoveStuckTrigger ("move stuck", 5)** | OG h:20-61 | VEN same | TGT `Ai/Base/Trigger/StuckTriggers.cpp:13-58` DIVERGED: drops the `GetGroupMaster() && !GetGroupMaster()->GetPlayerbotAI()` skip and the reset-"current position"-on-inactivity; reads `LogCalculatedValue::LastChangeDelay()/ValueLog()` directly instead of OG's "time since last change"/"distance moved since" qualified values (functionally similar: 5min frozen OR <50y over 10min) | deps: "current position" log value | notes: TGT binds → "reset" (MaintenanceStrategy.cpp:56); OG binds → UnstuckAction ("reset").
- **MoveLongStuckTrigger ("move long stuck", 5)** | OG h:63-145 (invalid grid bounds; **mmap-tile-not-loadable check** via `MMapFactory::IsMMapIsLoaded/loadMap`; 10min frozen; XP-gated 15min distance) | VEN: mmap preload check DEFERRED (marked, "Phase-8"); grid-bounds kept | TGT cpp:60-142 DIVERGED + **suspected inverted condition**: `if (bot->GetMap()->IsGridCreated(GridCoord(...))) return true;` (:92-99) flags "stuck" when the grid IS created — the opposite of OG's "stuck in unloaded grid". Latent only because **no TGT strategy binds "move long stuck"** (registered TriggerContext.h:209, zero NextAction bindings) | deps: "experience" memory value, grid API.
- **CombatStuckTrigger ("combat stuck", 5)** | OG h:147-186 | VEN `duel->StartTime` rename | TGT cpp:144-170 DIVERGED-minor: gate `bot->IsInCombat()` vs OG `GetState()==BOT_STATE_COMBAT`; drops group-master gate and duel check | notes: **OG bug**: `bot->duel->startTime - time(0) > 15*MINUTE` is negative-always (operands reversed) — duel branch never fires; VEN preserves it. TGT dropped the branch entirely. Bound → "reset" (TGT CombatStrategy.cpp:39).
- **CombatLongStuckTrigger ("combat long stuck", 5)** | OG h:188-227 (15min) | VEN same (+bug) | TGT cpp:172-198 same-minus-duel; **unbound in TGT** (registered only). OG routes long-stuck to hearthstone/repop via UnstuckAction (OG UnstuckAction.cpp:25-36,46-57) — TGT has no hearthstone/repop escalation path for stuck.
- LeaderIsAfkTrigger | OG h:229-250 | VEN ObjectAccessor | TGT ABSENT.

**Range/combat-move family** (OG `strategy/triggers/RangeTriggers.h`, inline):
- **EnemyTooCloseForSpellTrigger** | OG :11-87 (kite-decision: chaser-speed bail, CC-awareness via HasBreakable/UnBreakableCC, combined combat reach, coeff 0.4/0.6/0.7-raid vs `GetRange("spell")`) | VEN: shims `BotGetTargetUnit`/`BotCombinedCombatReach`; `GetSpeedInMotion` → `GetSpeed(MOVE_RUN)` (unmarked minor: nominal vs current-motion speed) | TGT cpp:14-51 DIVERGED: reduced to `(victim!=bot || frozen || rooted) && GetObjectSize()<=10 && IsWithinCombatRange(MIN_MELEE_REACH)`; OG formula present but commented out.
- **EnemyTooCloseForShootTrigger** | OG :89-158 | VEN as above | TGT cpp:94-131 same simplification.
- EnemyTooCloseForAutoShotTrigger | OG ABSENT | VEN ABSENT | TGT-only cpp:53-92 (hunter trap-aware).
- **EnemyTooCloseForMeleeTrigger** | OG :160-173 | VEN same | TGT cpp:133-140 PORTED-FAITHFUL ("inside target" value; interval 5 vs OG 3).
- EnemyInRangeTrigger | OG :175-209 | VEN same | TGT ABSENT (only the EnemyIsClose specialization survives).
- **EnemyIsCloseTrigger** | OG :211-215 | VEN same | TGT cpp:142-147 PORTED-FAITHFUL (tooCloseDistance).
- EnemyWithinMeleeTrigger | TGT-only cpp:149-153.
- **OutOfRangeTrigger** | OG :217-236 (uses "distance" value) | VEN same | TGT cpp:155-164 DIVERGED-minor: `!IsWithinCombatRange(target, distance+CONTACT_DISTANCE)` (reach-aware, no LOS).
- **EnemyOutOfMeleeTrigger** | OG :238-251 (`!CanReachWithMeleeAttack || !IsWithinLOSInMap(target,true)`) | VEN `IsWithinMeleeRange` + LOS-no-flag | TGT h:77-86 DIVERGED: custom IsActive **commented out** — falls back to plain combat-range check, **LOS condition lost** (melee bot no longer repositions purely on LOS break via this trigger).
- **EnemyOutOfSpellRangeTrigger** | OG :253-266 (DIST_CALC_COMBAT_REACH − contactDistance, + LOS) | VEN GetDistance (AC subtracts reach) + LOS | TGT cpp:166-180: custom IsActive commented out — base check only, **LOS lost**.
- **PartyMemberToHealOutOfSpellRangeTrigger** | OG :268-282 | VEN same | TGT cpp:193-206 PORTED-FAITHFUL (2d + LOS; heal range +1.0f).
- **FarFromMasterTrigger** | OG :284-305 ("master target", friendliness gate, same-transport skip) | VEN mechanical | TGT cpp:208-211 DIVERGED: distance-to-"group leader" only — no transport skip (bots on boats with master will trip it), no friendly gate.
- **OutOfReactRangeTrigger ("out of react range")** | OG :307-311 (distance = `sPlayerbotAIConfig.reactDistance` = 150, interval 2) | VEN same | TGT h:118-122 DIVERGED: hardcoded **50.0f**, interval 5 | notes: OG AttackersValue::IgnoreTarget queries `"trigger active" "out of react range"` — porting IgnoreTarget needs this trigger's OG semantics (150y master-distance).
- NotNearMasterTrigger | OG :313-322 | VEN same | TGT ABSENT.
- **UpdateFollowTrigger ("update follow")** | OG :324-367 | VEN mechanical | TGT ABSENT | deps: "follow target", "formation" (GetAngle/GetOffset), "formation position", chase getters (`sServerFacade.GetChaseTarget/Angle/Offset`), IsStateActive | notes: core of OG's reactive follow refresh; unportable until Formation::GetAngle/GetOffset and follow-target value exist.
- **StopFollowTrigger ("stop follow")** | OG :369-390 | VEN `IsInFlight` | TGT ABSENT.
- **WanderFarTrigger / WanderMediumTrigger / WanderNearTrigger** | OG :392-428 (built entirely on `AI_VALUE2(bool,"can free move", "wandermin"/"wandermax")`) | VEN: **unmarked DEVIATION/BUG** — WanderMediumTrigger uses `"wandermaz"` (typo, VEN RangeTriggers.h:412): `GetRange("wandermaz")` returns 0 → CanFreeMove()==true → medium-wander fires anywhere beyond wandermin, overlapping far-wander | TGT ABSENT (no wander strategy triggers).
- **WaitForAttackSafeDistanceTrigger** | OG :430-461 | VEN same | TGT `Ai/Base/Trigger/WaitForAttackTriggers.h:18-47` PORTED-FAITHFUL.
- **OutOfFreeMoveRangeTrigger ("out of free move range")** | OG :463-472 (`!AI_VALUE(bool,"can free move")`) | VEN same | TGT ABSENT | notes: OG FollowMasterStrategy binds it → check-mount-state + follow at ACTION_HIGH (OG FollowMasterStrategy.cpp:8-11); TGT FollowMasterStrategy is a bare `"follow"` NextAction — the whole leash-driven follow kick is missing.

**Wait-for-attack machinery**:
- **WaitForAttackTimeValue ("wait for attack time")** | OG `strategy/values/WaitForAttackTimeValue.h` (ManualSetValue<uint8> default 10, Qualified, Save/Load persisted) | VEN same | TGT `Ai/Base/Value/WaitForAttackTimeValue.h:13-17` PORTED-FAITHFUL minus Save/Load persistence and Qualified. TGT also adds `CombatStartTimeValue` there (:19-23; OG has its own CombatStartTimeValue.h).
- **WaitForAttackStrategy::ShouldWait/GetWaitTime/GetSafeDistance/GetSafeDistanceThreshold** | OG `strategy/generic/CombatStrategy.cpp:97-140` + h:54-76 (safeDistance = spellDistance; threshold 2.5) | VEN same | TGT `Ai/Base/Strategy/WaitForAttackStrategy.cpp:29-74` PORTED-FAITHFUL, with two deliberate improvements: enemy-player check `target->IsPlayer()` (**fixes OG bug** — OG compares the target against itself via `IsFriendlyTo(target, dynamic_cast<Player*>(target))`, so the branch never fired) and combat-start-time bookkeeping done inline.
- **WaitForAttackMultiplier** | OG CombatStrategy.cpp:142-159 | VEN same | TGT :76-92 PORTED-FAITHFUL (+extra exempt action "reach pull").

---

## 9. GetRange keys + config defaults

**PlayerbotAI::GetRange** | OG `PlayerbotAI.cpp:6754-6775` | VEN `PlayerbotAI.cpp:6765-6786` NONE (identical; `isRaidGroup` rename) | TGT `Bot/PlayerbotAI.cpp:5397-5431` DIVERGED: **no raid-group "followraid" promotion, no "wandermin", no "attack" (explicit 0), no "followraid"; adds "melee" fallback**. Both check the manual `RangeValue` ("range", qualified) first.

**RangeValue ("range")** | OG `strategy/values/RangeValues.h/.cpp` | VEN same | TGT `Ai/Base/Value/RangeValues.h/.cpp` PORTED-FAITHFUL (Save/Load persisted).

Key-by-key (fallback config, OG/VEN default → TGT default; OG config `PlayerbotAIConfig.cpp:121-143`, VEN `:149-171` identical to OG, TGT `PlayerbotAIConfig.cpp:89-113`):
| key | OG/VEN fallback | TGT fallback | delta |
|---|---|---|---|
| spell | spellDistance 25.0 | spellDistance **28.5** | value drift |
| shoot | shootDistance 25.0 | shootDistance **5.0** | **huge drift — TGT shoot leash is melee-ish; OG kiting math assumes 25** |
| flee | fleeDistance 8.0 | fleeDistance 5.0 | drift |
| heal | healDistance 125.0 | healDistance **38.5** | huge drift (OG 125 is deliberate — heal-follow across rooms) |
| follow | followDistance 1.5 | followDistance 1.5 | same |
| followraid | raidFollowDistance 5.0 | **key ABSENT** (no raidFollowDistance config) | lost raid spacing |
| guard | sightDistance 75.0 | guardDistance (default = sightDistance **100**) | TGT has separate GuardDistance key |
| wandermin | wanderMinDistance 5.0 | **key + config ABSENT** | wander-medium/near triggers unportable without it |
| wandermax | wanderMaxDistance 50.0 | wanderMaxDistance 50.0 | same (TGT comments it as the free-move leash) |
| attack | 0 (leash off) | **key ABSENT** (returns 0 via fall-through) | same effective behavior |
| melee | (no key; OG uses meleeDistance config directly, 1.5) | meleeDistance **0.75** | drift |
| (related) sightDistance | 75.0 | 100.0 | drift |
| (related) farDistance | 20.0 | 20.0 | same |
| (related) reactDistance | 150.0 | 150.0 | same — but TGT OutOfReactRangeTrigger ignores it (hardcoded 50) |
| defaultFormation | "near" (OG cfg:543) | **ABSENT** (hardcoded chaos) | |

OG call-site census (`GetRange("...")`): follow×41, spell×23, flee×4, wandermax×3, heal×3, shoot×2, guard×1, attack×1 (+qualifier pass-through "wandermin"/"wandermax" via "can free move"). TGT census: spell×11, shoot×9, heal×5, flee×4, follow×1.

---

## Dependency summary — what Scope D needs from other subsystems

**Values/types this scope consumes that must exist first (Scope A/B/C side):**
1. **"follow target" value** (OG FollowTargetValue) — ABSENT in TGT (uses "group leader"). Blocks: FreeMoveCenterValue/RangeValue, Formation::GetAngle/GetOffset, all OG formation GetLocation bodies, UpdateFollow/StopFollow triggers, FarFromMasterTrigger transport-skip.
2. **"master target" value** — ABSENT in TGT. Blocks: FarFromMasterTrigger (OG form), MeleeFormation target, MasterPositionValue.
3. **TravelPath / TravelTarget** ("travel target traveling", `distance("travel target")`) — needed by AttackersValue::IgnoreTarget; TGT travel values exist but under NewRpg/TravelMgr naming — mapping required.
4. **ServerFacade chase getters** (`GetChaseTarget/GetChaseAngle/GetChaseOffset`) — needed by UpdateFollowTrigger + FarFormation(OG); no TGT equivalent (AC FollowMovementGenerator exposes no angle/offset getters — needs a botcompat shim like VEN's).
5. **PathFinder/navmesh area codes** — Hazard carries `hazardNavmeshArea=14 / previousNavmeshArea=11`; only meaningful once the Scope-B pathfinder honors per-area costs; harmless to port ahead of it.
6. **Qualified multi-value plumbing** — OG "attackers"/"possible targets" use `Qualified::MultiQualify` (range:ignoreValidate) and int-qualifier getOne; TGT Qualified exists but AttackersValue is unqualified — porting PossibleAttackTargetsValue requires re-qualifying TGT "attackers" or porting OG AttackersValue wholesale.
7. **Values framework primitives** — `MemoryCalculatedValue/LogCalculatedValue` (present in TGT), "time since last change"/"distance moved since" qualified helpers (ABSENT in TGT; stuck triggers there read the log value directly — acceptable adaptation).
8. **"inside target"**, "distance" (qualified), "trigger active" (qualified) values — present in TGT except "trigger active" needs checking when porting IgnoreTarget.
9. **PlayerbotAI::IsStateActive / BotState reaction-state** — named missing by TGT's own PORT-TODO (TravelNode.cpp:1093) for ClipPath's combat gate.
10. **UnstuckAction escalation** (hearthstone/repop) — OG binds long-stuck triggers to it; TGT has "reset" only.

**What this scope provides that other scopes consume:**
- "possible attack targets" + RemoveNonThreating CC-buckets → TravelPath::ClipPath (Scope B), TargetValue::FindTarget/attack-target selection (Scope C), counts.
- "hazards"/HazardPosition → ClipPath, GeneratePathAvoidingHazards, IsValidPosition, MoveAwayFromHazard (Scope B movement).
- "can free move"/center/range → ChooseRpgTarget/ChooseTravelTarget/MoveToTravelTarget/FollowActions/PositionAction gating (Scope A/C).
- LastMovement fields (lastPath/lastMoveShort/nextTeleport/moveEvent vs TGT priority/msTime) → every MoveTo/dispatch path (Scope B) — **merge, don't replace**.
- Formation/Stance GetLocation + GetAngle/GetOffset → follow dispatch and combat positioning (Scope B/C).

**Highest-risk findings for the port plan:**
1. TGT `MoveLongStuckTrigger` grid condition inverted (`IsGridCreated` → stuck) — latent only because the trigger is unbound; must be fixed before binding it.
2. VEN `"wandermaz"` typo (VEN RangeTriggers.h:412) — unmarked deviation; do not copy into the port.
3. OG shareTargets valueName precedence bug (AttackersValue.cpp:66) and OG duel `startTime - time(0)` reversed-operand bug (StuckTriggers.h:177/218) — both preserved in VEN; fix or drop in the port.
4. Stance::GetTarget discarded-fallback bug present in all three trees.
5. TGT range-trigger simplifications silently dropped LOS from enemy-out-of-melee/spell triggers and the whole kiting coefficient model; TGT config defaults (shoot 5, heal 38.5, spell 28.5, melee 0.75) are tuned to that simplified model — porting OG formulas verbatim while keeping TGT defaults (or vice versa) will misbehave; the plan must pick a consistent pair.
6. TGT wait-for-attack is the one Scope-D subsystem already PORTED-FAITHFUL (and fixes an OG bug); TGT ClipPath's startP-before-cutTo fix should likewise be kept over OG's ordering.
