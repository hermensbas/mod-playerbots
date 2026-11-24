Aligned:

Bot 50° cap + water 10× bias at engine filter level (covers every path query)
PathFinder reuse + Clear() per step
Underwater path-extension + dispatch fixup
GetFullPath mmap-probe-first
BG gating
ClipPath (LOS + level+5)
Inactive-bot teleport (with self-bot carve-out — intentional)
masterWalking
Pre-dispatch state cleanup
setPath before mutations
needsLongPath + cross-map gate
StopMoving short-stop
10% reuse + regression guard
WaitForReach formula
Stateless re-resolve
walkDistance config
Differ (blocked on infrastructure):

Hazard avoidance (GeneratePathAvoidingHazards) — needs hazard system
Out of scope:

Flying / transports / vehicles
