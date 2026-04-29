# Tunnel Portal Research

Research target: let NAM.dll create or relink tunnel portals outside the base
game's normal two-ended tunnel drag workflow.

## Current Goal

The intended feature is not just arbitrary path metadata linking. A useful tool
must place tunnel portal occupants that:

- use the correct network-specific tunnel exemplar,
- connect to the surface network at the selected tile,
- update terrain around the portal,
- are inserted into the occupant manager correctly,
- and are linked to a peer tunnel portal for traffic/pathing.

## Confirmed Windows Functions

Windows target: `SimCity 4.exe`, image-base virtual addresses.

| Address | Name | Notes |
| --- | --- | --- |
| `0x00628390` | `InsertTunnelPiece` | Creates a single `0x8A4BD52B` tunnel portal occupant within an initialized `cSC4NetworkTool` placement context. |
| `0x006287d0` | `InsertTunnelPieces` | Consumes tunnel records produced by the drag solver, creates both portal sides, and links paired tunnel occupants. |
| `0x00629210` | `PlaceNetworkOccupants` | Public-ish `cISC4NetworkTool` placement entry. Sets up endpoints, handles adjacency extension, then calls `PlaceNetwork`. |
| `0x00633030` | `ComputeAndStoreCellInfo` | Builds cached cell info for an existing compatible network occupant at a tile. Returns null when the tile has no matching surface network occupant. |
| `0x00633220` | `GetCellInfo` | Existing wrapper in `NetworkStubs.h`; fetches cached cell info or calls `ComputeAndStoreCellInfo`. |
| `0x00639490` | `IdentifyTunnelCells` | High-confidence match. Detects tunnel spans during drag placement, marks portal steps as type `2`, and interior/hidden steps as type `3`. |
| `0x0063b830` | `PlaceNetwork` | Main placement/solver pipeline. Calls tunnel detection, solving, regular occupant insertion, then `InsertTunnelPieces`. |
| `0x00624210` | `cSC4NetworkTool_CreateNetworkOccupant` | Creates network occupant subclasses. The tunnel subclass is selected by `0x8A4BD52B`. |
| `0x006272f0` | `ClearNetworkOccsFromCell` | Mac name match. Called by `InsertTunnelPiece` after setting the portal exemplar; clears/replaces existing network occupants from the target cell. |
| `0x00624fd0` | `PlaceNetworkOccupantById` | Places a single network occupant by ID, but does not obviously use `InsertTunnelPiece`. Probably not sufficient for portals. |
| `0x00647530` | `cSC4NetworkTunnelOccupant_SetOtherEndOccupant` | Stores the linked endpoint at `this + 0x1d0`, AddRef'ing the new endpoint and Release'ing the old one. |
| `0x00647570` | `cSC4NetworkTunnelOccupant_GetOtherEndOccupant` | Returns the linked endpoint from `this + 0x1d0`. |
| `0x006476f0` | `cSC4NetworkTunnelOccupant_ctor` | Initializes the Windows tunnel occupant, including `this + 0x1d0 = nullptr`. |
| `0x007140e0` | `cSC4TrafficSimulator::DoTunnelChanged` | Reads linked endpoints and computes tunnel metadata. Existing NAM hook extends supported network types at `0x00714222`. |
| `0x004adf50` | `cSC4TrafficNetworkMap_InitializeTunnelPaths` | Scans tunnel occupants and initializes path info when the other endpoint is non-null. |

## Key Findings

`InsertTunnelPiece` is the placement primitive we want to preserve. It is not a
standalone factory. It expects:

- a live `cSC4NetworkTool`,
- populated dragged-cell and cell-info state,
- network-specific tunnel exemplar vectors,
- terrain and network world cache services,
- and a valid `cSC4NetworkCellInfo` for the portal cell.

It creates the tunnel occupant, applies tunnel flags, sets the correct exemplar,
clears/replaces surface network occupants in the target cell, adjusts portal
terrain heights, and refreshes the terrain area.

The Mac reference names the cleanup helper as `ClearNetworkOccsFromCell`. This
is an important side effect: a direct portal tool should treat the selected tile
as the portal endpoint cell. Placing into an arbitrary through-road tile may
remove the surface road occupant unless the approach segment is rebuilt or the
selected tile is already the intended transition point.

`GetCellInfo`/`ComputeAndStoreCellInfo` is a promising guard for a custom portal
tool. On Windows, the compute path first asks for the current network occupant
at the target tile and returns null if one is not present. That means a custom
tool can select a surface road/network tile, ask the selected network tool for
cell info, and only proceed when the base game can describe that tile as a
compatible network cell.

`PlaceNetworkOccupantById` can place one network occupant from a manually built
cell descriptor, but it routes through regular network occupant insertion. It
does not appear to run the tunnel-specific portal terrain/exemplar logic. Treat
it as a weak lead unless later evidence shows a tunnel occupant ID path.

The linked endpoint field is nullable, but persistent null-linked tunnel portals
are risky. Some code paths check for null before using the other end, while
others need more review. For example, `DestroyOccupantsAndResolve` references
the other tunnel endpoint in a larger network repair path and may not be safe
for deliberately unpaired portals.

## Likely Patch Shape

Use a custom ViewInputControl only for user intent:

