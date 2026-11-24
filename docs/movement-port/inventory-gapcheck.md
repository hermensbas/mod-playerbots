MISSED-ITEMS REPORT — movement code the six scope inventories would miss
Path shorthand: OG = C:/Users/Admin/git/scratch/cmangos-playerbots/playerbot | VEN = C:/Users/Admin/git/azerothcore-wotlk/modules/mod-cmangosbots/src/playerbot | TGT = C:/Users/Admin/git/main/azerothcore-wotlk/modules/mod-playerbots/src

RECURRING UNMARKED DEVIATION PATTERNS (apply to many entries below; verify each during port):
- P1: cmangos `IsWithinLOS(x,y,z, true)` (ignore-M2 flag) → VEN `IsWithinLOS(x,y,z)` with the flag silently dropped. Seen in FleeManager.cpp:24/117, Stances.cpp:38, PositionAction.cpp:160. TGT also drops it and often drops `+GetCollisionHeight()` too (TGT Stances.cpp:42).
- P2: cmangos `Unit::GetAttackDistance` → VEN `ToCreature() ? ...GetAttackDistance(bot) : <fallback>` with INCONSISTENT fallbacks: 0.0f in FleeManager.cpp:186 vs 5.0f in WaitForAttackAction.cpp (unmarked; changes flee/wait decisions vs player attackers).
- P3: cmangos TerrainInfo water checks → AC `Map::IsInWater(phaseMask,x,y,z,collisionHeight)`; marked in FleeManager.cpp calculatePossibleDestinations but UNMARKED in isTooCloseToEdge and GoAction (marked there at GoAction.cpp:784).
- P4: `IsInSwimmableWater()` → `IsInWater()` (unmarked, IsMovingValue.h IsSwimmingValue) — any-water vs swimmable-water.

== A. FleeManager (explicitly requested full inventory) ==
FleePoint (class) | OG FleeManager.h:13 | VEN FleeManager.h:13 | NONE | TGT Mgr/Move/FleeManager.h PORTED (ctor takes botAI) | WorldPosition | —
FleeManager ctor | OG FleeManager.h:36 | VEN FleeManager.h:36 | NONE | TGT FleeManager.cpp:11 PORTED (out-of-line) | startPosition defaulting to WorldPosition(bot) | forceMaxDistance default false
FleeManager::calculateDistanceToCreatures | OG FleeManager.cpp:11 | VEN :11 | DEVIATION(unmarked): IsWithinLOS loses ignore-M2 arg (P1) | TGT :21 DIVERGED: the "do not count non-LOS mobs" filter is MISSING entirely | "possible targets no los" value, ServerFacade.GetDistance2d | LOS filter is what keeps bots from fleeing toward walls
intersectsOri (free fn) | OG FleeManager.cpp:32 | VEN :32 | NONE | TGT :44 PORTED (vector not list) | — | —
FleeManager::calculatePossibleDestinations | OG FleeManager.cpp:43 | VEN :43 | ADAPTATION(marked): TerrainInfo→Map::IsInWater; DEVIATION(unmarked): target->IsWithinLOS loses ignore-M2 (P1) | TGT :56 DIVERGED: bot-side `!bot->IsWithinLOS(x,y,z)` is ACTIVE (OG has it commented out), edge check (MoveStyleValue::CheckForEdges + isTooCloseToEdge) MISSING | "current target", MoveStyleValue::CheckForEdges, followDistance/tooCloseDistance config, UpdateAllowedPositionZ | ring/angle scan geometry identical in VEN
FleeManager::isTooCloseToEdge | OG FleeManager.cpp:103 | VEN :103 | ADAPTATION(unmarked but same P3 water swap) | TGT ABSENT | Map water check, followDistance, UpdateAllowedPositionZ | needed by "edge aware" move style
FleeManager::cleanup | OG :125 | VEN :124 | NONE | TGT :126 PORTED | — | —
FleeManager::isBetterThan | OG :135 | VEN :134 | NONE | TGT :136 PORTED | — | sumDistance-max heuristic
FleeManager::selectOptimalDestination | OG :~141 | VEN :139 | NONE | TGT :141 PORTED | — | —
FleeManager::CalculateDestination | OG :153 | VEN :152 | NONE | TGT :154 PORTED | — | —
FleeManager::isUseful | OG :173 | VEN :172 | DEVIATION(unmarked): GetAttackDistance fallback 0.0f for non-creature attackers (P2) | TGT :174 DIVERGED: missing the "has area debuff"(AoE) gate; creature-only loop | "has area debuff"/"self target" value, sqDistance | AoE gate makes bots flee ground effects even when out of attack range

== B. Formations.cpp/h + Arrow.cpp/h (explicitly requested) ==
Formation::GetMaxDistance | OG values/Formations.cpp:18 | VEN :18 | NONE | TGT Ai/Base/Value/Formations.cpp PORTED | sPlayerbotAIConfig.followDistance | —
Formation::IsNullLocation | OG :23 | VEN :23 | NONE | TGT :22 PORTED | — | —
Formation::GetAngle | OG :28 | VEN :28 | NONE | TGT PORTED | GetFollowAngle | —
Formation::GetOffset | OG :42 | VEN :42 | NONE | TGT PORTED | — | —
FollowFormation::GetLocation | OG :55 | VEN :55 | NONE | TGT (inline/GetLocation) PORTED | master pos + angle/offset | —
MoveAheadFormation::GetLocation | OG :71 | VEN :71 | NONE | TGT :50 PORTED | master move prediction | predicts ahead of moving master
MeleeFormation | OG :123 | VEN ~same | NONE | TGT :86 PORTED | GetFollowAngle | —
QueueFormation | OG :132 | VEN ~same | NONE | TGT :94 PORTED | — | —
NearFormation | OG :141 | VEN ~same | ADAPTATION(marked, 1 mark): cmangos Map::GetHitPosition LoS-collision-point call replaced | TGT :102 PORTED-FAITHFUL-ish (uses CheckCollisionAndGetValidCoords) | Map height/hit-position, GetPhaseMask | GetHeight gained phaseMask arg
ChaosFormation | OG :186 | VEN ~same | NONE | TGT :133 PORTED | urand jitter | —
CircleFormation | OG :235 | VEN ~same | NONE | TGT :185 PORTED | — | —
LineFormation | OG :272 | VEN ~same | NONE | TGT :240 PORTED | MoveLine | —
ShieldFormation | OG :308 | VEN ~same | NONE | TGT :279 PORTED | MoveLine/MoveSingleLine | —
FarFormation | OG :371 | VEN ~same | NONE (FOLLOW_MOTION_TYPE check at OG :383) | TGT :345 DIVERGED: added CheckCollisionAndGetValidCoords + GetHoverHeight fallback logic (target-side extension, not in OG) | farDistance/followDistance config, GetMapHeight | TGT farDistance default 20.0f
CustomFormation | OG :407 | VEN ~same | NONE | TGT ABSENT (no "custom" formation; FormationValue::Load lacks it) | "formation position" custom offsets | drop or port decision needed
Formation::GetFollowAngle | OG :459 | VEN :458 | NONE | TGT :419 PORTED | group index math | —
FormationValue ctor/Reset/Save/Load | OG :520/528/538/543 | VEN :~520/527/537/542 | NONE | TGT :522(Load) PORTED minus "custom" branch | all formation classes | chat `formation` command
SetFormationAction::Execute | OG :600 | VEN :599 | NONE | TGT :593 PORTED | FormationValue, ai->Ping | "formation show" pings location
MoveFormation::MoveLine | OG :643 | VEN :642 | NONE | TGT :631 PORTED | MoveSingleLine | —
MoveFormation::MoveSingleLine | OG :671 | VEN :670 | NONE | TGT :661 PORTED | Map GetHeight(phaseMask) | —
ArrowFormation::GetLocationInternal | OG values/Arrow.cpp:7 | VEN :7 | NONE (Arrow.cpp/h VEN diff is churn only; 0 marks) | TGT Ai/Base/Value/Arrow.cpp:12 PORTED | FormationUnit/FormationSlot, "arrow formation" building | —
ArrowFormation::Build | OG Arrow.cpp:60 | VEN :60 | NONE | TGT :65 PORTED | group composition (tank/melee/ranged/heal lines) | —
ArrowFormation::FillSlotsExceptMaster | OG :83 | VEN :83 | NONE | TGT :88 PORTED | — | —
ArrowFormation::AddMasterToSlot | OG :104 | VEN :104 | NONE | TGT :108 PORTED | — | —
FormationSlot::PlaceUnits | OG :125 | VEN :125 | NONE | TGT :128 PORTED | UnitPlacer | —
FormationSlot::Move | OG :159 | VEN :159 | NONE | TGT :159 PORTED | — | —

