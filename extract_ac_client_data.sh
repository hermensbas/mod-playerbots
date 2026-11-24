#!/usr/bin/env bash
# =============================================================================
#  AzerothCore client-data extraction for mod-playerbots.
#  Run from anywhere — output lands in $SERVER_DATA_DIR.
# =============================================================================
set -euo pipefail

# ─── PATHS ──────────────────────────────────────────────────────────────────
WOW_CLIENT_DATA="/home/dev/wow_client_data"
TOOLS_DIR="/home/dev/azerothcore_installer/_server/azerothcore/env/dist/bin"
SERVER_DATA_DIR="$TOOLS_DIR"

# ─── TOGGLES ────────────────────────────────────────────────────────────────
EXTRACT_DBC_AND_MAPS=true
EXTRACT_VMAPS=true
EXTRACT_MMAPS=true

MMAP_THREADS=0           # 0 = auto-detect (each thread uses 1-2 GB RAM)
MMAP_SINGLE_MAP=""       # e.g. "489" for Warsong Gulch only

# Verbatim copy of azerothcore-wotlk/master:src/tools/mmaps_generator/mmaps-config.yaml
# (also the same config the mod-playerbots fork ships).
MMAPS_CONFIG_YAML=$(cat <<'YAML_EOF'
mmapsConfig:
  skipLiquid: false
  skipContinents: false
  skipJunkMaps: true
  skipBattlegrounds: false

  # Path to the directory containing navigation data files.
  # This directory should contain the "maps" and "vmaps" folders,
  # and is also where the "mmaps" folder will be created or located.
  dataDir: "./"

  meshSettings:
    # Here we have global config for recast navigation.
    # It's possible to override these data on map or tile level (see mapsOverrides).

    # Maximum slope angle (in degrees) NPCs can walk on.
    # Surfaces steeper than this will be considered unwalkable.
    walkableSlopeAngle: 60

    # --- Cell Size Calculation ---
    # Many parameters below are defined in "cell units".
    # In RecastDemo, you often work with world units instead of cell units.
    # The actual generator uses (src/tools/mmaps_generator/Config.cpp:28):
    #
    #     cellSize = MMAP::GRID_SIZE / vertexPerMapEdge
    #
    # Where:
    #     MMAP::GRID_SIZE = 533.3333f (the size of one map tile in world units)
    #     vertexPerMapEdge = number of vertices along one edge of the full map grid
    #
    # Example (AC stock):
    #     vertexPerMapEdge = 2000 → cellSize ≈ 533.3333 / 2000 ≈ 0.2667 yd
    #
    # IMPORTANT: when changing vertexPerMapEdge, the per-cell parameters
    # below (walkableHeight, walkableClimb, walkableRadius) must be re-scaled
    # to preserve their world-unit semantics. Doubling vertexPerMapEdge
    # halves cellSize, so cell counts must double to keep the same yd value.
    #
    # To convert a value from cell units to world units (e.g., walkableClimb),
    # multiply by cellSize. For example, a walkableClimb of 6 at 2000 resolution:
    #     6 × 0.2667 ≈ 1.60 yd

    # Minimum ceiling height (in cell units) NPCs need to pass under an obstacle.
    # Controls how much vertical clearance is required.
    # To convert to world units, multiply by cellSize (see "Cell Size Calculation").
    # 6 cells × 0.2667 yd ≈ 1.60 yd — matches WoW player capsule height at
    # 2000 resolution (AC stock). Preserves the 1.60 yd world-unit
    # ceiling-clearance requirement.
    walkableHeight: 6

    # Maximum height difference (in cell units) NPCs can step up or down.
    # Higher values allow walking over fences, ledges, or steps.
    # To convert to world units, multiply by cellSize (see "Cell Size Calculation").
    #
    # Vanilla WotLK uses 6, which allows creatures to "jump" over fences.
    # Classic WotLK uses 4, which forces creatures to walk around fences.
    # 6 cells × 0.2667 yd ≈ 1.60 yd — Vanilla-WotLK step semantics at
    # 2000 resolution. Preserves the 1.60 yd world-unit step. The mmap
    # is shared with every creature, NPC patrol, escort, and quest mob;
    # tightening below stock breaks patrols that cross 1.5y ledges.
    walkableClimb: 4

    # Minimum distance (in cell units) around walkable surfaces.
    # Helps prevent NPCs from clipping into walls and narrow gaps.
    # To convert to world units, multiply by cellSize (see "Cell Size Calculation").
    # 2 cells × 0.2667 yd ≈ 0.53 yd — AC stock world-unit buffer at
    # 2000 resolution. Tested wider (= 0.71y world units): erodes polys
    # near mountains/cliffs so pathfinder routes through surviving
    # (higher/worse) polys → bot climbs mountains.
    walkableRadius: 2

    # Number of vertices along one edge of the entire map's navmesh grid.
    # Higher values increase mesh resolution but also CPU/memory usage.
    # 2000 = AC stock baseline. cellSize ≈ 0.2667 yd.
    vertexPerMapEdge: 2000

    # Number of vertices along one edge of each tile chunk.
    # Must divide vertexPerMapEdge evenly — the generator uses integer
    # division: tilesPerMapEdge = vertexPerMap / vertexPerTile
    # (src/tools/mmaps_generator/Config.cpp:144).
    # A higher vertex count per tile means fewer total tiles,
    # reducing runtime work to load, unload, and manage tiles.
    # 80 = AC stock baseline. 2000 / 80 = 25 tiles per map edge, 625
    # tiles per map (~21y per tile). Lots of small tiles, low per-tile
    # RAM, more seams to stitch across.
    vertexPerTileEdge: 80

    # Tolerance for how much a polygon can deviate from the original geometry when simplified.
    # Higher values produce simpler (faster) meshes but can reduce accuracy.
    # 0.8 (vs the AC stock 1.8 and recast canonical 1.3) keeps polygon
    # edges close to real terrain. Targets "merged step into ramp"
    # simplification artifacts that produce corner-cuts and false NOPATH.
    maxSimplificationError: 0.8

    # You can override any global parameter for a specific map by specifying its map ID.
    # Inside each map override, you can also override parameters per individual tile,
    # identified by a string "tileX,tileY" (coordinates).
    #
    # Overrides cascade: global settings → map overrides → tile overrides.
    # For example:
    #
    # mapsOverrides:
    #   "0":                              # Map ID 0 overrides
    #     walkableRadius: 5               # Override global climb height for entire map 0
    #
    #     tilesOverrides:
    #       "50,70":                      # Tile at coordinates (50,70) on map 0
    #         walkableSlopeAngle: 70      # Override slope angle locally just here
    #         walkableClimb: 4            # Also override climb height for this tile only
    #
    #       "51,71":
    #         walkableClimb: 3            # Override climb height for tile (51,71)
    #
    #       "48,32":
    #         walkableClimb: 1            # Even smaller climb for tile (48,32)
    #
    #   "1":                              # Map ID 1 overrides example
    #     walkableHeight: 8               # Increase clearance for whole map 1
    #
    #     tilesOverrides:
    #       "100,100":
    #         maxSimplificationError: 2.5 # Looser mesh simplification for this tile only
    #
    #       "101,101":
    #         walkableRadius: 1           # Smaller NPC radius here for tight corridors
    #
    # This approach allows very fine-grained control of navigation mesh parameters
    # on a per-map and per-tile basis, optimizing pathfinding quality and performance.
    #
    # All parameters defined globally are eligible for override.
    # Just specify the parameter name and new value in the override section.
    mapsOverrides:
      "562": # Blade's Edge Arena
        walkableRadius: 0 # This allows walking on the ropes to the pillars

      "48": # Blackfathom Deeps
        cellSizeVertical: 0.5334 # ch*2 = 0.2667 * 2 ≈ 0.5334. Reduce the chance to have underground levels.

      "529": # Arathi Basin
        tilesOverrides:
          "30,29": # Lumber Mill
            # Make sure that Fear will not drop players rom cliff -
            # https://github.com/azerothcore/azerothcore-wotlk/pull/22462#issuecomment-3067024680
            walkableSlopeAngle: 45

      "530": # Outland
        tilesOverrides:
          "32,30": # Dark portal
            walkableSlopeAngle: 45 # https://github.com/chromiecraft/chromiecraft/issues/8404#issuecomment-3476012660

  # debugOutput generates debug files in the `meshes` directory for use with RecastDemo.
  # This is useful for inspecting and debugging mmap generation visually.
  #
  # My workflow:
  # 1. Install RecastDemo. I'm building it from the source of this fork: https://github.com/jackpoz/recastnavigation
  # 2. In-game, move your character to the area you want to debug.
  # 3. Type `.mmap loc` in chat. This will output:
  #    - The current tile file name (e.g., `04832.mmtile`)
  #    - The Recast config values used to generate that tile
  # 4. Enable `debugOutput` and regenerate mmaps (preferably just the tile from step 3).
  #    - To regenerate only one tile, delete it from the `mmaps` folder.
  # 5. After generation, you will find debug files in the `meshes` folder, including an OBJ file (e.g., `map0004832.obj`)
  # 6. Copy these debug files to the `Meshes` folder used by RecastDemo.
  #    - RecastDemo expects this folder to be in the same directory as its executable.
  # 7. In RecastDemo:
  #    - Click "Input Mesh" and select the `.obj` file
  #    - Choose "Solo Mesh" in the Sample selector
  # 8. (Optional) Reuse the Recast config values from step 3:
  #    - `cellSizeHorizontal` → "Cell Size"
  #    - `walkableSlopeAngle` → "Max Slope"
  #    - `walkableClimb` → "Max Climb"
  #    - and so on
  # 9. Scroll to the bottom of RecastDemo UI and press "Build" to generate the navigation mesh
  debugOutput: false
YAML_EOF
)