- select network type,
- select first portal location/orientation,
- select second portal location/orientation,
- then commit both endpoints together.

Avoid leaving unpaired portals in the city. The safer first version should place
or relink both endpoints in one operation, then immediately call
`SetOtherEndOccupant` both ways and refresh path info.

The best implementation path is probably not direct object construction. There
are two viable approaches:

1. Enter the normal network tool placement pipeline and intervene around
   `InsertTunnelPieces`, where the game already has a valid `cSC4NetworkTool`
   context and solved cell data.
2. Use a selected `cSC4NetworkTool`, call `GetCellInfo` for each chosen surface
   network tile, then call private `InsertTunnelPiece` directly for each portal
   direction. This may avoid dragging a sacrificial tunnel, while still reusing
   the game's tunnel occupant creation, exemplar, and terrain-cut logic.

The second approach is currently the more interesting path for a single-purpose
portal tool, but it needs side-effect review: undo/cost handling, occupant
replacement behavior, cache invalidation, and path-info refresh after linking.

Direct-call sketch:

- Get `cISC4City` through `cISC4AppPtr`, then
  `cISC4City::GetNetworkManager()`.
- Use `cISC4NetworkManager::GetNetworkTool(networkType, bCreateUnique)` to get
  the selected network tool.
- Convert clicked terrain to a tile, or use `PickOccupant` and query for
  `GZIID_cISC4NetworkOccupant`.
- Call `cSC4NetworkTool::GetCellInfo(z << 16 | x)`.
- Reject the endpoint if `GetCellInfo` returns null or if the cell's
  `networkTypeFlags` does not contain the chosen network.
- Call private Windows `InsertTunnelPiece` at `0x00628390` with a cardinal
  portal direction and tunnel-exemplar sequence index.
- Query both returned occupants for `0x8A4BD52B`, call
  `SetOtherEndOccupant` both ways, then refresh each endpoint's path info using
  the same path-info vfunc `+0x80` pattern seen in `InsertTunnelPieces`.

`InsertTunnelPiece` returns a raw occupant pointer after releasing its local
reference; native `InsertTunnelPieces` immediately uses that pointer because the
occupant manager owns it by then. A DLL implementation should either use it
immediately or `QueryInterface`/`AddRef` before retaining it.

## Research Questions

1. Can we synthesize a two-point `PlaceNetworkOccupants` call that creates valid
   portal cells, then suppress one or both unwanted native portal/linking actions?
2. Can `InsertTunnelPieces` be hooked with a small mode flag to create only the
   requested endpoint portal while still using `InsertTunnelPiece` internally?
3. Where should a custom portal pair be stored between UI selection and commit?
4. Which functions must be notified after relinking or creating endpoints?
5. Which null-handling patches are needed before single unpaired portals are
   allowed to persist?
6. Does direct `InsertTunnelPiece` placement update enough state when called
   outside `InsertTunnelPieces`, or must the tool synthesize more placement
   context first?
7. Does `GetNetworkTool(..., true)` create an isolated tool state that has the
   tunnel exemplar vectors initialized for the chosen network, or do we need the
   active UI tool instance?
8. Does direct placement correctly participate in undo/cost/construction crew
   bookkeeping, or is an explicit surrounding game transaction needed?

## Candidate Intervention Points

`InsertTunnelPieces` contains the pure orthogonal endpoint gate:

- `0x006288d9`: compare start/end Z for east/west tunnel handling.
- `0x006288f7`: compare start/end X for north/south tunnel handling.
- `0x006288f9 -> 0x00628bc6`: return when endpoints are not orthogonal.

It calls `InsertTunnelPiece` twice:

- `0x006289a5`: first side.
- `0x00628a81`: second side.

It links tunnel occupants:

- `0x00628afe`: first `SetOtherEndOccupant` call.
- `0x00628b08`: second `SetOtherEndOccupant` call.

These addresses are promising hook sites, but the exact register/stack state
still needs disassembly-level review before coding.

## Native Pipeline Notes

`PlaceNetworkOccupants` has useful endpoint-extension behavior. When passed a
non-zero network mask, it checks neighboring cells around the requested start
and end points, shifts endpoints outward when compatible existing network
occupants are found, then calls `PlaceNetwork`. That is useful if the portal
tool needs to build or repair a short approach segment before creating the
portal.

This does not by itself solve arbitrary portal pairing. `IdentifyTunnelCells`
and `InsertTunnelPieces` still assume a normal dragged tunnel span and
`InsertTunnelPieces` exits when the two endpoint cells are neither same-row nor
same-column. So the native pipeline is still best for "make valid endpoint
cells and approaches"; direct `InsertTunnelPiece` plus manual linking is still
the better candidate for arbitrary non-orthogonal pairings.

`FindTunnelEnd` (`0x00638770`) is also a confirmed Windows/Mac match. It scans
forward from the detected slope break, stops on blocked/typed steps or placement
limit, and only accepts tunnel candidates long enough to satisfy the internal
distance/grade checks. This supports the current conclusion that trying to
coerce the normal tunnel detector is more cumbersome than placing explicit
portal endpoints.

## Prototype Patch

The first experimental patch is implemented in `src/TunnelPortalTool.cpp` and is
disabled by default behind:

```ini
EnableExperimentalTunnelPortalTool=false
```

