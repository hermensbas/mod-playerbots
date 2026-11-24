All evidence gathered and verified against the working tree (clean — committed state = current form) and against OG/vendored references. Final ledger follows.

SCOPE F — TARGET DIVERGENCE LEDGER (branch feature/new_rpg_and_nav_5_bash, commits 4b129606..HEAD inclusive, 30 entries, oldest first)

All TARGET paths relative to C:/Users/Admin/git/main/azerothcore-wotlk/modules/mod-playerbots/. OG = C:/Users/Admin/git/scratch/cmangos-playerbots/playerbot/. VEND = C:/Users/Admin/git/azerothcore-wotlk/modules/mod-cmangosbots/src/playerbot/.

---

**0. 4b129606 — feat(Core/Travel): Stateless per-tick movement resolution for RPG travel (BASE, inclusive)**
- WHAT: Foundational port commit. Replaced plan-and-replay RPG travel executor with per-tick re-resolution: MoveFarTo → MoveTo2 (src/Ai/Base/Actions/MovementActions.cpp:4017, overload :4226) → ResolveMovePath (:3626) → DispatchMovement (:3893); path reuse via lastMove.lastPath + makeShortCut; getRoute/getFullPath unification in src/Mgr/Travel/TravelNode.cpp (getRoute :1830, getFullPath :2118, GetFullPath plan-variant :2064); special legs (area trigger, static portal, transport, flight) via server-side synthesized triggers; Z-validated recovery teleports gated on no-player-watching; single-node leaf routes; no runtime link rebuilds (shared-lock data-race avoidance — degraded links become move-to-node points); removed AiPlayerbot.EnableTravelNodes; re-applied quest-POI height resolver, quest-status crash guards, NOPATH reachability gate, flight-status timeout, taxi-node learning. Also src/Ai/Base/Value/LastMovementValue.cpp/.h (lastPath storage), src/Ai/World/Rpg/Action/NewRpgAction.cpp wiring, conf/playerbots.conf.dist.
- DUPLICATES: This IS the port skeleton of OG MoveTo/getFullPath/getRoute (OG MovementActions.cpp:1372-2010, OG TravelNode.cpp:1716-1920). Known deliberate deltas vs OG: dispatches the WHOLE spline via MoveSplinePath instead of OG's one bounded MovePoint per action selected by TravelPath::getNextPoint ≤ maxDist(=sightDistance) + WaitForReach of that hop (OG MovementActions.cpp:2004-2008, 2073); no runtime link-path rebuild (OG rebuilds under shared lock — a latent OG data race); no OG makeShortCut 50% coin-flip (OG :1536 `urand(0,1)`).
- DISPOSITION: This is the object being audited, not a divergence per se. The whole-spline dispatch is the single largest structural deviation from the reference and is the root cause that four later commits (d0139d7c, cfa2cee3, 6c1e0238, 9b1e1067-adjacent behavior) patch around. The full-fidelity port plan must decide once: restore OG per-action bounded dispatch (REPLACE-BY-PORT for the guards below) or keep spline dispatch and FOLD the guards.

**1. 393fe2b0 — Guard the remaining quest-status lookup in the POI progression check**
- WHAT: src/Ai/World/Rpg/Action/NewRpgAction.cpp:398-427 (DoIncompleteQuest): `getQuestStatusMap().find(questId)` instead of `.at()` (throw → crash) + `ChangeToIdle()` when quest vanished mid-state; objectiveIdx range hardened to `>= 0 &&` / `>= QUEST_OBJECTIVES_COUNT &&`.
- WHY: quest rewarded/dropped while DoQuest state still active → std::out_of_range crash; negative objectiveIdx → OOB array read.
- DUPLICATES: nothing in references — NewRpg framework is modpb-only; OG's TravelTarget lifecycle can't dangle a quest this way (destinations re-validated via "need quest objective" conditions, OG ChooseTravelTargetAction.cpp:133).
- DISPOSITION: FOLD. Pure crash guard; must survive in any NewRpg-side code the port keeps.