# =============================================================================
# ─── DO NOT EDIT BELOW ──────────────────────────────────────────────────────
# =============================================================================

[ -n "$SERVER_DATA_DIR" ] || { echo "SERVER_DATA_DIR is not set"; exit 1; }
[ -n "$WOW_CLIENT_DATA" ] || { echo "WOW_CLIENT_DATA is not set"; exit 1; }
mkdir -p "$SERVER_DATA_DIR"
cd "$SERVER_DATA_DIR"

# ─── SAFETY: source MPQs are READ-ONLY to this script ──────────────────────
# Resolve both paths to canonical form and refuse to run if the output dir
# is inside the source. Combined with safe_rm() below, this script cannot
# touch any file inside WOW_CLIENT_DATA.
SERVER_DATA_DIR_REAL="$(cd "$SERVER_DATA_DIR" && pwd -P)"
WOW_CLIENT_DATA_REAL="$(cd "$WOW_CLIENT_DATA" && pwd -P 2>/dev/null || echo "$WOW_CLIENT_DATA")"
case "$SERVER_DATA_DIR_REAL/" in
    "$WOW_CLIENT_DATA_REAL"/|"$WOW_CLIENT_DATA_REAL"/*)
        echo "ERROR: SERVER_DATA_DIR ($SERVER_DATA_DIR_REAL) is inside WOW_CLIENT_DATA — refusing." >&2
        exit 1
        ;;
esac

# Refuses to remove anything outside SERVER_DATA_DIR. Resolves the parent
# to absolute path so a symlink inside cwd can't trick us into traversing
# into the source. Use this for every cleanup in this script.
safe_rm() {
    local target="$1"
    local parent_abs base
    parent_abs="$(cd "$(dirname -- "$target")" 2>/dev/null && pwd -P)" || return 0
    base="$(basename -- "$target")"
    local abs="$parent_abs/$base"
    case "$abs/" in
        "$SERVER_DATA_DIR_REAL"/|"$SERVER_DATA_DIR_REAL"/*) ;;
        *)
            echo "REFUSING to rm path outside SERVER_DATA_DIR: $target → $abs" >&2
            exit 1 ;;
    esac
    rm -rf -- "$target"
}

[ "$MMAP_THREADS" -eq 0 ] && MMAP_THREADS=$(nproc 2>/dev/null || echo 4)

echo "Working dir : $(pwd)"
echo "Tools dir   : $TOOLS_DIR"
echo "Threads     : $MMAP_THREADS"
echo "Steps       : maps=$EXTRACT_DBC_AND_MAPS  vmaps=$EXTRACT_VMAPS  mmaps=$EXTRACT_MMAPS"
echo

# ─── Symlink Data/ → MPQ source (only when extracting from client) ──────────
if [ "$EXTRACT_DBC_AND_MAPS" = true ] || [ "$EXTRACT_VMAPS" = true ]; then
    has_mpqs() { find "$1" -maxdepth 1 -iname "*.mpq" -print -quit 2>/dev/null | grep -q .; }

    if has_mpqs "$WOW_CLIENT_DATA"; then
        MPQ_DIR="$WOW_CLIENT_DATA"
    elif has_mpqs "$WOW_CLIENT_DATA/Data"; then
        MPQ_DIR="$WOW_CLIENT_DATA/Data"
    else
        echo "ERROR: no .mpq files in $WOW_CLIENT_DATA" >&2
        exit 1
    fi
    MPQ_DIR="$(cd "$MPQ_DIR" && pwd)"

    # Symlink only — refuse to clobber an existing real directory.
    if [ -e Data ] && [ ! -L Data ]; then
        echo "ERROR: Data/ exists in $(pwd) but is not a symlink" >&2
        exit 1
    fi
    ln -sfn "$MPQ_DIR" Data
    echo "Data/ → $MPQ_DIR"
fi

# ─── STEP 1: DBCs + Maps ────────────────────────────────────────────────────
if [ "$EXTRACT_DBC_AND_MAPS" = true ]; then
    echo
    echo "[1/3] Extracting DBCs + Maps..."
    # Clean slate — map_extractor refuses to run if these dirs already exist.
    safe_rm dbc
    safe_rm maps
    safe_rm Cameras
    # -e 7 = bitfield MAP(1)|DBC(2)|CAMERA(4) — extract everything.
    # The old "-e 2" was DBC-only and skipped maps + cameras entirely.
    "$TOOLS_DIR/map_extractor" -e 7 -f 0
    if [ ! -d maps ] || [ -z "$(ls -A maps 2>/dev/null)" ]; then
        echo "ERROR: map_extractor finished but maps/ is empty — check its output above" >&2
        exit 1
    fi
fi

# ─── STEP 2: VMaps ──────────────────────────────────────────────────────────
if [ "$EXTRACT_VMAPS" = true ]; then
    echo
    echo "[2/3] Extracting VMaps..."
    # Clean slate — vmap4_extractor refuses to run if these dirs already exist.
    safe_rm Buildings
    safe_rm vmaps
    "$TOOLS_DIR/vmap4_extractor" -l -d ./Data
    mkdir -p vmaps
    "$TOOLS_DIR/vmap4_assembler" Buildings vmaps
    safe_rm Buildings
    if [ ! -d vmaps ] || [ -z "$(ls -A vmaps 2>/dev/null)" ]; then
        echo "ERROR: vmap4_assembler finished but vmaps/ is empty — check output above" >&2
        exit 1
    fi
fi

# ─── STEP 3: MMaps ──────────────────────────────────────────────────────────
if [ "$EXTRACT_MMAPS" = true ]; then
    if [ ! -d maps ]; then
        echo "ERROR: maps/ missing in $(pwd) — run with EXTRACT_DBC_AND_MAPS=true once" >&2
        exit 1
    fi
    if [ ! -d vmaps ]; then
        echo "ERROR: vmaps/ missing in $(pwd) — run with EXTRACT_VMAPS=true once" >&2
        exit 1
    fi

    echo
    echo "[3/3] Generating MMaps... (do not interrupt)"
    printf '%s\n' "$MMAPS_CONFIG_YAML" > mmaps-config.yaml

    # Wipe any existing tiles before regenerating. Mixed tiles from
    # previous runs (different cellSize / verticesPerTileEdge / etc.)
    # would otherwise be silently kept and mixed with new ones,
    # producing a corrupt navmesh. Clean slate every mmap run.
    safe_rm mmaps
    mkdir -p mmaps

    # Workaround: some mmaps_generator builds write a few tiles to /mmaps
    # via an absolute path. Pre-create it so the writes don't fail, then
    # fold the strays into our local mmaps/ at the end.
    sudo rm -rf /mmaps
    sudo mkdir -p /mmaps && sudo chmod 777 /mmaps

    CMD=("$TOOLS_DIR/mmaps_generator" --config mmaps-config.yaml --threads "$MMAP_THREADS")
    [ -n "$MMAP_SINGLE_MAP" ] && CMD+=("$MMAP_SINGLE_MAP")

    START=$(date +%s)
    "${CMD[@]}"
    ELAPSED=$(( $(date +%s) - START ))

    if compgen -G "/mmaps/*.mmtile" >/dev/null; then
        cp /mmaps/*.mmtile mmaps/ && rm -f /mmaps/*.mmtile
    fi

    echo
    echo "MMap done in $((ELAPSED / 60))m $((ELAPSED % 60))s"
    echo "Tiles: $(ls mmaps/*.mmtile 2>/dev/null | wc -l)"
fi

echo
echo "Done. Restart worldserver to pick up changes."