Activation is wired to command/message ID `0x4A7B6E30`. When the setting is
enabled, the KEYCFG entry can bind `Control Shift Alt T` to this command.

The prototype view input control:

- asks the user to click two existing surface network tiles,
- infers the network type by asking candidate network tools for `GetCellInfo`,
- rejects mismatched networks,
- infers each portal's cardinal direction from the vector to the other endpoint,
- calls private Windows `InsertTunnelPiece` (`0x00628390`) for both endpoints,
- queries both results as `0x8A4BD52B` tunnel occupants,
- calls `SetOtherEndOccupant` (`0x00647530`) both ways,
- and refreshes each tunnel endpoint's path info using the native vtable pattern
  from `InsertTunnelPieces`.

The first build deliberately does not support persistent unpaired portals.

## 2026-04-25 Recheck: Missed Placement State

The first direct-call prototype created tunnel-looking occupants but did not
produce real commute connectivity. It also produced portals that behaved as if
they were not fully integrated for demolition.

Rechecking the Windows path shows an important missing precondition:
`InsertTunnelPiece` is normally called while `cSC4NetworkTool + 0x50` is set to
placement/commit mode by `PlaceNetwork`. `CreateNetworkOccupant` branches on
that flag. In commit mode it stores the created occupant back into the cell info
and copies the full network flags/edge information into the occupant. Outside
commit mode it follows a preview-like path with much narrower flags.

`InsertTunnelPiece` also calls the occupant manager directly when
`cSC4NetworkTool + 0x48` is present:

- `0x0062848e`: tunnel occupant `AsOccupant` (`cISC4NetworkOccupant` vtable
  slot `+0xc0`).
- `0x00628498`: occupant manager vtable slot `+0x50`, matching
  `cISC4OccupantManager::InsertOccupant`.
- The boolean result is stored at `cSC4NetworkCellInfo + 0x54`.

The prototype now temporarily sets:

- `cSC4NetworkTool + 0x50 = 1` for placement/commit mode,
- `cSC4NetworkTool + 0x20c = 0`, clearing the placement failure code.

`cSC4NetworkTool + 0x208` is passed into the occupant vfunc at
`0x0062848e`, immediately before occupant-manager insertion. A test that forced
this field to `3` made the native insertion flag report success but stopped the
visible portal/tunnel piece from appearing; the log showed the old stage was
`1`. For the direct-call prototype, this field is now treated as a contextual
mode value and preserved rather than forced.

The prototype restores the old placement flag and failure code after both
portal occupants are created and logs the native occupant-manager insertion
result from each cell. The attempted synthetic
`0x99EF1142`/`0xC772BF98` network-occupant-added message path was removed from
the active flow because `CreateMessage(kcRZMessage2Standard, ...)` returned
null in-game, while `InsertTunnelPiece` already reaches the occupant manager
insertion path. If commute connectivity still fails while both insertion flags
are true, the next likely missing piece is not object insertion but
traffic/network-map topology: either the tunnel portal occupant lacks the
correct edge/path data for an arbitrary endpoint, or the traffic map needs a
normal surface approach rebuild around the replaced portal cell.

## 2026-04-27 Recheck: Post-Link Traffic State

The PPC symbolized binary exposed two useful names:

- `cSC4TrafficSimulator::DoMessage(cIGZMessage2*)`
- `cSC4TrafficSimulator::DoConnectionsChanged(...)`

The Windows target confirms the same flow in `cSC4TrafficSimulator::DoMessage`
at `0x007241b0`: for occupant-added/removed messages `0x99EF1142` and
`0x99EF1143` with standard payload `0xC772BF98`, it queries
`GZIID_cISC4NetworkOccupant`, checks network flags, queries
`kcSC4NetworkTunnelOccupant`, calls `DoTunnelChanged` at `0x007140e0`, then
calls the local connectivity rebuild helper at `0x0071a860`.

This explains a likely direct-call failure mode. `InsertTunnelPiece` reaches
the occupant manager before the two custom endpoints are linked. If the native
occupant-added notification is processed immediately or before both
`otherEnd` fields are valid, `DoTunnelChanged` observes a null peer and does
not create tunnel metadata. The prototype now explicitly calls, after linking:

- `DoTunnelChanged(trafficSimulator, firstTunnel, true)`
- `DoTunnelChanged(trafficSimulator, secondTunnel, true)`
- `DoConnectionsChanged(trafficSimulator, x, z, x, z)` for each endpoint

The PPC `cSC4PathInfo::MakeTunnelPaths(cISC4NetworkOccupant*,
cISC4NetworkOccupant*)` also shows that path generation chooses the direction
from endpoint A to endpoint B using cardinal path directions. A brief prototype
that reused this direction for tunnel-piece rotation prepared terrain but did
not show visible portals, so the tool was reverted to its earlier endpoint-vector
rotation heuristic while keeping the post-link traffic notification.

Rechecking PPC/Windows `InsertTunnelPieces` shows that this was still too
loose. The `InsertTunnelPiece` direction argument is not the path-code
`kNextX/kNextZ` numbering. Native tunnel-piece rotation maps the edge toward
the peer endpoint as:

- north: `0`
- east: `1`
- south: `2`
- west: `3`