**2. 8d784428 — Yield to grind at drop-sourced item objectives**
- WHAT: NewRpgAction.cpp:474-484: at a gather-item POI, if `HasNearbyQuestMob(30.0f)` return false (yield tick to grind/attack) before `MoveRandomNear(20.0f)`.
- WHY: drop-sourced item objectives (venom sacs) are kill quests in disguise; under per-tick resolution MoveRandomNear succeeds every tick so the bot roams forever amid the mobs it needs.
- DUPLICATES: OG has no travel-focus attack gate at all — grind/attack strategies always compete by relevance, and OG QuestObjectiveTravelDestination + "need quest objective" naturally sends bots to the mobs. The yield re-creates OG's default behavior inside modpb's gated framework.
- DISPOSITION: REPLACE-BY-PORT (superseded by porting OG's ungated strategy relevance + TravelDestinationPurpose quest objectives); keep as-is until that lands.

**3. 9b1e1067 — Keep the movement commitment window intact while a target exists**
- WHAT: MovementActions.cpp:1551-1573 WaitForReach: added `+ sPlayerbotAIConfig.reactDelay`, commented OUT the globalCoolDown clamp when a target exists.
- WHY: lingering grind target collapsed every movement commitment to 0.5s → constant spline cancel/re-dispatch jitter.
- DUPLICATES: EXACTLY the reference form — OG MovementActions.cpp:2727 has `+ reactDelay` and :2731-2734 has the identical clamp commented out.
- DISPOSITION: REPLACE-BY-PORT (absorbed: a faithful WaitForReach port ships this form natively; nothing extra to preserve).

**4. d0139d7c — Keep routes dense with a null mover + bound raw water dispatches**
- WHAT: (a) MovementActions.cpp:3912-3944 DispatchMovement MovePoint branch: bounded hop — target furthest waypoint within reactDistance, not path end; wait sized to hop (:3948 via cfa2cee3). (b) TravelNode.cpp:1352-1377 TravelNodeRoute::buildPath: null-mover leg repair — incomplete same-map link pathed live into a LOCAL TravelNodePath temporary (no shared-graph mutation) instead of collapsing to bare node centers.
- WHY: (a) NOT_USING_PATH straight-line MovePoint from shallow water toward a far land target air-walks the whole way; (b) bare node-center legs make the spline interpolate straight through air.
- DUPLICATES: (a) duplicates OG's bounded per-action dispatch (TravelPath::getNextPoint ≤ sightDistance per action, OG MovementActions.cpp:1358+2004-2008) — OG can never dispatch a full-length raw hop. (b) OG rebuilds incomplete links at runtime into the SHARED graph (TravelNode.cpp:301 `path = endPos.getPathFromPath(path, bot)` inside TravelNodePath build) — a data race 4b129606 deliberately avoided; the local-temporary repair achieves OG's outcome race-free.
- DISPOSITION: (a) REPLACE-BY-PORT if per-action dispatch is restored, else FOLD. (b) KEEP — documented improvement over the reference's racy rebuild; the port must NOT reintroduce OG's shared-graph mutation under a shared lock.

**5. cfa2cee3 — Never dispatch sparse spline segments; quest-first status picks**
- WHAT: (a) MovementActions.cpp:3955-3993 density guard: truncate spline dispatch at first >50y gap (MAX_SPLINE_SEGMENT :3965), return if <2 pts, wait sized to dispatched prefix. (b) NewRpgBaseAction.cpp:1717-1745 RandomChangeStatus: if autoDoQuests and RPG_DO_QUEST available, pick it deterministically instead of the weighted lottery. (c) TravelNode.cpp:1366-1370: null-mover repair accept tolerance made explicit `isPathTo(localLeg, 2.0f)` (default falls back to targetPosRecalcDistance 0.1y which never passes).
- WHY: (a) sparse tail/degraded link → straight-line air-walk; (b) lottery sent bots grinding/wandering with open quests; (c) repair never applied at default tolerance.
- DUPLICATES: (a) same root as #4a — OG's bounded per-action dispatch makes this impossible; (b) duplicates OG's travel purpose ordering — quest destinations first (OG ChooseTravelTargetAction "future travel purpose"="quest", ChooseTravelTargetAction.cpp:1431, :21); the RandomChangeStatus lottery is a modpb invention with config weights AiPlayerbot.RpgStatusProbWeight; (c) local fix to #4b.
- DISPOSITION: (a) FOLD (keep even if per-action dispatch returns — cheap invariant); (b) REPLACE-BY-PORT (superseded by OG ChooseTravelTargetAction ordered destination selection); note it bypasses RpgStatusProbWeight config silently — document; (c) FOLD with #4b.

**6. 3918c0e1 — Step off filter-excluded polys instead of freezing (unstick-step)**
- WHAT: MovementActions.cpp:4085-4131 MoveTo2 empty-path branch: `ClosestCorrectPoint(10,10)` then raw MovePoint step to nearest walkable poly (bounds now 2y-15y + LOS after later commits), EmitDebugMove "unstick-step".
- WHY: bot standing on a poly its own nav filter excludes (steep ledge it strayed onto) → every resolution fails from that start → permanent freeze.
- DUPLICATES: nothing — OG has no steep-tagged mmaps (steep tagging is this project's extractor extension, cf. memory steep-slope-navmesh-rootcause), so OG bots cannot stand on filter-excluded polys. OG's only analog is "I have no path" abort + TravelTarget retry (OG MovementActions.cpp:1449-1463). ClosestCorrectPoint itself is the ported cmangos primitive.
- DISPOSITION: KEEP — documented extension required by steep-tagged-mmaps handling.

**7. 558219d5 — Resolve turn-in height from the quest ender's spawn**
- WHAT: NewRpgAction.cpp:602 (DoCompletedQuest calls ResolveQuestTurnInDestZ); NewRpgBaseAction.cpp:1500-1516 ResolveQuestTurnInDestZ + :1521-1560 NearestQuestSpawnZ (factored from ResolveQuestPOIDestZ, 100y 2D-nearest spawn decides Z); NewRpgBaseAction.h:89-92.
- WHY: top-down terrain probe targets the ground under treehouse/tower quest enders; spawn Z routes the walk up the ramp.
- DUPLICATES: OG needs none of this — OG travel destinations ARE spawn positions (TravelDestination points come from creature/GO spawns, TravelMgr), never terrain-probed POI centroids. The modpb POI-centroid + Z-probe pipeline is the invention being patched.
- DISPOSITION: REPLACE-BY-PORT (superseded by OG spawn-based TravelDestinations); FOLD if the port keeps POI-centroid destinations for NewRpg.

**8. 48ed86ac — Undirected reachability components so node routes resolve**
- WHAT: TravelNode.cpp:3470-3535 PrecomputeReachability: components computed over UNDIRECTED edge closure (reverseLinks map :3481, reverse expansion :3521); plus LOG_DEBUG partial-probe line :2184 in getFullPath.
- WHY: one global-visited BFS over one-directional links is order-dependent → singleton components → hasRouteTo false negatives → route selection falls to raw probes that walk into terrain.
- DUPLICATES: YES — OG has no PrecomputeReachability at all; OG hasRouteTo is a lazy per-source DIRECTED closure cached per node (OG TravelNode.h:202: `if (routes.empty()) for (auto mNode : getNodeMap(...)) routes[mNode]=true`), which is direction-correct with no false negatives or positives. modpb's PrecomputeReachability was the broken invention; this commit fixes it with a cheaper approximation (benign false positives, A* is ground truth).
- DISPOSITION: REPLACE-BY-PORT (superseded by OG TravelNode::hasRouteTo lazy per-source route map via getNodeMap). If precompute is retained for perf, the undirected form is correct-enough — document the false-positive tradeoff.

**9. bd3dcda5 — Hardcode the 50y travel-node threshold + route-resolution diagnostics**
- WHAT: MovementActions.cpp:3650-3660 ResolveMovePath: `constexpr float TRAVELNODE_THRESHOLD = 50.0f` replaces `sPlayerbotAIConfig.sightDistance` gate; :3667-3691 "node-route"/"partial-fallback"/"no-route" EmitDebugMove diagnostics (endGap < 10% heuristic).
- WHY: gating on sightDistance ties routing to a combat key deployments raise freely; raised sightDistance meant multi-hundred-yard trips never consulted the node graph.
- DUPLICATES: OG gates on sightDistance by design (OG MovementActions.cpp:1358 `maxDist = sPlayerbotAIConfig.sightDistance` — "Maximum distance a bot can move in one single action" — and :1415 `totalDistance > maxDist`); in OG that same maxDist ALSO bounds each dispatched hop, so raising it is self-consistent there. In TARGET, where the hop bound is gone, decoupling is defensible.
- DISPOSITION: KEEP the decoupling but FOLD as a dedicated config key during the port (not a buried constexpr); if the port restores OG per-action bounded dispatch, restoring the sightDistance gate is the faithful option. Diagnostics: KEEP.

**10. 2fe0511c — Repair the route A\* heap discipline and expansion cap**
- WHAT: TravelNode.cpp:1636-1712 GetNodeRoute: lazy deletion (skip popped nodes already closed :1650-1652), always push on relax (duplicates over in-place mutation :1703-1707), MAX_A_STAR_EXPLORED 500→3000 (:1640), cap counted after stale-skip.
- WHY: pre-branch modpb code mutated costs in place inside a binary heap → corrupted order → wrong routes and spurious 500-cap failures on legitimate long routes.
- DUPLICATES: OG guarantees correct expansion order differently — full `std::sort` of the open list EVERY iteration (OG TravelNode.cpp:1642) and NO expansion cap; the in-place mutation is harmless under per-iteration sort. modpb's heap was an unmarked deviation that broke it.
- DISPOSITION: FOLD — keep heap+lazy-deletion inside the ported GetNodeRoute (strictly better than OG's O(n log n)-per-pop sort); keep 3000 cap as a documented runaway guard the reference lacks. NOTE: OG GetNodeRoute also seeds hearthstone/teleport PortalNodes and gold-aware link costs (OG TravelNode.cpp:1520-1624) — ABSENT in TARGET's A*; the port plan must restore those (separate scope).

**11. b62aa72f — Per-stage fail counters for route selection diagnostics**
- WHAT: TravelNode.cpp:1813-1827 thread_local s_lastRouteFailReason + GetLastRouteFailReason (TravelNode.h:715); counters pairsNoReach/pairsNoAstar/tailFails/beginFails through getRoute (:1860-2075); consumed by ResolveMovePath debug (MovementActions.cpp:3687-3691).
- WHY: route-selection failures were unattributable in-game.
- DUPLICATES: OG's analog is the "debug move"/"debug travel" TellDebug lines + deadzone.csv logging (OG TravelNode.cpp:1820-1880 hasLog("deadzone.csv"), OG MovementActions.cpp:1421-1446) — different mechanism, same purpose.
- DISPOSITION: KEEP (diagnostic infrastructure; thread_local is sound — bots resolve on map-update threads).

**12. 345a5dd2 — Disambiguate the route whisper when nodes are skipped**
- WHAT: TravelNode.cpp:2137-2147: when probe reaches within accept distance and nodes are skipped, set fail-reason "navmesh-reached spellDist (nodes skipped), probeGap=..., pts=...".
- WHY: false "reached" probes (endpoint near target but wrong side of obstacle) were indistinguishable from failed node routes.
- DUPLICATES: no.
- DISPOSITION: KEEP (diagnostic only).

**13. db58c17d — Faithful mmap path chaining — reuse the corridor across steps**
- WHAT: TravelMgr.cpp:905-955 getPathFromPath: removed the per-step `path.Clear()` an earlier fix added; one PathGenerator threaded through the whole chain so BuildPolyPath prefix-recycling EXTENDS the corridor around obstacles; NAV_WATER cost 20→10. Plus rawProbeGap prefix on fail reason (TravelNode.cpp:2155-2171).
- WHY: clearing re-planned from scratch each step, re-hitting the same ridge; bot walked to the foot and stalled.
- DUPLICATES: THIS IS the reference mechanism — OG getPathFromPath (OG WorldPosition.cpp:1070-1110) threads ONE PathFinder through all 40 attempts, never resets; OG area costs NAV_AREA_WATER=10 (also area 12=5, area 13=20 — the latter two are ABSENT in TARGET; note for port).
- DISPOSITION: REPLACE-BY-PORT/absorbed — restores reference behavior; a faithful getPathFromPath port ships it natively. Port should also carry OG's area-12/13 costs.

**14. 6c1e0238 — Density guard must never truncate the first spline segment**
- WHAT: MovementActions.cpp:3967-3975: density-guard scan starts at i=2 so segment bot→first-waypoint is exempt.
- WHY: truncating the first segment resized to a single point → returned with no movement → hard stall.
- DUPLICATES: n/a (fix to TARGET-only guard #5a).
- DISPOSITION: FOLD with #5a.

**15. 3c6f3ede — Soft-cost steep-edge fallback so bots reach ledge NPCs**
- WHAT: TravelMgr.cpp:900-990: getPathFromPath refactored into runChain lambda (:910); soft-steep fallback (:958-988) — when the bot-filtered chain makes NO progress, retry through a temp Creature whose filter includes NAV_GROUND_STEEP at Detour cost sPlayerbotAIConfig.botSteepTravelCost; config key AiPlayerbot.BotSteepTravelCost (PlayerbotAIConfig.cpp:118-130, .h:99, conf:407-417; default 20→100 by #24).
- WHY: bot filter hard-excludes 50-60deg steep and PathGenerator has no SetIncludeFlags; NPC reachable only along a steep edge = permanent wedge.
- DUPLICATES: nothing — steep tagging doesn't exist in OG/cmangos mmaps or stock AC; entire mechanism is this project's steep-mmaps extension.
- DISPOSITION: KEEP — canonical example of a documented extension the references lack. Port must preserve runChain + fallback + config key.

**16. 40e3a7de — Engage quest-objective mobs while traveling to the POI**
- WHAT: src/Ai/Base/Actions/ChooseTargetActions.cpp:107-152 static IsQuestObjectiveCreature (kill-credit still needed OR drops needed quest item via LootTemplates_Creature.HaveQuestLootForPlayer); :190-199 AttackAnythingAction::isUseful exempts such mobs from the travel-focus gate while travelingToPOI (not while heading to turn-in).
- WHY: gate made bots walk past Webwood Venom spiders — the objective itself.
- DUPLICATES: OG has no travel-focus attack gate (see #2); the gate is modpb's, this is its correction. NOTE: :129 uses `getQuestStatusMap().at(questId)` — same .at() pattern 393fe2b0 fixed elsewhere; status==INCOMPLETE is checked first so the key exists, but it is fragile.
- DISPOSITION: REPLACE-BY-PORT (superseded by removing the gate per OG behavior); FOLD if the gate is retained. Harden the .at() either way.

**17. b3febded — Outnumbered trigger must accumulate foe power across attackers**
- WHAT: src/Ai/Base/Trigger/GenericTriggers.cpp:144 `foePower +=` (was `=`, evaluating only the last attacker).
- WHY: being outnumbered scales with foe count.
- DUPLICATES: matches OG intent for the outnumbered trigger (OG GenericTriggers accumulates); modpb's `=` was an unmarked transcription bug.
- DISPOSITION: FOLD.

**18. 06ebdbd3 — Activate the flee strategy**
- WHAT: src/Bot/Factory/AiFactory.cpp:288 adds "flee" to default non-BG combat strategies.
- WHY: flee triggers existed but strategy was never installed.
- DUPLICATES: OG installs flee by default in its combat engine setup.
- DISPOSITION: FOLD (parity restoration).

**19. bb9cd853 — Engage the closest attacker + de-escalate flee by health**
- WHAT: (a) src/Ai/Base/Value/GrindTargetValue.cpp:71-97: pick CLOSEST alive attacker instead of first hash-ordered entry. (b) GenericTriggers.cpp:129-144: friendPower = 100 + 100*botHealthPct, foe contributions weighted by foe health pct.
- WHY: (a) hash-ordered pick walked bots past near hostiles into packs; (b) whittled-down packs kept re-tripping flee → flee-through-pack loops.
- DUPLICATES: (a) OG GrindTargetValue returns attackers filtered by CanFreeTarget but not distance-sorted — TARGET's closest-first is a reasoned extension; (b) health weighting is a TARGET invention (OG uses static 200 friendPower).
- DISPOSITION: (a) KEEP; (b) KEEP but flag as behavioral deviation from OG's trigger tuning — revisit after the flee stack (FleeManager etc.) is ported at full fidelity.

**20. a2dd85c2 — Port the free-move-range leash for target selection**
- WHAT: GrindTargetValue.cpp:14-42 static CanFreeTarget (anchor=master when following else self; range = wanderMaxDistance [+followDistance when following]); applied :86 (attackers) and :160 (grind candidates). Config AiPlayerbot.WanderMaxDistance=50, AiPlayerbot.GuardDistance (PlayerbotAIConfig.cpp:111-112, conf:405-417); PlayerbotAI::GetRange "wandermax"/"follow"/"guard" (src/Bot/PlayerbotAI.cpp:5421-5428).
- WHY: bots struck out past nearer mobs toward distant camps.
- DUPLICATES: functional subset of OG FreeMoveValues (OG strategy/values/FreeMoveValues.h/.cpp: FreeMoveCenterValue, FreeMoveRangeValue, CanFreeMoveValue::CanFreeTarget :101, wired at OG GrindTargetValue.cpp:48,162 — the exact same two call sites). TARGET omits the value-object plumbing, stay/guard-position anchors, and qualified variants.
- DISPOSITION: REPLACE-BY-PORT (superseding mechanism: OG FreeMoveValues value triple registered in ValueContext.h:344-346); keep the config keys and call sites.

**21. 2243218c — Hunt quest-source mobs in sight instead of camping respawns**
- WHAT: NewRpgBaseAction.cpp:1113-1160: HasNearbyQuestMob now wraps new NearestQuestMob (nearest kill-credit/needed-drop mob within range, phase-checked); NewRpgAction.cpp:490-491: at dry gather POI, MoveWorldObjectTo(nearest quest mob within sightDistance).
- WHY: bots camped a spot for respawns with live objectives in sight.
- DUPLICATES: OG equivalent behavior emerges from TravelTarget + grind (no gate); the explicit hunt is a NewRpg-framework workaround.
- DISPOSITION: REPLACE-BY-PORT (superseded by OG destination/grind interplay); FOLD while NewRpg framework stands.

**22. 51fc5a3f — Reach NPCs on sloped ground + show real goal distance in debug**
- WHAT: (a) NewRpgBaseAction.cpp:73-160 MoveWorldObjectTo: two-pass approach ring — pass 0 flat-only (exclude NAV_GROUND_STEEP), pass 1 allows steep; 8 deterministic angles; findNearestPoly snap (5y extents, 10y max Z drift). (b) MovementActions.cpp:80-250 EmitDebugMove: goalDist "@Ny" (real distance to RPG goal) vs "step Ny".
- WHY: NPC on/beside a slope had its whole 5.5y ring steep-tagged → every angle rejected → bot milled with MoveRandomNear.
- DUPLICATES: OG's approach primitive is MoveNear (angle cycling) with no navmesh-snap two-pass — the flat/steep pass split only makes sense with steep-tagged mmaps (project extension). Snap validation is the ClosestCorrectPoint port.
- DISPOSITION: (a) KEEP (steep-extension handling) — becomes the 4-pass in #26; (b) KEEP (diagnostic).

**23. 236b038e — Approach objects inside interaction range, not at its edge**
- WHAT: NewRpgBaseAction.cpp:97-106: `ringDist = max(distance - 2.0f, 2.5f)` — ring placed ~2y inside requested range.
- WHY: landing exactly at `distance` left bots at the interaction-range limit (drift = out of range) and on worse ground.
- DUPLICATES: OG MoveNear applies its own inward bias (distance shrink per retry).
- DISPOSITION: FOLD into whatever approach function the port keeps.

**24. 0a8eee8a — Trust the node graph — routing policy per design**
- WHAT: Four things. (a) TravelNode.cpp:1987-2060 trust-the-nodes fallback in getRoute: when no end node has a validated mmap tail, ride the graph to the node NEAREST the destination with validated begin leg, empty endPath, re-resolve from there; skipped if bot already within 20y of nearest end node. (b) Flat-only probe: getPathFromPath gains `allowSteepFallback` param (TravelMgr.h:294-301, TravelMgr.cpp:900+); both getFullPath probe sites pass false (TravelNode.cpp:2083, :2130) so a steep-assisted probe can't short-circuit node routing. (c) MoveWorldObjectTo LOS gated to <40y (superseded by #26). (d) unstick-step LOS guard (MovementActions.cpp:4100-4110: step must be IsWithinLOS — ClosestCorrectPoint can snap through walls); BotSteepTravelCost default 20→100 (PlayerbotAIConfig.cpp:130).
- WHY: probes that "reach" over ridges hijacked node routing; unreachable-pocket destinations abandoned the graph entirely.
- DUPLICATES: (a) NO — OG getRoute simply returns empty and OG getFullPath returns an EMPTY movePath (OG TravelNode.cpp:1907-1911 — it does NOT follow a partial probe; note OG also leaks m_nMapMtx shared lock on that branch, a latent OG bug), then OG MoveTo aborts ">maxDist*3 → 'I have no path'" and TravelTarget retries. TARGET's partial-probe follow (from 4b129606, mirroring VENDORED's early-return) and node-approach fallback are both extensions. (b,d) corollaries of the steep extension.
- DISPOSITION: (a) KEEP — needed because AC steep-tagged mmaps create unreachable pockets OG never sees; pairs with #25's guard. (b) KEEP (mandatory guard for #15). (c) superseded. (d) KEEP. Steep cost 100: KEEP.

**25. 2cefaf41 — Only skip the node graph when the probe truly arrives (10y accept)**
- WHAT: TravelNode.cpp:2088 (GetFullPath plan-variant) and :2134 (getFullPath): probe accept tightened `isPathTo(beginPath, 10.0f)` — was spellDistance (28.5y).
- WHY: probe ending 20+y short (behind wall/tree trunk/fence) counted as "reached" and skipped the graph.
- DUPLICATES: DEVIATION from OG, which accepts at spellDistance (OG TravelNode.cpp:1895: "If we can get within spell distance a longer route won't help."). OG tolerates it because per-action bounded movement + TravelTarget retry recover; TARGET's per-tick resolver rode the bad probe forever.
- DISPOSITION: KEEP as long as per-tick resolution stands; if the port restores the OG movement loop, restoring spellDistance is the faithful option — decide once with #0/#9.

**26. 1f4e6c97 — Apply reference-verification verdicts (ClipPath, fallback guard, LOS preference)**
- WHAT: (a) TravelNode.cpp:1078-1092 TravelPath::ClipPath: check `startP == fullPath.end()` BEFORE cutTo (erase invalidates startP; stale iterator == new end() exactly when cut prefix size == remainder size → early return mid-route → whole remaining path dispatched as one unclipped spline, the observed 243y dispatches), then `fullPath.empty()` after. (b) TravelNode.cpp:2002-2012 probe-beats-node guard on trust-the-nodes: only ride when node ends ≥10y closer than the probe already got. (c) NewRpgBaseAction.cpp:108-133 MoveWorldObjectTo: LOS demoted to preference — 4-pass order LOS+flat, LOS+steep, noLOS+flat, noLOS+steep; always produces a destination. (d) MovementActions.cpp:~4005 dispatch debug reports post-truncation endpoint.
- WHY: verification against references.
- DUPLICATES: (a) the BUG is latent in BOTH references — OG TravelNode.cpp:1118-1125 and VENDORED TravelNode.cpp:1125-1132 both call `cutTo(*startP)` before the end() check (OG additionally dereferences a possible end iterator = UB). The fix restores intended semantics. (b) TARGET-only (guards #24a). (c) matches OG intent — OG MoveNear biases by LOS, never hard-rejects.
- DISPOSITION: (a) FOLD — the canonical "correct fix that must survive inside the ported code"; apply to the ported ClipPath, do not copy the reference's broken order. (b) FOLD with #24a. (c) FOLD. (d) KEEP.

**27. 38ae877e — Rotate dried-up gather POIs; make debug reuse/failure visible**
- WHAT: NewRpgAction.cpp:445-515: scoutTimeoutMs (30s, :450) hoisted; dry gather POIs fall through to the POI-rotation scout (>50y different candidate) instead of MoveRandomNear forever (:494-499); MovementActions.cpp:3643 "Resolve reuse" debug; :4131-area "MoveTo2 no-path" debug.
- WHY: dried-up Grellkin camps trapped gatherers; path reuse invisible in debug stream.
- DUPLICATES: OG mechanism = TravelTarget expiry/retire → ChooseTravelTargetAction picks a new destination (possibly next POI for same objective).
- DISPOSITION: REPLACE-BY-PORT (superseded by TravelTarget lifecycle); FOLD while NewRpg framework stands. Debug lines: KEEP.

**28. e49743a6 — No-progress paths are no paths; bounded unstick; unreachable-GO memory**
- WHAT: (a) MovementActions.cpp:3705-3725 ResolveMovePath: result with <2 points, or end within 1y of start while totalDistance >2y, is cleared to EMPTY so MoveTo2 recovery runs (plus "direct"/"direct-none" short-trip debug :3691-3704). (b) :4104-4110 unstick min step raised 0.5y→2y (sub-2y nudges = hover-jitter, block real recovery). (c) NewRpgBaseAction.cpp:62-71 MarkUnreachable/IsMarkedUnreachable (single-slot lastUnreachableGO + 60s TTL, .h:77-85); TryLootQuestGO :746-800 un-commits and skips the unreachable spawn so the picker tries a DIFFERENT one.
- WHY: seed-point "paths" produced endless zero-length splines; bots re-committed the same unreachable mushroom forever.
- DUPLICATES: (a) OG equivalent outcome via movePosition selection + "I have no path" abort — different mechanism, same invariant; (c) OG mechanism = TravelTarget::incRetry / maxRetry → destination retired and another chosen.
- DISPOSITION: (a) FOLD (sane invariant for ported ResolveMovePath). (b) FOLD. (c) REPLACE-BY-PORT (superseded by TravelTarget retry bookkeeping); note single-slot memory only remembers ONE GO — sufficient short-term, inferior to OG's per-target counts.

**29. 92f379e9 — Name the acting action in every movement debug line**
- WHAT: MovementActions.cpp:230-236: EmitDebugMove prefixes getName() (the executing action) before method/generator/status.
- WHY: combat move during a quest read as "moving to Fel Moss".
- DUPLICATES: OG TellDebug lines carry context ad hoc; no structured equivalent.
- DISPOSITION: KEEP (debug whisper system, with #11/#12/#22b/#27-debug: EmitDebugMove at MovementActions.cpp:75, format "[M] | action | method | generator | status | step Ny | target @Ny | extra").

---

**DISPOSITION TOTALS**: KEEP 11 (steep-extension family: #6 unstick, #15 soft-steep+config, #24a/b/d trust-nodes+flat-probe+LOS-guard, #25 10y accept, #9 50y threshold-policy, #4b null-mover repair, #19 combat tuning, debug system #11/#12/#29); FOLD 10 (#1, #5a+#14 density guard, #5c, #17, #18, #23, #26a ClipPath, #26b/c, #28a/b); REPLACE-BY-PORT 9 (#2, #3 WaitForReach, #4a bounded hop, #5b quest-first, #7 turn-in Z, #8 reachability, #13 corridor reuse, #16, #20 leash, #21, #27, #28c — superseding mechanisms: OG ungated strategy relevance, OG WaitForReach form, TravelPath::getNextPoint bounded dispatch, ChooseTravelTargetAction purpose ordering, spawn-based TravelDestinations, TravelNode::hasRouteTo lazy closure, OG getPathFromPath single-PathFinder chain, OG FreeMoveValues, TravelTarget retry/expiry lifecycle).

**DEPENDENCY SUMMARY — what Scope F's ledger needs from other subsystems:**
- From the TravelMgr/TravelTarget scope: whether the port brings OG's TravelTarget lifecycle (incRetry/expiry/ChooseTravelTargetAction) — it is the superseding mechanism for 6 of the 9 REPLACE-BY-PORT entries (#2, #5b, #7, #21, #27, #28c).
- From the dispatch/MoveTo scope: the single structural decision — OG per-action bounded dispatch (getNextPoint ≤ maxDist + WaitForReach per hop) vs TARGET whole-spline MoveSplinePath — flips #4a/#5a/#9/#25 between REPLACE-BY-PORT and FOLD.
- From the node-graph scope: OG GetNodeRoute's PortalNode seeding (hearthstone/teleport spells) and gold-aware costs (OG TravelNode.cpp:1520-1624) are ABSENT in TARGET's A* — restoring them must preserve the lazy-deletion heap fix (#10) and the 3000 cap.
- From the navmesh/extractor scope: every KEEP in the steep family (#6, #15, #24, #25, and MoveWorldObjectTo passes #22/#26c) exists only because of steep-tagged mmaps; if mmap policy changes, re-evaluate the whole family together.
- Types/helpers relied on: WorldPosition::{getPathFromPath, getPathStepFrom, isPathTo, cropPathTo, ClosestCorrectPoint, distance}, TravelPath::{makeShortCut, ClipPath, getNextPoint, getPointPath, getBack, clear}, TravelNodeMap::{getRoute, getFullPath, GetNodeRoute, getNodes, PrecomputeReachability}, PathGenerator::{SetExcludeFlags, SetNavTerrainCost}, PlayerbotAIConfig keys {sightDistance, spellDistance, reactDistance, targetPosRecalcDistance, reactDelay, maxWaitForMove, globalCoolDown, botSteepTravelCost, wanderMaxDistance, guardDistance, followDistance, autoDoQuests, RpgStatusProbWeight}, NewRpgInfo status machine, AI values {"attackers", "possible targets", "last movement"}, LootTemplates_Creature.HaveQuestLootForPlayer, sObjectMgr quest-relation maps.
- Latent reference bugs found while cross-checking (feed to Scope A/B verifiers): OG ClipPath stale-iterator/UB (OG TravelNode.cpp:1118-1125, also in VENDORED :1125-1132); OG getFullPath leaks m_nMapMtx shared lock on the empty-route branch (OG TravelNode.cpp:1901-1911); OG makeShortCut coin-flip `urand(0,1)` (OG MovementActions.cpp:1536) not present in TARGET; TARGET missing OG area-cost 12=5.0/13=20.0 in getPathFromPath; ChooseTargetActions.cpp:129 fragile `.at()` on quest status map.
