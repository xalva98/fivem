# RedM 128-Player OneSync — Gate 0: Can this be tested/developed solo?

Branch: `redm-onesync-128` (off `citizenfx/fivem:master`). Investigation notes only — no code.

Gate 0 is the go/no-go: **can ~40 fake/headless clients be stood up locally to exercise
the sync path, without needing 32+ real humans online?** If not, the project isn't
solo-feasible and shouldn't start. This documents the answer, grounded in the actual tree.

---

## Verdict: GO — via simulated server-side peers, not headless clients

The testing blocker is solvable, but only by one of the two candidate routes. The other is
dead for RedM.

---

## Route A — Headless / no-render game clients: DEAD for RedM

The idea (EHBW's): spin up 33+ game clients with no GPU to load-test sync. Findings from
the tree:

- **No headless / no-render client mode exists.** No `-noRender`, `nullRender`,
  `GFX_NULL`, software-rendering, or `botClient` flag anywhere in client code. Every client
  path creates a real hardware D3D11 device:
  - `rage-graphics-five/src/RenderHooks.cpp:620` — `D3D11CreateDeviceAndSwapChain(..., D3D_DRIVER_TYPE_HARDWARE, ...)`
  - `client/launcher/ViabilityChecks.cpp:117` — launcher assumes a real GPU adapter.
- **The only "controlled by another process" mechanism is fxdk "reverse game"** — and it is
  **off-screen, not off-GPU**. It substitutes a `BufferBackedDXGISwapChain`
  (`RenderHooks.cpp:686-697`) whose `Present` just `CopyResource`s the fully-rendered frame
  into a shared texture (`RenderHooks.cpp:255-285`). It still runs the entire GPU pipeline
  every frame — one real GPU context per instance. Spinning up 33 this way needs 33 GPUs.
- **Reverse-game is GTA-V-only.** `rage-graphics-rdr3/src/RenderHooks.cpp` (303 lines) has
  **zero** reverse-game hooks — no `isReverseGame`, no `BufferBackedDXGISwapChain`. So even
  the off-screen path doesn't exist for RedM; it would be a from-scratch RE sub-project
  *and* still be GPU-bound.
- The dev's memory of a "removed no-render mode" matches: no such mode is present; only the
  GPU-bound, Five-only reverse-game remnant remains.
- One reusable nugget: fxdk **injects input programmatically** via `ReverseGameData`
  (`client/citicore/ReverseGameData.h:79-175` — keyboard/mouse/`ioPad` structs). The
  input-driving half of a bot has a proven pattern; the render-skipping half does not.

**Conclusion:** headless RedM clients cannot be produced from anything in the tree. Cross
this route off.

---

## Route B — Simulated server-side peers: FEASIBLE (this is the path)

Inject N fake `fx::Client` entries server-side that flow through the real scope/sync path
without a real ENet socket or game client. Findings:

- **`fx::Client`'s transport is just an `int` peer handle.** `Client::SetPeer`
  (`citizen-server-impl/src/Client.cpp:14`) stores `new int(peer)` + a `net::PeerAddress`.
- **Outbound sync silently no-ops for an unknown peer.**
  `Client::SendPacket` → `gscomms_send_packet` → `GameServerNetImplENet::SendPacket`
  (`GameServerNetImplENet.ENet.cpp:469`) does
  `if (m_peerHandles.find(peer) == end()) return;`. So a fake client generates the **full**
  server-side clone workload — scope computation, buffer building, serialization for all 128
  slots — and the bytes are harmlessly dropped. **Exactly what a load test wants:** server
  cost scales, fake "sends" cost nothing.
- **A socketless fake-client harness already exists** in `code/tests/server/`:
  - `tests/server/ClientRegistry.cpp:53-59` — `MakeClient` + `SetPeer(1, PeerAddress::FromString("127.0.0.1"))`, no real socket.
  - `tests/server/TestRequestObjectIds.cpp:70` — `SetNetworkMetricsSendCallback(...)`
    captures every outbound `SendPacket` for inspection instead of a socket. This is the
    proven mechanism to "receive" clone data for a fake peer.
  - Mock `GameServer` / `ServerInstance` / `ServerGameStatePublicInstance` builders exist.
  - Caveat: these tests mock `GameServer`, so they prove fake-client fabrication + packet
    capture, but do **not** yet drive the real `ServerGameState::Tick` end-to-end. The
    harness extends this to run the real tick.
- **Couplings to defeat (small, enumerated):**
  1. **Liveness** — `Client::IsDead` / `Client::GetPing` (`Client.cpp:44-77`) treat a missing
     peer as dead and would evict the fake client. Fake the ping / register a dummy handle /
     keep `Touch()` fresh.
  2. **Lifecycle bootstrap** — replicate `GameServer::ProcessPacket` msgType-1 path
     (`GameServer.cpp:643`): `SetData("passedValidation", true)`, `SetConnectionToken`,
     `SetPeer` (triggers slot assignment), `HandleConnectingClient`, `HandleConnectedClient`.
  3. **Ack feedback** — under `SyncStyle::ARQ` the server waits on client acks to advance
     frames; a passive fake client stalls resends. **Run the harness in NAK mode**
     (`ServerGameState.cpp:892`, default when `g_oneSyncARQ` off) to avoid synthesizing acks,
     or feed synthetic `msgPackedAcks` if ARQ must be tested.
  4. **Slot ceiling** — force BigMode + OneSync convars on (`GameStateExports.cpp:16-55`);
     `m_clientsBySlotId` sized in BigMode (`ClientRegistry.cpp:66-73`); slots 31 (all) and 16
     (RDR3) reserved.
- **`ServerGameState::Tick` iterates by slot id** (`ServerGameState.cpp:1000-1032`), so fake
  slots participate in scope/serialization automatically — no per-fake special-casing.

**Key files for the harness:** `src/ClientRegistry.cpp` (MakeClient/slots),
`src/GameServer.cpp:643` (ProcessPacket admit path), `src/Client.cpp:44-77`
(IsDead/GetPing), `src/state/ServerGameState.cpp:873` (Tick — the load target),
`GameStateExports.cpp` (force BigMode/OneSync), reference: `code/tests/server/*`.

---

## Important scope limit of Route B (be honest about this)

A simulated peer tests **the server, not the client.** It validates that the server's
scope/serialization/CPU path scales to 128 slots — which is half the project and the half
most worth de-risking first. It does **NOT** validate that a real RedM client correctly
*deserializes* clone data, applies the sync node trees, and renders 128 players without
desync. The hardest, most nondeterministic bugs live on the client deserialization side, and
fake peers (which drop their bytes) test none of it.

So Gate 0 splits:
- "Load-test the **server** side locally without 32 humans?" → **Yes, confidently.**
- "Validate the full **client-side** 128-player experience locally?" → **No cheap way** (still
  needs real clients or a from-scratch GPU-bound RDR3 reverse-game harness).

---

## How FiveM actually did it (and why this matches)

OneSync was built largely solo (Blattersturm/NTA) in **layers over ~2 years**, each
shippable and testable on its own — not as one monolithic "128-player" project:

1. **Reverse the sync node trees first** — hand-reversed `CNetObj*` trees by reading the
   game's VMTs (`SyncTrees_Five.h`). The foundational, per-entity-type RE grind.
2. **Server as passive relay** — early OneSync only *parsed* clone data flowing through the
   reserved virtual "player 31"; the server understood world state before controlling it.
3. **Server-authoritative state** (`ServerGameState`) — server owns entities, assigns object
   IDs, computes per-client scope/culling (424-unit focus zone), sends clone create/sync/remove.
4. **BigMode/Infinity scaling** — object IDs 8192→65535; the key insight that makes 128
   tractable: **each client only ever sees ~30-127 nearby players** (culling). RDR3 BigMode =
   30 visible slots; no client renders 128.

**How he tested it without 32 people** — three methods, none needing a crowd:
- **Fake server-side players (primary).** Exactly Route B. The entire commercial
  "FiveM fake players / server bots" tooling industry (FiveBoosts simulating 100+
  walking/driving units, CFX.BOT, etc.) is server-side fake-peer injection — proving at
  market scale that you can push the sync path to 128 with synthetic peers. His own version
  was a private harness (not in the public repo, which is why grepping the tree finds none).
- **A few real clients for correctness, not scale** — deserialization correctness needs 2-3
  clients, not 128.
- **`cl2` multi-client** — FiveM runs multiple game clients on one machine
  (`client/shared/CL2LaunchMode.h`, `IsCL2()` on `"cl2"` in command line), so one person runs
  2-4 real clients locally for multi-client correctness.

Synthesis: fake peers prove the **server** scales; a few `cl2` clients prove the **client**
deserializes; emergent 128-real-client desync is validated **last**, with volunteers on a
live test server — not during development. You would be doing nothing NTA didn't do.

---

## Our sequencing (mirrors his)

1. **Gate 1 — audit RDR3 sync-tree completeness** (in progress): how much of
   `SyncTrees_RDR3.h` / cloneexperiments already exists vs. what's missing for 128. Decides
   whether the RE remaining is "fill gaps" or "large rewrite."
2. **Fake-peer harness** — extend `tests/server/` to run the real `ServerGameState::Tick`
   with ~128 fake slots in NAK mode + BigMode. Load-test the server side.
3. **`cl2` multi-client** on one machine — deserialization correctness at small N.
4. **Volunteers, last** — emergent-desync validation at real scale on a test server.

---

## Gate 1 — RDR3 sync-tree completeness ("is most of it already there?")

Short answer: **no — the scaffolding is there, but ~2/3 to 3/4 of the node-level RE remains.**

**What exists (the cheap, structural part):**
- `SyncTrees_RDR3.h` (2846 lines), selected via `STATE_RDR3` in `SyncTrees.h:1-7`.
- **31 sync trees** defined — *more* than Five's 14 (RDR3 adds horse/mount, draft wagon,
  herd, incident, guardzone, anim scene, combat director, stats tracker, etc. tree shells).