For example, an east-west tunnel uses `1` for the west-side portal and `3` for
the east-side portal. The prototype now uses that mapping and passes the first
portal's direction toward the second endpoint, with the second portal using the
opposite rotation.

## 2026-04-27 Recheck: Native Commit Visibility Step

The symbolized PPC build made the post-insertion flow clearer. Native
`PlaceNetwork` runs the solved-cell commit setup, calls `InsertTunnelPieces`,
then calls `cSC4NetworkConstructionCrew::MarkOccupantsUsable` on the temporary
placement list. The Mac x86 and Windows target show the same ordering:

- PPC `cSC4NetworkTool::PlaceNetwork` at `0x0069dcac`.
- Mac x86 `cSC4NetworkTool::PlaceNetwork` at `0x004ceb36`.
- Windows `PlaceNetwork` at `0x0063b830`.
- PPC `cSC4NetworkConstructionCrew::MarkOccupantUsable` at `0x001f07d0`.
- Windows equivalent `FUN_00605b30`.

`MarkOccupantUsable` clears network flag `0x4000`, calls
`cISC4NetworkOccupant::AsOccupant()`, then
`cISC4Occupant::SetVisibility(true, true)`, and sets network flag
`0x10000000`. `InsertTunnelPiece` already sets the tunnel exemplar and performs
the terrain update, but a direct call bypasses this later construction-crew
visibility/usable pass because our occupants are not part of the native commit
list. The prototype now mirrors that pass for both directly-created tunnel
occupants after linking/path-info refresh and before the explicit traffic
simulator notification.

The crash at Windows `0x005e16f5` should not currently be treated as flora
evidence. The Ghidra label comes from the function name around a release/cleanup
path, but the observed fault is still more consistent with a stale or
half-integrated occupant pointer being cleaned up after the failed portal
experiment.

## 2026-04-27 Recheck: Model Path Is Not The Blocker

Runtime tracing shows the direct-created road tunnel occupant resolves exemplar
`0x0AD00000` in group `0x2821ED93`, and `GetModelInstances` returns non-null
model instances for every zoom level. The missing visible portal is therefore
unlikely to be a bad exemplar/model-key problem.

The active experiment now installs an `InsertTunnelPiece` inline diagnostic
hook when `EnableExperimentalTunnelPortalTool=true`. The hook logs whether the
call came from native placement or the custom tool, plus the surrounding network
tool vectors and cell-info state. The next check is to compare one normal
in-game road tunnel (`source=native`) against the experimental portal placement
(`source=custom`). The suspected difference is still native solved placement
context, especially dragged/solved/tunnel-record vectors or cell-info buffer
indices that the custom `GetCellInfo` path does not populate.

The first native/custom comparison showed a sharper immediate difference. Native
portal cells entered `InsertTunnelPiece` as empty tunnel endpoint cells:
`networkTypeFlags=0x00080001`, `edgeFlags=0x02000200`, and
`bytes51-57=01 00 01 00 ...`. The custom path entered with ordinary surface
road cells: `networkTypeFlags=0x00000001`, one-sided edge flags, an existing
road occupant, and `byte[0x53]=0`.

The current prototype now prepares the selected endpoint cell to match the
native tunnel-cell shape before calling `InsertTunnelPiece`: it sets
`0x00080000`, sets `byte[0x51]` and `byte[0x53]`, and ORs in the full
orthogonal portal edge pair (`0x02000200` north/south or `0x00020002`
east/west). This is a narrow test: if portals still do not appear, the remaining
missing state is likely the native drag/solve/tunnel-record transaction rather
than a per-cell flag mismatch.

The first visible/functional result showed that using the vector between the
two selected endpoints for portal rotation is too restrictive. Portal rotation
must be independent per endpoint. The prototype now tries to infer each endpoint
direction from the selected surface approach edge: a single approach edge means
the tunnel faces the opposite way from that surface road. This supports mixed
axis pairs such as one north/south portal linked to one east/west portal, and
same-axis pairs where both portals face east, west, or any other combination.
Ambiguous through/intersection tiles still fall back to the old endpoint-vector
heuristic and should get an explicit direction UI later.

Mixed-axis portals exposed a separate traffic/path-info limitation. PPC
`cSC4PathInfo::MakeTunnelPaths` computes a single global direction from endpoint
A's occupied cell to endpoint B's occupied cell, then only extends local path
entries whose path key exit byte matches that global direction. That is fine for
native straight tunnels, but not for independently rotated custom portals.

The prototype now keeps the visual tunnel-piece direction independent, but ORs
the coordinate A-to-B tunnel axis into the prepared cell edges as an additional
path-stitch axis. The intended effect is to let the portal keep its model
rotation while still generating path-info entries that native `MakeTunnelPaths`
can stitch for the paired endpoint.

## 2026-04-27 Recheck: Mixed-Axis Traffic

Visual placement is now working well enough that the remaining problem is
traffic connectivity for mixed-orientation pairs, especially when one portal is
effectively east/west and the other is north/south.

### Why the OR-of-axis approach does not fix mixed-axis