== C. Stances.cpp/h (explicitly requested) ==
Stance::GetTarget | OG values/Stances.cpp:10 | VEN :10 | NONE | TGT Ai/Base/Value/Stances.cpp PORTED | "current target"/"attack target" values | OG bug preserved everywhere: `if (attackTarget) ai->GetUnit(attackTarget);` return value discarded
Stance::GetLocation | OG :21 | VEN :21 | NONE | TGT :25 PORTED | GetLocationInternal | —
Stance::GetNearLocation | OG :30 | VEN :30 | DEVIATION(unmarked): IsWithinLOS loses ignore-M2 (P1) | TGT :34 DIVERGED: LOS check also drops `z + GetCollisionHeight()` | bot LOS | —
MoveStance::GetLocationInternal | OG :44 | VEN :44 | NONE | TGT :48 DIVERGED: GetObjectBoundingRadius → GetCombatReach | meleeDistance config | reach vs bounding radius changes melee stance distance
NearStance | OG :55 | VEN ~:55 | NONE (ifdef collapse + GetSource rename only) | TGT :70 PORTED | "behind" strategy check, group fan-out | —
TankStance | OG :108 | VEN ~same | NONE | TGT :117 PORTED | — | —
TurnBackStance | OG :120 | VEN ~same | NONE | TGT :129 PORTED | ranged-member average angle | —
BehindStance | OG :149 | VEN ~same | NONE | TGT :160 PORTED | group melee fan-out | —
StanceValue ctor/Reset/Save/Load | OG :180/184/190/195 | VEN same | NONE | TGT :199(Load) PORTED | — | —
SetStanceAction::Execute | OG :222 | VEN :222 | NONE | TGT :231 PORTED | ai->Ping ("stance show") | —