- Shared tree-walking machinery (`SyncTrees_Header.h`), `IsRDR` wire-bit handling
  (`SyncTrees_Header.h:606,661-703`), and the client transport (RDR3 **reuses Five's**
  `CloneExperiments.cpp` directly — `gta-net-rdr3/component.lua:15-16`).

**What's missing (the expensive, per-node RE part):**
- **76% of RDR3 sync *nodes* are empty `{ };` stubs — 97 of 127.** Only ~28-30 have real
  serialization. (Five is inverted: 51 of 75 implemented, 29% stub.)
- Trees walk fine, but stubbed leaf nodes carry **no state** → entities sync
  position/orientation/basic-creation only, **not gameplay state**.
- `CPlayerGameStateDataNode` — the single most important node — is a bare stub in RDR3
  (`SyncTrees_RDR3.h:722`) vs ~360 lines in Five (`:2953`). Same for vehicle script/damage
  state, ped tasks/AI/appearance, plane/heli/train control.
- Every RDR3-unique system is 100% stub beyond creation: horses' full state, `CDraftVeh*`
  (wagons), herds, incidents, guardzones, anim scenes, combat director, stats tracker.
- Explicit `// TODO` mid-node bailouts at `:828`, `:831`, `:874`, `:1515`.
- **Estimate: ~90+ node bodies to reverse-engineer**, including the large
  `CPlayerGameStateDataNode` and the ped-task / vehicle-script clusters.