`MakeTunnelPaths` (Windows equivalent of PPC `0x004752a0`, exposed via pathInfo
vtable slot `+0x80` = slot `0x20`) computes a single cardinal direction from
**occupant cell coordinates**: `|A.x − B.x| vs |A.z − B.z|` → dominant axis,
sign → cardinal direction. It then calls `GetPathsFromEntryEdge(direction)` on
occupant A and `GetPathsToExitEdge(direction)` on occupant B. Cell edge flags
are not consulted at this stage; only the coordinate-derived direction matters.

For a mixed-axis pair where A faces south (direction=2) at (10, 10) and B faces
west (direction=3) at (20, 10):

- `MakeTunnelPaths(firstPathInfo, A, B)`: direction = east (dx=10 dominant).
  `GetPathsFromEntryEdge(east)` on A finds nothing — A's path entries are keyed
  south/north (its facing axis). Nothing is stitched for the first portal.
- `MakeTunnelPaths(secondPathInfo, B, A)`: direction = west (dx=-10 dominant).
  `GetPathsFromEntryEdge(west)` on B finds the west-keyed entries — B's facing
  matches the coordinate direction. This portal is accidentally stitched.

ORing additional edge flags into the prepared cell before `InsertTunnelPiece`
does not help here because the path entry keys on the occupant are populated
during `InsertTunnelPiece` from the cell edge shape, while `MakeTunnelPaths`
later queries only by the A-to-B coordinate direction. Those two sources agree
for aligned tunnels and diverge for mixed-axis portals.

### Confirmed fix: hook `MakeTunnelPaths` / `InitTunnelPath` with a facing override

The right intervention is to substitute each portal's actual facing direction
for the coordinate-derived direction inside `MakeTunnelPaths`. The hook:

1. Reads `sCustomPortalFacingOverride` (set by `RefreshTunnelPathInfo` before
   calling pathInfo vtable `0x20`).
2. Replaces the computed direction with the portal's facing (0=N, 1=E, 2=S, 3=W)
   so that `GetPathsFromEntryEdge` queries the correct axis.

The concrete Windows address for `MakeTunnelPaths` is logged as **"InitTunnelPath
concrete address"** by `RefreshTunnelPathInfo` on first custom portal placement.
Copy that value into `sInitTunnelPathHook.address` in `TunnelPortalTool.cpp`.
`InstallDiagnostics` will then install the hook automatically.

### Hook implementation (confirmed, implemented)

Windows `MakeTunnelPaths` function entry: **`0x0053FD70`** (confirmed from log).
Prologue bytes: `83 EC 44 53 55 56 57` (sub esp,0x44 + push ebx,ebp,esi,edi).

The direction byte is stored at `[esp+0x58]` (the param_1 slot reused as a
local after the arg is consumed). After prologue: total ESP adjustment = 0x54,
so param_1 at entry+4 → current `[esp+0x58]`. Five `C6 44 24 58 XX`
instructions write the direction at addresses:

```
0x0053FDB5: C6 44 24 58 00  → 0 (north, default)
0x0053FDBC: C6 44 24 58 02  → 2 (south)  taken when A_coord1 < B_coord1
0x0053FDCF: C6 44 24 58 03  → 3 (west)   taken when A_coord2 < B_coord2
0x0053FDDA: C6 44 24 58 00  → 0 (north)  taken when A_coord1 > B_coord1
0x0053FDE5: C6 44 24 58 01  → 1 (east)   taken when A_coord2 > B_coord2
```

The first coordinate comparison checks `x` (GetOccupiedCell coord_0, probably
east-west position), with the priority order: south > west > north > east. For
our mixed-axis test (A=(5,25), B=(12,18)): A.x=5 < B.x=12 → south (2) for
call 1 where A faces west (3). Neither call produces the correct direction.