== D. Mount subsystem (missed unless "movement actions" scope included it) ==
CheckMountStateAction::Execute | OG actions/CheckMountStateAction.cpp:14 | VEN :15 | ADAPTATION(4 marks: ObjectMgr instance-template mount check; IsOutdoors; Spell.cpp SPELL_AURA_MOUNTED mirror; no Player::CanStartFlyInArea) | TGT Ai/Base/Actions/CheckMountStateAction.cpp:63 DIVERGED WHOLESALE (rewritten: Mount()/Dismount()/TryForms/TryPreferredMount/TryRandomMountFiltered/CalculateMount(Dismount)Distance/ShouldFollowMasterMountState/GetMountType — none of OG's shape) | MountValue, "attackers", masters mount state | mount speed sync w/ group is behavioral core of catch-up while following
CheckMountStateAction::isUseful | OG :250 | VEN :251 | ADAPTATION(marked IsOutdoors) | TGT :149 DIVERGED | CanFly/CanMountInBg | —
CheckMountStateAction::CanFly | OG :314 | VEN :319 | ADAPTATION(marked: no CanStartFlyInArea, uses area/continent rules) | TGT ABSENT (folded into GetMountType) | map/area flags | WotLK cold-weather-flying interplay = least-tested cmangos path
CheckMountStateAction::CanMountInBg | OG :341 | VEN :349 | NONE | TGT ABSENT | BG status | —
CheckMountStateAction::GetAttackDistance | OG :375 | VEN :383 | NONE | TGT ABSENT | enemy proximity | —
CheckMountStateAction::Mount | OG :394 | VEN :402 | ADAPTATION(marked mount-allowed mirror) | TGT :206 DIVERGED | MountValue::GetSpeed, limitSpeedToGroup | —
CheckMountStateAction::UnMount | OG :555 | VEN :563 | NONE | TGT Dismount():245 + CompleteDismount():269 DIVERGED | — | —
MountValue::GetSpeed | OG values/MountValues.cpp:9 | VEN :10 | ADAPTATION(4 marks incl. PlayerSpell::disabled dropped `!false` at :259) | TGT ABSENT (whole MountValues file) | spell effect parsing | —
MountValue::GetMountSpell | OG :88 | VEN :89 | NONE | TGT ABSENT | item template | —
MountValue::IsValidLocation | OG :108 | VEN :109 | ADAPTATION(marked) | TGT ABSENT | instance template, outdoors | —
CurrentMountSpeedValue::Calculate | OG :173 | VEN :182 | ADAPTATION | TGT ABSENT | aura scan | —
MaxMountSpeedValue::Calculate | OG :257 | VEN :266 | NONE | TGT ABSENT | MountListValue | —
MountListValue::Format / MountSkillTypeValue / CanTrainMountValue / CanBuyMountValue | OG :271/283/345/356 | VEN :280/292/354/365 | NONE/ADAPTATION | TGT ABSENT | trainer/vendor values ("mount vendors" AvailableMountVendors also ABSENT) | TGT does mount selection inside CheckMountStateAction instead

== E. Vehicle/boarding ==
EnterVehicleAction::Execute | OG actions/VehicleActions.cpp:11 | VEN :14 (+ EnterVehicle helper :63) | ADAPTATION(marked, WHOLESALE: rewritten onto AC Vehicle API, old cmangos bodies kept under `#if 0` at VEN :103/:173) | TGT Ai/Base/Actions/VehicleActions.cpp:21 PORTED-FAITHFUL to the AC shape (VEN was modeled on TGT's) | "nearest vehicles" value?, MoveNear to vehicle, Unit::HandleSpellClick | boarding movement = MoveNear then board
LeaveVehicleAction::Execute | OG :81 | VEN :87 | ADAPTATION(marked) | TGT :98 PORTED | ExitVehicle | —
PlayerbotAI::CastVehicleSpell/CanCastVehicleSpell/IsInVehicle | OG PlayerbotAI.cpp:5084/5182/5338 | VEN present | ADAPTATION | TGT partially (IsInVehicle exists) | vehicle turret facing/turning (MotionMaster on vehicle base at OG :5233) | vehicle TURNING is movement-adjacent, uses vehicle->GetMotionMaster

== F. BattleGround movement tactics ==
BGTactics class | OG actions/BattleGroundTactics.h (101 lines) | VEN BattleGroundTactics.h | ADAPTATION(marked shims: TEAM_INDEX_NEUTRAL botcompat gap :19, Team→TeamId helper :24, cmangos BG constants→AC GO entries :28, BG event/object storage shims :135) | TGT Ai/Base/Actions/BattleGroundTactics.h:105 DIVERGED (different lineage: `class BGTactics : public MovementAction` but different member set) | MovementAction::MoveTo, BattleBotPath tables, BG APIs | —
Static path tables vPath_* | OG BattleGroundTactics.cpp:1–2311 (138 unique vPath_ names) | VEN identical set (138) | NONE | TGT 104 unique — 34 path tables MISSING/different | — | tables are data; port must diff per-BG (WSG/AB/AV/EY/IC)
BGTactics::wsgPaths | OG :2311 | VEN present | NONE | TGT ABSENT (has wsJumpDown:1443 instead) | — | —
BGTactics::wsgRoofJump | OG :2593 | VEN present | NONE | TGT ABSENT | JumpTo mechanics | —
BGTactics::eotsJump | OG :2648 | VEN present | NONE | TGT eyJumpDown:1512 DIVERGED | — | —
BGTactics::Execute | OG :2696 | VEN present | ADAPTATION | TGT :1560 DIVERGED | — | —
BGTactics::moveToStart | OG :2858 | VEN present | ADAPTATION | TGT :1720 DIVERGED | — | —
BGTactics::selectObjective | OG :2961 | VEN present | ADAPTATION (BG object lookups via GetBGObjectByEntry/GetBGCreatureByEntry — mod-cmangosbots CORE accessors) | TGT :1842 DIVERGED | AC Battleground APIs | VEN depends on core additions that mod-playerbots lacks
BGTactics::moveToObjective | OG :3999 | VEN present | ADAPTATION | TGT :3177 (adds ignoreDist param) DIVERGED | MoveTo | —
BGTactics::selectObjectiveWp | OG :4056 | VEN present | NONE | TGT :3223 DIVERGED | path tables | —
BGTactics::resetObjective | OG :4188 | VEN present | NONE | TGT :3362 | — | —
BGTactics::moveToObjectiveWp | OG :4216 | VEN present | NONE | TGT :3397 | — | —
BGTactics::startNewPathBegin / startNewPathFree | OG :4258/:4316 | VEN present | NONE | TGT :3439/:3500 | — | —
BGTactics::atFlag | OG :4367 | VEN present | ADAPTATION | TGT :3565 | flag GO ids | —
BGTactics::flagTaken/teamFlagTaken/protectFC | OG :4594/4603/4612 | VEN present | NONE | TGT :3981/3990/3999 | — | —
BGTactics::useBuff | OG :4625 | VEN present | NONE | TGT :4026 | — | —
BGTactics::getDefendersCount | OG :4688 | VEN present | NONE | TGT getPlayersInArea:4086 DIVERGED (renamed+re-signatured) | — | —
BGTactics::IsLockedInsideKeep | OG :4719 | VEN present | NONE | TGT :4117 | IC/SotA logic | —
BGTactics::SelectAvObjectiveAlliance/Horde, CheckFlagAv | OG BattleGroundTacticsAV.cpp:102/263/443 | VEN same file | ADAPTATION(2 marks: AC BattlegroundAV node API + GetBGCreatureByEntry core accessor) | TGT ABSENT as separate file (AV logic inline, different) | AC BattlegroundAV | —
ArenaTactics::Execute / moveToCenter | OG :4851/:4898 | VEN present | NONE | TGT :4250/:4317 DIVERGED | — | —
TGT-only: BGTactics::HandleConsoleCommand(Private) | — | — | — | TGT :1277/:1297 (target-side extension) | ChatHandler | exists only in TGT

== G. Follow / stay / guard / flee-to-master actions ==
FollowAction::Execute | OG actions/FollowActions.cpp:13 | VEN :13 | NONE (0 marks; API churn only: WorldLocation members, HasTarget→GetTarget) | TGT Ai/Base/Actions/FollowActions.cpp:25 DIVERGED (rewritten 25–167) | Formations "formation location", MovementAction::MoveTo/Follow | —
FollowAction::isUseful | OG :36 | VEN :36 | DEVIATION check: `IsTaxiFlying`→`IsInFlight` (equiv); GetChaseTarget()->isMoving casing | TGT :167 DIVERGED | "formation location", CanDeadFollow | —
FollowAction::CanDeadFollow | OG :95 | VEN :95 | NONE | TGT :223 PORTED | — | —
StopFollowAction::isUseful | OG :106 | VEN :106 | NONE | TGT ABSENT | "stop follow" trigger pair | reaction-engine stop
FleeToMasterAction::Execute | OG :117 | VEN :117 | NONE | TGT FleeToGroupLeaderAction:236 DIVERGED (renamed, simplified, no BOT_TEXT distance chatter, no SetDuration(3000)) | "master target" value, Follow(), reactDistance | —
FleeToMasterAction::isUseful | OG :162 | VEN :162 | NONE | TGT :268 DIVERGED | — | —
StayActionBase::Stay | OG actions/StayActions.cpp:10 | VEN :10 | NONE (FLIGHT_MOTION_TYPE rename, UNIT_STATE_* rename) | TGT StayActions.cpp:13 PORTED (no requester param) | motion master clear chase/follow states, "stay position" | —
StayAction::Execute/isUseful, SitAction::Execute/isUseful | OG :47/53/58/67 | VEN same | NONE | TGT :42/44/64/73 PORTED | — | —
PositionAction::Execute | OG actions/PositionAction.cpp:23 | VEN :23 | NONE | TGT PositionAction.cpp:30 PORTED-ish | PositionValue map | —
MoveToPositionAction::Execute/isUseful | OG :94/108 | VEN same | NONE | TGT :105/:119 PORTED | MoveNear position | —
GuardAction::isUseful | OG :115 | VEN :115 | DEVIATION: GetTarget→BotGetTargetUnit shim; IsWithinLOS loses ignore-M2 at :160 (P1) | TGT ABSENT (GuardStrategy exists but guard pull-to-position isUseful logic missing) | "position" value ("guard") | —
SetReturnPositionAction::Execute/isUseful | OG :146/170 | VEN same | NONE | TGT :126/:151 PORTED | RTSC "see spell" set | —
ReturnAction::isUseful | OG :177 | VEN :177 | NONE | TGT :157 PORTED | — | —
ReturnToStayPositionAction::isPossible | OG :183 | VEN :183 | NONE | TGT :163 PORTED | — | —
ReturnToPullPositionAction::isPossible | OG :205 | VEN :205 | NONE | TGT ABSENT | pull position value | pull-strategy return leg missing

== H. Chat shortcuts (movement mode switches) ==
ReturnPositionResetAction::ResetPosition/SetPosition/PrintStrategies | OG actions/ChatShortcutActions.cpp:10/18/26 | VEN same | NONE | TGT PositionsResetAction (4 fns :14–38) DIVERGED (return+stay split, no PrintStrategies) | PositionValue | —
FollowChatShortcutAction::Execute | OG :35 | VEN :35 | NONE (one `loc.mapid` line churn) | TGT :46 DIVERGED | +follow strategy across states, StopMoving | —
StayChatShortcutAction | OG :95 | VEN same | NONE | TGT :118 PORTED-ish | — | —
GuardChatShortcutAction | OG :115 | VEN same | NONE | TGT ABSENT | guard position set | —
FreeChatShortcutAction | OG :135 | VEN same | NONE | TGT ABSENT | free-move mode | —
WanderChatShortcutAction | OG :151 | VEN same | NONE | TGT ABSENT | WanderStrategy | —
FleeChatShortcutAction | OG :167 | VEN same | NONE | TGT :152 PORTED-ish | — | —
GoawayChatShortcutAction | OG :190 | VEN same | NONE | TGT :177 PORTED-ish | RunawayStrategy | —
GrindChatShortcutAction / TankAttackChatShortcutAction / MaxDpsChatShortcutAction | OG :207/220/237 | VEN same | NONE | TGT :195/:212/:233 PORTED-ish | — | TGT adds MoveFromGroup/Naxx/Bwl shortcuts (target-only)

== I. Go command (master-directed travel) ==
GoAction::Execute | OG actions/GoAction.cpp:22 | VEN :23 | ADAPTATION(4 marks: Corpse include; cmangos Taxi::Map/GetTaxiPathSpline active-taxi destination has NO AC equivalent — functionality dropped at VEN :180; GetPosition out-param; water checks) | TGT GoAction.cpp:19 DIVERGED WHOLESALE — TGT has ONLY Execute; all 9 helpers below ABSENT | TravelMgr destinations, TravelTarget, WorldBuff | VEN taxi-destination gap is a real functional hole
GoAction::TellWhereToGo | OG :500 | VEN :493 | NONE | TGT ABSENT | quest/grind/boss destination lists | —
GoAction::LeaderAlreadyTraveling | OG :527 | VEN ~:520 | NONE | TGT ABSENT | group travel target | —
GoAction::TellHowToGo | OG :532 | VEN ~:525 | NONE | TGT ABSENT | TravelNodePath route description | —
GoAction::TravelTo | OG :607 | VEN ~:600 | NONE | TGT ABSENT | TravelTarget::SetTarget | —
GoAction::MoveToGo | OG :638 | VEN :~631 | ADAPTATION(marked GetPosition) | TGT ABSENT | GO lookup | —
GoAction::MoveToUnit | OG :687 | VEN ~:680 | NONE | TGT ABSENT | — | —
GoAction::MoveToGps / MoveToMapGps | OG :708/:766 | VEN ~same | ADAPTATION(marked water checks :784) | TGT ABSENT | WorldPosition map/gps math | —
GoAction::MoveToPosition | OG :817 | VEN ~same | NONE | TGT ABSENT | PositionValue | —
GoAction::UpdateStrategyPosition | OG :835 | VEN ~same | NONE | TGT ABSENT | guard/stay strategy position rewrite | —

== J. Corpse-run / spirit-healer movement ==
ReviveFromCorpseAction::Execute | OG actions/ReviveFromCorpseAction.cpp:13 | VEN :~13 | ADAPTATION(1 mark: Corpse include) | TGT ReviveFromCorpseAction.cpp:18 DIVERGED | corpse pos, sightDistance | —
FindCorpseAction::Execute | OG :61 | VEN ~:61 | NONE | TGT :77 DIVERGED | MoveTo corpse (OG :208 `MoveTo(mapId,...,false,false)`), MotionMaster Clear at :188 | corpse-run pathing — long-range MoveTo consumer
FindCorpseAction::isUseful | OG :220 | VEN same | NONE | TGT :200 | — | —
SpiritHealerAction::Execute | OG :228 | VEN same | NONE (MotionMaster Clear :320, MoveTo grave :328) | TGT :296 DIVERGED | graveyard table, "nearest npcs" | —
SpiritHealerAction::isUseful | OG :332 | VEN same | NONE | TGT :365 PORTED | ghost flag | —

== K. RTSC (master-drawn movement orders) ==
SeeSpellAction::CreateWps | OG actions/SeeSpellAction.cpp:19 | VEN ~:19 | NONE | TGT SeeSpellAction.cpp:18 PORTED-ish | wp creature summon | visual waypoints
SeeSpellAction::isUseful | OG :44 | VEN ~:44 | NONE | TGT ABSENT | RTSC strategy check | —
SeeSpellAction::Execute | OG :49 | VEN ~:49 | ADAPTATION(3 marks: packet read ReadForCaster→AC dest read; PathFinder from botcompat shim; PathFinder::getArea/getFlags mmap nav-area check has NO AC equivalent — DROPPED at VEN :100) | TGT :34 DIVERGED | CMSG spell-target packet, "RTSC selected" value | VEN nav-area validation gap affects where bots accept move orders
SeeSpellAction::SelectSpell | OG :213 | VEN same | NONE | TGT :147 | — | —
SeeSpellAction::MoveToSpell | OG :223 | VEN same | NONE | TGT :159 DIVERGED (no requester) | Formation offset, MoveTo :264 | —
SeeSpellAction::SetFormationOffset | OG :267 | VEN same | NONE | TGT :180 | — | —
RTSCAction::Execute | OG actions/RtscAction.cpp:10 | VEN :10 (0 marks, churn only) | NONE | TGT RtscAction.cpp:11 PORTED-ish | RTSCValues ("RTSC next spell action" etc.) | —
RTSCValues | OG values/RTSCValues.cpp (diff 0) | VEN identical | NONE | TGT Ai/Base/Value/RTSCValues.cpp present | — | covered enough; listed for completeness
RTSCStrategy | OG generic/RTSCStrategy.cpp (diff 0) | VEN identical | NONE | TGT Ai/Base/Strategy/RTSCStrategy.cpp present | — | —

== L. Dungeon hazard-avoidance movement (ALL ABSENT in target) ==
MoveAwayFromHazard::Execute | OG actions/DungeonActions.cpp:11 | VEN :~11 | NONE (0 marks; searcher API churn) | TGT ABSENT | HazardsValue ("active hazards"), MoveTo, LastMovement | whole subsystem missing in TGT
MoveAwayFromHazard::isPossible / IsHazardNearby | OG :94/:104 | VEN same | NONE | TGT ABSENT | — | —
MoveAwayFromCreature::Execute/isPossible/IsValidPoint/HasCreaturesNearby/IsHazardNearby | OG :120/226/236/251/265 | VEN same | NONE | TGT ABSENT | creature grid search | —
Hazard::GetPosition/IsExpired/IsValid | OG values/HazardsValue.cpp:61/78/83 | VEN :61/78/83 | NONE | TGT ABSENT | GuidPosition | —
AddHazardValue::Set + StoredHazardsValue + HazardsValue | OG HazardsValue.h:7/42/49/59, .cpp:106 | VEN same | NONE | TGT ABSENT | — | —
DungeonStrategy (hazard trigger wiring) | OG generic/DungeonStrategy.cpp (diff 0) | VEN identical | NONE | TGT ABSENT as such (TGT has its own per-dungeon Ai/Dungeon system, different lineage) | DungeonTriggers ("close to hazard") | —

== M. Stuck detection / recovery ==
MoveStuckTrigger | OG triggers/StuckTriggers.h:20 | VEN :~20 | NONE | TGT Trigger/StuckTriggers.cpp:13 DIVERGED (own impl; OG deps missing, see StuckValues) | StuckValues "time since last change"/"distance moved since", AllowActivity | —
MoveLongStuckTrigger | OG StuckTriggers.h:63 | VEN :~65 | ADAPTATION(marked): cmangos MMAP::MMapFactory unloaded-grid stuck check + GetCurrentCell REMOVED (deferred "Phase-8 map/pathfinding port"); duel->startTime rename | TGT :60 DIVERGED | mmap tile loaded query, teleport recovery | the unloaded-grid check is the mmap-loading helper the sweep was asked about — currently a VEN functional gap
CombatStuckTrigger / CombatLongStuckTrigger | OG :147/:188 | VEN same | NONE | TGT :144/:172 DIVERGED | combat time values | —
LeaderIsAfkTrigger | OG :229 | VEN same | NONE | TGT ABSENT | leader AFK flag | —
UnstuckAction::Execute | OG actions/UnstuckAction.cpp:4 | VEN identical (diff 0) | NONE | TGT ABSENT — stuck triggers wired straight to "reset" (CombatStrategy.cpp:39, MaintenanceStrategy.cpp:56); no hearthstone/repop escalation | "reset"/"hearthstone"/"repop" actions | escalation ladder missing in TGT
TimeSinceLastChangeValue / DistanceMovedSinceValue | OG values/StuckValues.h:7/15 (cpp diff 0) | VEN identical | NONE | TGT ABSENT (no "time since last change"/"distance moved since" anywhere) | current-position value snapshots | required by OG stuck triggers
string_sprintf helper | OG StuckTriggers.h:6 | VEN same | NONE | TGT ABSENT | — | trivial

== N. Move-style / free-move / avoid-area values ==
MoveStyleAction::Execute | OG actions/MoveStyleAction.cpp:10 | VEN identical (diff 0) | NONE | TGT ABSENT | MoveStyleValue | chat "move style"
MoveStyleValue (incl. static CheckForEdges) | OG values/MoveStyleValue.cpp/h (diff 0) | VEN identical | NONE | TGT ABSENT | consumed by FleeManager edge check | port with FleeManager
FreeMoveCenterValue | OG values/FreeMoveValues.h:6 | VEN identical | NONE | TGT ABSENT | master/travel anchor | —
FreeMoveRangeValue::Calculate | OG FreeMoveValues.cpp:55 | VEN :55 | NONE | TGT ABSENT | maxFreeMoveDistance, freeMoveDelay config (BOTH ABSENT in TGT config) | scales range by delay-phase
CanFreeMoveValue::CanFreeMove/CanFreeMoveTo/CanFreeTarget/CanFreeAttack/Calculate | OG :81/95/101/109/114 | VEN same | NONE | TGT ABSENT | Formation::IsNullLocation, free move center/range | consumed by ChooseRpgTargetAction.cpp:159, ChooseTravelTargetAction.cpp:103, ChooseTargetActions.cpp:37, PartyMemberValue.cpp:23/132, DebugAction:4978 — cross-scope dependency the travel-consumer scope inherits
OutOfFreeMoveRangeTrigger | OG triggers/RangeTriggers.h:466 | VEN same | NONE | TGT ABSENT | CanFreeMoveValue | drives follow when out of leash
SetAvoidAreaAction::Execute/isUseful | OG actions/SetAvoidAreaAction.cpp:10/57 | VEN same (0 marks) | NONE | TGT ABSENT | avoid-area storage consumed by PathFinder shim | affects nav polygon costs

== O. Follow/wander/range triggers (RangeTriggers.h — verify vs scope 4, listed because TGT gaps are large) ==
FarFromMasterTrigger | OG triggers/RangeTriggers.h:287 | VEN same | NONE | TGT Trigger/RangeTriggers.h:103 PORTED | distance param 12.0f | —
OutOfReactRangeTrigger | OG :310 | VEN same | NONE | TGT :118 DIVERGED: hardcodes 50.0f instead of sPlayerbotAIConfig.reactDistance | — | config decoupling bug in TGT
NotNearMasterTrigger | OG :316 | VEN same | NONE | TGT ABSENT | — | —
UpdateFollowTrigger | OG :327 | VEN same | NONE | TGT ABSENT | drives ACTION_IDLE follow refresh | without it follow never re-fires at idle priority
StopFollowTrigger | OG :372 | VEN same | NONE | TGT ABSENT | reaction engine | —
WanderFarTrigger/WanderMediumTrigger/WanderNearTrigger | OG :392/405/416 | VEN same | NONE | TGT ABSENT | wanderMin/MaxDistance (wanderMinDistance ABSENT in TGT config) | —
WaitForAttackSafeDistanceTrigger | OG :430 | VEN same | NONE | TGT (WaitForAttackTriggers.h exists — DIVERGED) | — | —
EnemyInRangeTrigger | OG :175 | VEN same | NONE | TGT ABSENT (only EnemyIsCloseTrigger variant) | — | —
WithinAreaTrigger::IsActive | OG triggers/WithinAreaTrigger.h:7 | VEN same file | ADAPTATION(marked: lastAreaTrigger semantics documented; sObjectMgr-> lookup; box-rotation math cleaned w/ std::) | TGT Trigger/WithinAreaTrigger.cpp:11 + IsPointInAreaTriggerZone:28 DIVERGED (restructured, logic similar) | LastMovement.lastAreaTrigger set by MovementActions, AreaTrigger.dbc geometry | order dependency: MovementActions must SET lastAreaTrigger or trigger is dead

== P. Misc single-site movement consumers (small but real) ==
ReachAreaTriggerAction::Execute | OG actions/AreaTriggerAction.cpp:8 | VEN :~8 | NONE (0 marks; atEntry->m_mapId renames; MovePoint(mapid,...,FORCED_MOVEMENT_RUN) kept) | TGT AreaTriggerAction.cpp:14 PORTED-ish | raw MotionMaster::MovePoint (NOT MovementAction::MoveTo!) | one of only a few raw MovePoint users — pathless straight-line move
AreaTriggerAction::Execute | OG :54 | VEN same | NONE | TGT :64 PORTED-ish | teleports via trigger | —
TaxiAction::Execute | OG actions/TaxiAction.cpp:9 | VEN :~9 (whole-file EOL diff; semantically NONE) | NONE | TGT TaxiAction.cpp:15 PORTED-ish | LastMovement "last taxi", CMSG_TAXICLEARALLNODES | —
RememberTaxiAction::Execute | OG actions/RememberTaxiAction.cpp:8 | VEN identical (diff 0) | NONE | TGT present | "last taxi" | —
TeleportAction::Execute | OG actions/TeleportAction.cpp:9 | VEN same (EOL churn) | NONE | TGT TeleportAction.cpp:15 PORTED-ish | "nearest game objects no los" portals | —
UseMeetingStoneAction::Execute | OG actions/UseMeetingStoneAction.cpp:16 | VEN :~16 (0 marks; GetSelectionGuid→GetTarget) | NONE | TGT UseMeetingStoneAction.cpp:18 DIVERGED (preserveAuras param) | — | —
SummonAction::Execute/SummonUsingGos/SummonUsingNpcs/Teleport | OG :75/105/123/171 | VEN same (MovementExpired/Clear at :206/:209) | NONE | TGT :60/93/113/164 DIVERGED | motion clear on summonee | —
AcceptSummonAction::Execute | OG :232 | VEN same | NONE | TGT (in header?) verify | — | —
MoveToFishAction::isUseful/Execute + FishAction + UseFishingBobberAction | OG actions/FishAction.cpp:10/26/62/92/138 | VEN same (0 marks) | NONE | TGT ABSENT (TGT FishingAction.cpp is a different-lineage implementation) | water-edge point finding, MoveNear bobber | fishing movement = water-edge micro-positioning
WaitForAttackKeepSafeDistanceAction::Execute/IsEnemyClose | OG actions/WaitForAttackAction.cpp:8/95 | VEN same | DEVIATION(unmarked): GetAttackDistance fallback 5.0f non-creature (P2) | TGT WaitForAttackAction.cpp:92 DIVERGED (simpler, no IsEnemyClose) | safe-distance ring search, wp debug creatures | —
AttackAction::Attack (movement bits) | OG actions/AttackAction.cpp:79 (mm/taxi check :81) | VEN :~79 | ADAPTATION(2 marks; one ADDS an `IsWithinLOSInMap` target-reject branch VEN :218 "mirrors mod_playerbots" — deliberate borrowed behavior, flagged) | TGT AttackAction.cpp DIVERGED | FLIGHT_MOTION_TYPE guard, WorldPosition::currentHeight | VEN LOS-gate is an intentional behavior change vs OG
SwitchToMeleeAction / SwitchToRangedAction | OG actions/CombatActions.cpp:10-31 | VEN same | NONE | TGT CombatActions.cpp present-ish | "behind"/range strategies | movement-stance switch
MoveToSuppressionDeviceAction | OG actions/BlackwingLairDungeonActions.h:24 | VEN same | NONE | TGT ABSENT (TGT has own Raid/Bwl system) | MovementAction::MoveTo | dungeon-specific mover
UseLightwellAction (MoveNear at GenericActions.h:86) | OG actions/GenericActions.h:86 | VEN same | NONE | TGT n/a | MoveNear(go,4.0f) | tiny
DK runeforge MoveTo | OG deathknight/DKActions.cpp:137 | VEN same | NONE | TGT n/a (verify TGT DK) | MoveTo(runeForgePos) | class-action mover
UseItemAction FOLLOW_MOTION_TYPE guards | OG actions/UseItemAction.h:668/745 | VEN same | NONE | TGT verify | motion-type introspection | prevents item use while following
ShamanTriggers inMovement guards | OG shaman/ShamanTriggers.h:62/125/172/229 | VEN same | NONE | TGT verify | GetCurrentMovementGeneratorType | totem drop while moving
ThreatValues fleeing check | OG values/ThreatValues.cpp:144 | VEN same | NONE | TGT verify | FLEEING/TIMED_FLEEING motion types | —
SnareTargetValue::Calculate | OG values/SnareTargetValue.cpp:10 (motion-type switch :34/:54) | VEN :10 (0 marks) | NONE | TGT Ai/Base/Value/SnareTargetValue.cpp PORTED-ish | motion-type introspection | root/snare target pick
CollisionValue::Calculate | OG values/CollisionValue.cpp:13 | VEN :13 | DEVIATION(unmarked): isVisibleFor→CanSeeOrDetect; GetObjectBoundingRadius→GetObjectSize | TGT Ai/Base/Value/CollisionValue.cpp present | grid search | "collision" trigger → move-out
IsMovingValue / IsSwimmingValue | OG values/IsMovingValue.h:7/23 | VEN same | DEVIATION(unmarked P4: IsInSwimmableWater→IsInWater) | TGT IsMovingValue.cpp present | ServerFacade.isMoving/IsUnderwater | —
DistanceValue / InsideTargetValue | OG values/DistanceValue.h:11/107 | VEN same | DEVIATION: GetObjectBoundingRadius→GetObjectSize | TGT DistanceValue.cpp present | — | verify scope 4 covered; listed for the deviation
GameObjectsValue/EntryFilterValue/GuidFilterValue/RangeFilterValue/GoUsable/GoTrapped/GosInSight/GoSClose/HasObjectValue | OG values/GuidPositionValues.h:9-130 | VEN same (0 marks) | NONE | TGT ABSENT (whole file) | GuidPosition | rpg/travel target feeders — cross-check vs scope 4/5

== Q. Generic movement strategies (strategy/generic/) ==
FollowMasterStrategy (Init*Triggers x4, OnStrategyAdded/Removed) | OG generic/FollowMasterStrategy.cpp:7-60+, .h:6 | VEN identical (diff 0) | NONE | TGT Ai/Base/Strategy/FollowMasterStrategy.cpp DIVERGED SEVERELY: only getDefaultActions{follow}, empty InitTriggers — no out-of-free-move-range gate, no check-mount-state chain, no update-follow idle refresh, no stop-follow reaction, no StopMoving on removal | OutOfFreeMoveRangeTrigger, UpdateFollowTrigger, StopFollowTrigger, CheckMountStateAction | THE follow behavior delta for the port
FreeStrategy | OG FollowMasterStrategy.h:30 | VEN same | NONE | TGT ABSENT | free-move values | —
FollowJumpStrategy | OG FollowMasterStrategy.h:45 | VEN same | NONE | TGT ABSENT | jump-while-follow (PlayerbotAI jump state machine) | —
StayStrategy (GetDefault(Non)CombatActions, Init*Triggers, OnStrategyRemoved w/ StopMoving at OG StayStrategy.cpp:20) | OG :8-38 | VEN identical | NONE | TGT StayStrategy.cpp DIVERGED (verify member set) | stay actions | —
SitStrategy | OG StayStrategy.h:20 | VEN same | NONE | TGT verify | — | —
GuardStrategy | OG generic/GuardStrategy.cpp:8/14 | VEN identical | NONE | TGT GuardStrategy.cpp present DIVERGED-lineage | guard position | —
RunawayStrategy | OG generic/RunawayStrategy.cpp:7 | VEN identical | NONE | TGT present | FleeManager via "runaway" action | —
FleeStrategy + FleeFromAddsStrategy | OG generic/FleeStrategy.cpp:7/18 | VEN identical | NONE | TGT FleeStrategy.cpp present | "flee" action, panic triggers | —
PassiveStrategy multipliers | OG generic/PassiveStrategy.cpp:8/13 | VEN identical | NONE | TGT PassiveStrategy.cpp present | PassiveMultiplier | —
WanderStrategy | OG generic/WanderStrategy.cpp:7, .h:7 (inherits FollowMasterStrategy) | VEN identical | NONE | TGT ABSENT | Wander*Triggers (also absent) | free-roam wander missing wholesale
ReturnStrategy | OG generic/ReturnStrategy.cpp:7 | VEN identical | NONE | TGT ReturnStrategy.cpp present | return position | —
KiteStrategy | OG generic/KiteStrategy.cpp:7 | VEN identical | NONE | TGT KiteStrategy.cpp present | "runaway"/snare | —
AvoidMobsStrategy | OG generic/AvoidMobsStrategy.cpp:7/14 | VEN identical | NONE | TGT ABSENT | mob-avoidance pathing (TravelMgr::SetMobAvoidArea nav-area costs) | pairs with the navmesh area-cost system
CombatStrategy | OG generic/CombatStrategy.cpp | VEN ADAPTATION(marked ADDITION): "not facing target"→"set facing" TriggerNode at ACTION_MOVE+7 (borrowed from mod_playerbots because UpdateFaceTarget only re-faces stationary bots) | TGT has its own facing handling | facing while moving | port-plan note: this VEN addition exists BECAUSE of a mod_playerbots behavior — do not double-port
WorldPacketHandlerStrategy movement wiring | OG generic/WorldPacketHandlerStrategy.cpp:75-144 ("activate taxi"→remember taxi+taxi, "taxi done"→taxi, "area trigger"→reach area trigger, "within area trigger"→area trigger, "see spell"→see spell) | VEN identical | NONE | TGT verify equivalent wiring in its WorldPacketHandler strategy | packet triggers | the glue that makes packet-driven movement fire

== R. WorldBuffTravel subsystem (ALL ABSENT in target) ==
WorldBuffTravelApplyAction (ApplyBuffToSelfAndRealPlayers :172, TakeFlightFromMaster :198, TrySummonFarAwayMembers :240, ApplyBuffsForStep :361, AdvanceStep :451, Execute :478) | OG actions/WorldBuffTravelActions.cpp | VEN same file | ADAPTATION(3 marks: no ObjectMgr::DoGOData iteration x2; SetSummonPoint signature) | TGT ABSENT (has unrelated WorldBuffAction) | taxi flight, summons, portals | multi-step travel orchestration (flight+portal+summon)
WorldBuffTravelSetTargetAction :512, DMBuffed :702, DMExited :710, DMCastPortal :719/724, DMTakePortal :755, CastPortal :788/793, TakePortal :824, Finish :857 | OG same file | VEN same | see above | TGT ABSENT | TravelTarget | —
WorldBuffTravelStrategy / Triggers / Values | OG generic/WorldBuffTravelStrategy.cpp, triggers/WorldBuffTravelTriggers.cpp, values/WorldBuffTravelValues.h | VEN same | NONE | TGT ABSENT | — | —

== S. PlayerbotAI-embedded movement machinery (root file — no scope owns it) ==
PlayerbotAI::CanMove | OG PlayerbotAI.cpp:8309 | VEN :8293 | NONE | TGT Bot/PlayerbotAI.cpp:6033 PORTED-ish | taxi/stun/root/falling checks | gate for every MovementAction
PlayerbotAI::StopMoving | OG :8407 | VEN :8391 | ADAPTATION(marked): cmangos hand-built MSG_MOVE_STOP packet sync `#if 0`'d ("deferred to movement/packet phase"); InterruptMoving(true)→StopMoving(); MotionMaster Clear(false,true)+MoveIdle kept | TGT ABSENT as helper (callers use bot->StopMoving() raw) | m_movementInfo flags, transport flag | VEN's dropped packet-sync is a known movement-fidelity gap
PlayerbotAI::Unmount | OG :920 | VEN :926 | NONE | TGT ABSENT | dismount + remount cooldown | —
PlayerbotAI::HandleTeleportAck | OG :1205 | VEN :1213 | NONE (StopMoving + heartbeat) | TGT :785 PORTED-ish | SendHeartBeat, motion top()->Interrupt | teleport-movement flush
PlayerbotAI jump/fall state machine (UpdateAI blocks OG :430-468 & :1871-1887; fields jumpTime/fallAfterJump/jumpDestination; accessors PlayerbotAI.h:618-624; SetFallAfterJump, IsJumping) | OG PlayerbotAI.cpp/h | VEN :385-468 | ADAPTATION(marked: AC MoveFall returns void; `useMoveFall=false` static — simulated falling used instead; SetFallInformation mirror at :301-303 marked) | TGT ABSENT: header has orphan Position jumpDestination + SetJumpDestination (PlayerbotAI.h:579-580), usage commented at :1382 — the WHOLE airborne state machine is missing | MovementActions::JumpTo (scope 1) produces the jump; this consumes/lands it | port MUST include this or JumpTo lands bots in permanent falling (cf. memory: socketless bots IsFalling freeze)
PlayerbotAI::UpdateFaceTarget | OG :647 | VEN present | NONE | TGT n/a (facing handled differently) | throttled ~500ms, stationary-only | reason for VEN CombatStrategy facing addition
isMovingToTransport flag (Set/GetMoveToTransport) | OG PlayerbotAI.h:651-652 | VEN same | NONE | TGT ABSENT | transport boarding path (transport navmesh gap memory) | —
PlayerbotAI::Reset movement resets | OG :1248 (jump reset :~1323 VEN) | VEN :1256 | NONE | TGT Reset exists — verify jump/taxi resets | last movement values | —
Follow-transport stop check | OG :550-574 (inside UpdateAI: stop follow when master on different transport) | VEN same | NONE | TGT verify | GetTransport comparison | subtle order-of-ops: runs before engine tick

== T. ServerFacade movement wrappers ==
GetDistance2d(Unit,WO) / (Unit,x,y) | OG ServerFacade.cpp (ifdef sqrt(DIST_CALC_NONE)) | VEN collapsed to unit->GetDistance2d | DEVIATION(technically NONE numerically — cmangos returned squared w/o sqrt under CMANGOS; verify) | TGT Util/ServerFacade.cpp present | — | distance-comparator basis for ALL movement ranges
isMoving / IsInFront / IsDistance{Less,Greater}[OrEqual]Than | OG ServerFacade.h:78/134/224-227 | VEN same | NONE | TGT partial (no isMoving wrapper) | sD compare epsilon | —
SetFacingTo(Unit,angle,force) | OG ServerFacade.h:229 | VEN same | NONE | TGT ServerFacade.h:106 (Player* variant) DIVERGED | turns while moving (unlike core SetFacingToObject) | facing-during-movement primitive
GetChaseTarget/GetChaseAngle/GetChaseOffset | OG ServerFacade.h:~240-243 | VEN same | NONE | TGT GetChaseTarget only (:114); GetChaseAngle/GetChaseOffset ABSENT | ChaseMovementGenerator introspection | used by FollowAction::isUseful chase re-target logic

== U. RandomPlayerbotMgr / PlayerbotMgr movement (root) ==
RandomPlayerbotMgr::RandomTeleport(bot,locs,hearth,activeOnly) | OG RandomPlayerbotMgr.cpp:2390 | VEN present (19 marks file-wide) | ADAPTATION | TGT Bot/RandomPlayerbotMgr.cpp has own lineage RandomTeleport | teleport cache, level-map | bot placement = movement-system入口
RandomTeleportForLevel :2952 / RandomTeleport(bot) :2984 / RandomTeleportForRpg :4027 / ScheduleTeleport :2119 / PrepareTeleportCache :2723 / PrintTeleportCache :2917 / Refresh :3174 (motion clear at :2642-2665) / LoadNamedLocations :1065 / AddNamedLocation :1099 / GetNamedLocation :1112 / LogPlayerLocation :388 | OG same file | VEN present | ADAPTATION file-wide | TGT diverged-lineage equivalents | — | —
Map memory unload thread | OG RandomPlayerbotMgr.cpp:4558 `boost::thread WorldPosition::unloadMapAndVMaps(mapId)` | VEN verify | — | TGT ABSENT | WorldPosition::unloadMapAndVMaps (scope 3 should own the function; THIS CALLER is the missed piece) | mmap/vmap RAM management
PlayerbotMgr::OnBotLoginInternal movement init | OG PlayerbotMgr.cpp:296-297 (StopMoving + MotionMaster), :484 MovementExpired | VEN present | NONE | TGT verify | — | login-time motion reset

== V. TravelMgr navmesh-cost + spatial helpers (verify vs scopes 2/3/5 — likely missed) ==
TravelMgr::SetMobAvoidArea | OG TravelMgr.cpp:1233 | VEN present (verify) | ADAPTATION expected | TGT ABSENT (grep botSteepTravelCost exists in TGT config — partial analog) | PathFinder area flags, async per-map | writes mob-avoidance costs into navmesh areas; pairs w/ AvoidMobsStrategy + SetAvoidAreaAction
TravelMgr::SetMobAvoidAreaMap | OG :1262 | VEN present | ADAPTATION | TGT ABSENT | PathFinder(mapId,0), FactionTemplate aggro scan | —
TravelMgr::LoadAreaLevels | OG :1182 | VEN present | ADAPTATION (DB API) | TGT has own area-level system (verify) | ai_playerbot_zone_level table | travel destination leveling
WorldPointSquare/WorldSquare containers | OG WorldSquare.h:13+ (.cpp) | VEN WorldSquare.cpp/h present (0 marks) | NONE | TGT ABSENT | used by TravelMgr.h + FishValues.h for fast nearest-destination lookup | performance-critical for destination selection; if scope 2/5 didn't list it, port plan loses the spatial index

== W. Movement config keys (PlayerbotAIConfig) ==
Movement key block | OG PlayerbotAIConfig.h:117-122 (+cpp readers) | VEN identical (movement-key diff EMPTY) | NONE | TGT PlayerbotAIConfig.h:94-100 DIVERGED | — | TGT MISSING: raidFollowDistance, wanderMinDistance, proximityDistance, maxFreeMoveDistance, freeMoveDelay, groupMemberLootDistance(+WithActiveMaster), gatheringDistance(+group variants), groupMemberGatheringDistanceWithActiveMaster. TGT EXTRA (target-only, keep): disableMoveSplinePath, dynamicReactDelay, botSteepTravelCost, guardDistance, botTaxiDelayMin/Max. Shared keys present both sides: sightDistance, spellDistance, reactDistance, grindDistance, lootDistance, shootDistance, fleeDistance, tooCloseDistance, meleeDistance, followDistance, whisperDistance, contactDistance, aoeRadius, rpgDistance, targetPosRecalcDistance (TGT default 0.1f — check OG default), farDistance, healDistance, aggroDistance, walkDistance, wanderMaxDistance, maxWaitForMove, reactDelay, rpgDelay, sitDelay, returnDelay, lootDelay, randomBotTeleportDistance, EatDrinkMin/MaxDistance
LeaderIsAfk/duel timers etc. | see StuckTriggers | — | — | — | — | —

== X. Dev/debug movement tooling (low priority, still movement code) ==
DebugAction motion harness | OG actions/DebugAction.cpp:1537-1580 (MoveFollow/MoveChase test), :1857-1934 (MotionMaster MovePath spline tests x2), :4978 (CanFreeMoveTo probe) | VEN present | ADAPTATION | TGT own DebugAction | raw MotionMaster | useful for port validation, port last

== COVERED-BY-SCOPE CONFIRMATIONS (not re-inventoried) ==
- MovementActions.cpp/h (incl. MovementAction::MoveTo/MoveNear/Follow/Flee/JumpTo, ReachTargetActions.h consumers) — scope 1 (movement actions). NOTE for scope 1 owner: ReachTargetActions.h had a 498-line VEN diff; confirm it was actually inventoried.
- TravelNode.cpp/h, TravelNodePath — scope 2.
- WorldPosition.cpp/h incl. loadMapAndVMap/unloadMapAndVMaps, getPathTo etc., GuidPosition.cpp/h — scope 3 (but confirm GuidPosition ctors and WorldSquare explicitly; WorldSquare listed above as likely missed).
- LastMovementValue.h, PositionValue.cpp/h (VEN diff = member-rename churn only), StuckValues*(listed above because TGT-absent), RangeTriggers core combat-range triggers, TravelTriggers/TravelValues — scope 4 presumed; deltas worth double-checking listed in sections M/N/O.
- ChooseTravelTargetAction, MoveToTravelTargetAction, TravelAction, TravelStrategy (VEN diff 19 — verify), ChooseRpgTargetAction, MoveToRpgTargetAction, RpgAction/RpgSubActions (TGT NewRpg lineage at Ai/World/Rpg/Action — RpgHelper::setFacingTo/setFacing/resetFacing use raw MotionMaster; OG resetFacing ABSENT in TGT RpgHelper) — scope 5. NOTE: their CanFreeMoveValue call sites depend on section N (ABSENT in TGT).
- BotStateActions/Engine/Queue — not movement.

== DEPENDENCY SUMMARY (what this missed-layer needs from other subsystems) ==
1. From MovementActions (scope 1): MoveTo/MoveNear/Follow/Flee/JumpTo primitives are called by ~25 files above (BGTactics, FindCorpse, GoAction, FishAction, SeeSpell, DungeonActions, BWL device, DK runeforge, UseLightwell). Follow() must clear/set FOLLOW_MOTION_TYPE consistently or FollowAction::isUseful/StayStrategy::OnStrategyRemoved misfire. MovementActions must also SET LastMovement.lastAreaTrigger for WithinAreaTrigger to ever fire.
2. From values/triggers (scope 4): "possible targets no los", "current target", "master target", "formation location", "stance", "position" map, "last movement"/"last taxi", "time since last change"+"distance moved since" (StuckValues — TGT-absent), "has area debuff"(self) for FleeManager, "collision", "moving"/"swimming".
3. From travel layer (scopes 2/5): TravelTarget/TravelDestination for GoAction+WorldBuffTravel; TravelMgr area levels; WorldSquare spatial index; nav-area cost writes (SetMobAvoidArea) consumed via PathFinder shim by AvoidMobsStrategy/SetAvoidAreaAction.
4. From core/compat: PathFinder→PathGenerator shim (botcompat.h) — missing getArea/getFlags blocks SeeSpell nav validation and mob-avoid costs; MMAP tile-loaded query — missing in AC shim, blocks MoveLongStuckTrigger grid check; MSG_MOVE_STOP packet sync — deferred in VEN StopMoving; mod-cmangosbots CORE accessors GetBGObjectByEntry/GetBGCreatureByEntry required by BGTactics (mod-playerbots has no such core patch — must be reimplemented module-side).
5. From PlayerbotAI (no scope owns): jump/fall state machine (jumpTime/fallAfterJump/MoveFall-or-simulated-fall + SetFallInformation) is REQUIRED by JumpTo (scope 1), FollowJumpStrategy, BG roof jumps; CanMove/StopMoving/Unmount/HandleTeleportAck are preconditions for every mover; ServerFacade::SetFacingTo + GetChaseAngle/GetChaseOffset for facing-while-moving and chase introspection.
6. Config: the 8 TGT-missing movement keys (esp. maxFreeMoveDistance/freeMoveDelay/wanderMinDistance/raidFollowDistance) must be added before FreeMove/Wander/raid-follow features can be ported; OutOfReactRangeTrigger in TGT must be re-tied to reactDistance.