**What this means (reframed honestly):**
1. **Fill-in, not rewrite.** Architecture, transport, tree structure, and node *inventory*
   exist. Work = implementing node bodies one at a time (reverse the game VMT for that node's
   read/write, transcribe the bit layout). The most parallelizable, least-architectural RE
   there is — exactly what NTA did for Five, node by node.
2. **Ships incrementally.** Entities already sync position/creation, so there's a working
   baseline today. Each node adds one gameplay slice; no "nothing works until everything
   works" cliff.
3. **128-scaling is mostly orthogonal to nodes.** BigMode/slot scaling lives in
   `ServerGameState`/`ClientRegistry` (shared, works for Five). So "128 players moving with
   basic sync" is reachable relatively soon; deep node fidelity deepens over time.
4. **Still a long grind.** ~90 nodes incl. several large ones = months of methodical RE.
   This is the true scope EHBW bounced off. Tractable and incremental, but long and
   repetitive — NOT "improving some stuff."

**Recommended node order (highest gameplay value first):** `CPlayerGameStateDataNode` →
ped task/AI/appearance cluster (`CPedTaskTreeDataNode`, `CPedAIDataNode`,
`CPedAppearanceDataNode`) → vehicle script/damage (`CVehicleScriptGameStateDataNode`,
`CVehicleDamageStatusDataNode`) → `CPhysicalAttachDataNode` → then RDR3-unique systems
(horse full state, wagons) as the server world needs them.

---

## Sources
- OneSync docs — https://docs.fivem.net/docs/scripting-reference/onesync/
- Entity Management (DeepWiki) — https://deepwiki.com/citizenfx/fivem/3.1-entity-management
- Server-side fake-player harness (proves Route B at market scale) — https://github.com/FiveboostsDotNet/Fivem-Fake-Players
- Blattersturm on GitHub — https://github.com/blattersturm