Hook site: **`0x0053FDEE`** — the 6-byte `FF 92 C4 00 00 00` = `call [edx+0xC4]`
instruction immediately after all direction assignments. At this point:
- `[esp+0x58]` = computed direction (to be overridden)
- `edx` = vtable of `esi` (set by `8B 16` at 0x0053FDEA)
- `ecx` = `esi` = otherEnd (set by `8B CE` at 0x0053FDEC)
- `eax` = clobbered freely (overwritten by the call's return value)

The installed hook (`Hook_InitTunnelPath`, naked assembly) writes
`sCustomPortalFacingOverride` into `[esp+0x58]` when the override is active,
then jumps to the trampoline which executes the original `call [edx+0xC4]` and
returns to 0x0053FDF4. No registers other than AL are clobbered; EFLAGS
between the override and the call do not matter.

### State wiring

`RefreshTunnelPathInfo` accepts `selfFacing` and stores it in
`sCustomPortalFacingOverride` around the `initTunnelPath` vtable call.
`PlacePortalPair` passes `firstDirection` and `secondDirection` to both calls.
`sInitTunnelPathHook` is installed at `0x0053FDEE` by `InstallDiagnostics`.

### Key addresses (all confirmed)

| Item | Address | Notes |
| --- | --- | --- |
| Windows `MakeTunnelPaths` entry | `0x0053FD70` | pathInfo vtable[0x20] concrete target |
| Hook site (direction patch) | `0x0053FDEE` | `call [edx+0xC4]` = first instruction after direction settled |
| Direction byte location | `[esp+0x58]` inside function | Reused param_1 slot |
| pathInfo `GetPathInfo` vtable slot | `0x31` (byte offset `+0xC4`) | Returns `cSC4PathInfo*` from tunnel occupant |
| pathInfo `InitTunnelPath` vtable slot | `0x20` (byte offset `+0x80`) | = MakeTunnelPaths entry |
| PPC `MakeTunnelPaths` | `0x004752a0` | Reference only |

### 2026-04-28 correction: MakeTunnelPaths uses path direction numbers

The `MakeTunnelPaths` hook must not write the `InsertTunnelPiece` rotation
number directly into `[esp+0x58]`. These are different direction systems:

- tunnel-piece rotation: `0=N`, `1=E`, `2=S`, `3=W`
- path key direction (`kNextX/kNextZ`): `0=W`, `1=N`, `2=E`, `3=S`

The conversion required before setting `sCustomPortalFacingOverride` is:

| Tunnel-piece facing | Path key direction |
| --- | --- |
| `0` north | `1` north |
| `1` east | `2` east |
| `2` south | `3` south |
| `3` west | `0` west |

Failing to convert means the hook fires but asks `MakeTunnelPaths` to stitch
the wrong path keys. That can break even cases where the visible tunnel portal
orientation is correct.

### 2026-04-28 diagnostic: prove the path hook is active

The path-direction conversion alone did not make mixed-orientation traffic
reliable, so the next build records whether the `MakeTunnelPaths` inline hook is
actually consumed by each custom refresh. `Hook_InitTunnelPath` now increments
hit/override counters and records:

- the original coordinate-derived direction byte from `[esp+0x58]`;
- the overridden path-key direction byte written by the custom portal tool.

`RefreshTunnelPathInfo` logs these as `hookHits`, `hookOverrides`,
`originalDirection`, and `overrideDirection`.

Expected result for each custom endpoint refresh:

- `hookHits=1`
- `hookOverrides=1`
- `overrideDirection` equals the converted path-key direction for that portal

If either counter is zero, the hook is not installed or the vtable call is no
longer reaching Windows `MakeTunnelPaths` at `0x0053FD70`. If both counters are
one but the path overlay still shows no tunnel path, the remaining likely
failure is inside `MakeTunnelPaths` after the direction filter: it uses the
local path key from one portal to query `otherPathInfo->GetPath(key, 0)` on the
peer. Mixed-orientation portals may have different full path keys even when the
entry/exit edge direction is correct, so the exact-key lookup can still fail.

The next diagnostic pass scans the path-key map at `pathInfo + 0x1c`, matching
the Windows decompile/disassembly of `MakeTunnelPaths`. The MSVC map header has
one reserved word first, then bucket vector start/end/capacity at `map + 4`,
`map + 8`, and `map + 0xc`. Native `MakeTunnelPaths` only uses start/end to
walk buckets. The hash node layout observed in Windows is:

- `node + 0`: next node
- `node + 4`: `uint32_t key`
- `node + 8`: path point vector

The scanner reports, before and after the native call:

- `matchingKeys`: local path keys where `uint8_t(key) == overrideDirection`;
- `matchingNonEmpty`: matching local keys with at least one path point;
- `peerExactMatches`: matching local keys also present on the peer pathInfo;
- `peerExactNonEmpty`: exact peer matches with at least one path point.

Interpretation:

- `matchingKeys=0`: the portal has no local path entries for the forced
  direction; the problem is occupant/path-info generation for that portal.
- `matchingKeys>0` and `peerExactMatches=0`: the direction override works, but
  `MakeTunnelPaths` cannot stitch mixed portals because it requires identical
  full path keys on both endpoints.
- `peerExactNonEmpty>0` but no overlay/traffic: the native stitch probably
  happens, and the remaining issue is traffic-map tunnel metadata or simulator
  refresh rather than path-key lookup.

Runtime evidence for a west-facing portal linked to a south-facing portal:

- west endpoint local key: `0x01000200`
- south endpoint local key: `0x01000103`

So the path key low byte is the endpoint's path direction, and the next byte is
the opposite direction. Native `MakeTunnelPaths` used the west endpoint key
`0x01000200` to query the south endpoint, but the south endpoint only had
`0x01000103`, so exact lookup failed.

The prototype now hooks the six-byte block at Windows `0x0053FE31`:
`push eax; mov ecx, ebx; lea ebp, [esi+8]`, immediately before the peer
`GetPath(key, 0)` call. While the custom refresh is active, it rewrites `AX` to
the peer endpoint key low word:

```cpp
((peerPathDirection ^ 2) << 8) | peerPathDirection
```

This preserves the high path-type bytes but asks the peer pathInfo for its own
facing-compatible tunnel path key.

After this change, runtime showed the peer lookup hook firing and paths became
visible, but commute connectivity was still absent. That means the path-info
stitch is no longer the only blocker. The next suspect is the traffic simulator
connection rebuild/tunnel metadata pass. The prototype now keeps the native-like
per-endpoint `DoConnectionsChanged(x,z,x,z)` calls and also calls one expanded
bounding rebuild from min endpoint cell to max endpoint cell. This is a cheap
test for whether the simulator needs both endpoints/interior in one dirty
region for custom mixed-axis tunnels.

That still did not create commute connectivity. Because native `MakeTunnelPaths`
only appends one point from the peer path to the local path, the next diagnostic
logs all local path keys plus first/last points for the self key, the rewritten
peer key, and the peer's opposite key. This tests whether the peer rewrite found
the wrong end of the peer portal path.

The point trace showed the stitched path really extends from one custom endpoint
to the other, but the previous lane-pairing attempts only changed the peer
lookup key. That is insufficient. A road tunnel has two reciprocal local path
keys:

- one path goes into the portal;
- one path goes out of the portal.

The logical stitching has to connect outgoing -> incoming, then do the reverse
direction separately:

- first outgoing path -> second incoming path;
- second outgoing path -> first incoming path.

The simple `d`/`d ^ 2` interpretation was still wrong for some orientations:
it connected to the surface entrance side. The current prototype chooses the
keys geometrically from the actual path point endpoints:

- self outgoing key: path whose last point is farthest toward the self tunnel
  mouth, using the tunnel-piece facing direction;
- peer incoming key: path whose first point is farthest toward the peer tunnel
  mouth.

`Hook_InitTunnelPath` overrides the local direction filter to the low byte of
the chosen self key. `Hook_PeerPathLookupKey` rewrites the peer lookup low word
to the chosen peer key's low word. This preserves the observed path-key lanes
instead of assuming a fixed mapping from facing direction to in/out.

The next cheap traffic-simulator test is to call
`DoTunnelChanged(..., false)` for both endpoints after linking, then
`DoTunnelChanged(..., true)` for both endpoints. This clears any stale tunnel
metadata that may have been created by occupant-manager notifications before
the two custom tunnel endpoints were linked.

The current diagnostic pass scans the traffic simulator tunnel-record hash map
at `trafficSimulator + 0xc8` before/after the remove/add calls and after the
connection rebuilds. Windows evidence:

- remove helper `0x00710910` uses table fields at `map + 4`, `map + 8`, and
  node key at `node + 4`;
- add/get helper `0x00713d60` uses the same table and returns the value at
  `node + 8`.

This should show whether `DoTunnelChanged(true)` creates records for either
endpoint key, or whether mixed-orientation portals fail before the tunnel table
is populated.

Runtime result: mixed portals did create two tunnel records under the expected
`x << 8 | z` endpoint keys. So `DoTunnelChanged` is not bailing out. The next
diagnostic dumps the 11 dwords of each matching record value so mixed custom
records can be compared against a working same-axis or native tunnel. If the
payload differs in direction/network/cost fields, the issue is tunnel metadata
content rather than absence of records.

Same-axis and mixed-axis custom records both populate the `trafficSimulator +
0xc8` hash map with peer endpoint data. Example:

- same-axis `(73,82)` <-> `(87,74)`:
  - key `0x4952` value starts with `0x01FE4A57`; low bytes `57 4A` are peer
    `(87,74)` in `x,z` byte order.
  - key `0x574A` value starts with `0x01FE5249`; low bytes `49 52` are peer
    `(73,82)`.
- mixed-axis `(72,82)` <-> `(75,70)`:
  - key `0x4852` value starts with `0x01FE464B`; low bytes `4B 46` are peer
    `(75,70)`.
  - key `0x4B46` value starts with `0x01FE5248`; low bytes `48 52` are peer
    `(72,82)`.

That makes a simple "missing tunnel record" explanation unlikely.

The PPC reference gives the useful names and layout for this payload:

- value byte `0`: peer endpoint x;
- value byte `1`: peer endpoint z;
- value word at byte `2`: tunnel direction mask, observed as `0x01FE`;
- remaining fields are tunnel costs;
- a later word stores the network type.

`DoConnectionsChanged` also does not use only this hash map for tunnel endpoint
graph rebuilds. The PPC reference shows a later pass over a temporary tunnel
connection list. On Windows this is visible at the `trafficSimulator + 0x104`
sentinel pointer. That pass:

- marks the endpoint cell traffic record as tunnel-related;
- ensures base traffic records exist for travel modes 0 and 1;
- walks neighbor directions around both endpoint cells;
- uses `local_14[10]` as a per-network direction mask;
- calls helper `0x0070eab0` to set per-cell connection bits.

Helper `0x0070eab0` has a clean 6-byte prologue
`53 8B 5C 24 08 55` and decompiles as:

```cpp
cell[x,z].mode[travelMode] |= 1 << (toDirection + fromDirection * 4);
```

For travel modes whose transit network is road, it also mirrors a related bit
onto the adjacent cell. This is now hooked diagnostically as
`AddTrafficConnection`, but only logs while the custom tool calls
`DoConnectionsChanged`, and only for the two endpoint cells plus immediate
neighbors.

The custom tool's rebuild list was empty even when the `+0xc8` hash map was
populated. Native placement had previously shown `tool->tunnelRecords=1`, while
custom insertion had `tunnelRecords=0`; this is likely the missing commit-side
side effect. The current experiment splices stack-owned temporary list nodes
around the custom `DoConnectionsChanged` calls:

- node byte `+8`: endpoint A x;
- node byte `+9`: endpoint A z;
- node byte `+10`: endpoint B x;
- node byte `+11`: endpoint B z;
- node word `+20`: tunnel mask, copied from endpoint A's `+0xc8` hash-map
  record when available, otherwise `0x01FE`.

The node payload is seeded from the matching `+0xc8` tunnel hash-map record
before those endpoint/list fields are overwritten. This preserves the native
cost and network payload rather than leaving the temporary list record mostly
zeroed.

One forward node and one reversed node are inserted. A single node proved that
the list is consumed (`AddTrafficConnection` fired), but the resulting edge
insertions were asymmetric: the first endpoint only received adjacent-cell
edges, while the second endpoint received endpoint-side edges. The reversed node
tests whether the native temporary list normally provides both endpoint
orderings for a completed portal pair.

The nodes are unlinked immediately after the rebuild scope exits. If this makes
mixed-axis portals commute, the durable fix is to either reproduce the native
network-tool tunnel-record commit path or keep carefully isolated temporary
records around every explicit custom rebuild.

Seeding the temporary nodes from the hash-map records did not change mixed-axis
commute behavior. The next diagnostic hooks `DoConnectionsChanged` at Windows
`0x0071A867`, after `sub esp, 0x58; mov eax, [esp+0x5c]`, with the six-byte
prologue `56 8B F1 83 C0 FE`. It dumps the first few native tunnel-list records
whenever the list is nonempty. The custom rebuild path suppresses this entry
trace so a regular in-game tunnel can reveal the real temporary record layout
without the custom synthetic records dominating the log.

The first native test after this hook did not produce a nonempty
`DoConnectionsChanged` entry list dump, but it did reconfirm that native
placement has `tool->tunnelRecords=1` before both `InsertTunnelPiece` calls
while custom placement has `tool->tunnelRecords=0`. The next diagnostic dumps
the raw vector at `cSC4NetworkTool + 0xe4` as 16-byte records before/after
`InsertTunnelPiece`. This is likely closer to the native source data for the
temporary traffic rebuild list than the `+0xc8` tunnel hash map.

Native `cSC4NetworkTool + 0xe4` tunnel records are stable construction input,
not a per-portal insertion output. A straight native test showed one 16-byte
record before and after both `InsertTunnelPiece` calls:

- first endpoint `(42,114)` direction `3`;
- second endpoint `(28,114)` direction `1`;
- vector bytes: `4389B13C/438AFBBE/00000002/00000010`.

The two integer fields match dragged/path indices used by native
`InsertTunnelPieces`; the first two dwords are float-like construction
constraints. This does not look like the traffic simulator's final tunnel
connectivity record, and native `InsertTunnelPieces` still only supports
straight same-row/same-column tunnel construction.

The PPC reference exposed the more important traffic-side limitation. The
tunnel hash map is consumed in three places:

- `DoConnectionsChanged`;
- `FloodSubnetwork`;
- `cSC4PathFinder::FindPath`.

In `FindPath`, the tunnel branch checks the current cell's tunnel bit, looks up
`TunnelInfo` by current endpoint key, then jumps directly to the peer endpoint
with:

```cpp
AddTripNode(peerX, peerZ, currentNode->edge, travelMode, tunnelCost, currentNode, ...);
```

That `currentNode->edge` reuse is fine when the two portals are opposite ends of
the same axis: the edge by which the route entered one portal is also the edge
that makes sense when arriving at the peer. It is wrong for mixed-axis custom
tunnels. Example: a route entering an east-facing portal may arrive at a
south-facing peer, but the pathfinder still labels the peer node with the
east/west edge from the source side. Subsequent expansion then reads the wrong
connection nibble from the peer endpoint cell.

`FloodSubnetwork` has the same shape. After a tunnel hash lookup it moves to
the peer endpoint but preserves the same edge/direction state for the peer
network-info check. This can explain why mixed-axis portals can have rendered
paths and tunnel hash records but still fail commute connectivity.

Current experiment:

- record each custom portal pair after directed path pairing is chosen;
- store the arrival edge for each endpoint as the portal's tunnel-mouth side,
  converted from tunnel-piece direction to pathfinder edge direction
  (`0=W, 1=N, 2=E, 3=S`);
- patch the tunnel-branch call site in Windows `cSC4PathFinder::FindPath`
  at `0x006D9ACF` (`E8 CC F4 FF FF`, original target
  `cSC4PathFinder_AddTripNode` at `0x006D8FA0`);
- when the pathfinder jumps exactly from one custom endpoint to its peer,
  rewrite the low two direction bits of the `edge` argument to the peer's
  stored arrival edge before calling the original `AddTripNode`.

The rewrite is now limited to mixed-axis portal pairs. Same-axis tunnels already
match the native assumption that the edge used to enter one endpoint is valid at
the peer, and forcing a geometrically selected path key back into `AddTripNode`
regressed those cases. For mixed-axis pairs, the destination node edge should be
the peer portal's tunnel mouth side. The incoming path key is still useful for
`MakeTunnelPaths` stitching, but it is not authoritative for pathfinder node
entry. The wrapper also preserves any upper bits in the edge byte instead of
replacing the whole value with a bare `0..3` direction.

Expected validation log during route search:

```text
TunnelPortalTool: rewrote pathfinder tunnel AddTripNode edge current=(...)
```

If this fires but mixed-axis commute still fails, the next target is the
analogous direction preservation in `FloodSubnetwork` (`0x00722E18` is the
Windows call to the tunnel hash-map lookup inside that routine).
