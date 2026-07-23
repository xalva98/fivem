# ihatemylife-128 — RedM 128-player OneSync: the plan

Branch: `ihatemylife-128` (local, off `citizenfx/fivem:master`). **Never pushed.**

## The thesis (why this project is tractable solo)

From the Gate 0/1 investigation ([GATE0.md](GATE0.md)), three findings reshape the work:

1. **The 128 barrier is NOT the sync trees.** EHBW's PR #3477 adds **zero** sync-node bodies
   (`SyncTrees_RDR3.h` is untouched). The real barrier is the **game client's hardcoded
   32-player arrays** overflowing past 32 — his `PlayerArrayResizes.cpp` (~1,359 lines) is
   targeted binary patches (`0x20`→`0x80`/`0x7F`) at the *specific* instructions that index
   those arrays, found via `hook::get_pattern` context. NOT a blind constant sweep (that
   would corrupt thousands of unrelated `0x20`s).
2. **"Works at 30" is because you're under the 32-array limit AND everyone's in scope** (stubs
   masked). Past 32 you hit the array walls; that's the crash frontier.
3. **This is a crash-discovery grind, not a code-reading task.** You can't find the overflowing
   arrays by reading — there are too many. You find them by **running past 32 and seeing what
   dies.** EHBW likely never drove enough concurrent load to find the long tail, which is why
   the PR stalled. That long tail is exactly what a local fake-client rig surfaces.

**So the core dev loop is:** inflate to 128 → crash → `!analyze` the dump → find the
overflowing array/instruction → targeted patch (EHBW-style) → rerun → repeat until dumps stop.
This is the dump-driven loop we're already good at (four unrelated crashes triaged this session).

## The key harness insight

- **Fake server-side peers** inflate the *server* to 128 slots cheaply — `fx::Client` transport
  is just an `int`; `GameServerNetImplENet::SendPacket` no-ops for unknown peers, so fake
  clients do the full server clone workload with bytes dropped. Template already exists in
  `code/tests/server/` (`ClientRegistry.cpp` MakeClient + `SetPeer(1, loopback)`).
- **BUT** the `PlayerArrayResizes` crashes happen in the **game CLIENT**, not the server. To
  trigger them you need a **real game client receiving 128 players' worth of data**. So the
  reproduction rig is: **fake peers stuff the server to 128 + one real `cl2` client as the
  crash canary.** The fake peers generate the volume; the real client overflows and dumps.

---

## Phase 0 — Baseline: crash on purpose (START HERE)

Goal: get a RedM client connected to a local FXServer that has been inflated toward 128
players, and **observe the first crash.** We start from zero. A crash is success — it's the
first entry on the list we grind to empty.

Steps:
1. **Stand up a local RedM FXServer** (self-built or artifacts) with OneSync BigMode on
   (`onesync on`, `onesync_population`, high `sv_maxclients`). Minimal resources — we want the
   sync path, not a full server.
2. **Build the fake-peer spawner.** New server component/command (model on the
   `static ConsoleCommand` at `GameServerNet.ENet.cpp:188`, and the fake-client factory in
   `tests/server/ClientRegistry.cpp:53-59`): a `spawn_fake_clients <N>` console command that
   loops `MakeClient` → bootstrap through the `GameServer::ProcessPacket` msgType-1 admit path
   (`SetData("passedValidation", true)`, `SetConnectionToken`, `SetPeer`,
   `HandleConnectingClient`, `HandleConnectedClient`) → occupies a slot. Defeat liveness
   (`Client::IsDead`/`GetPing`, `Client.cpp:44-77`). Run in **NAK mode** (avoid synthesizing
   acks). This lives in server code, is dev-only, and is NEVER pushed / never near adhesive.
3. **Connect one real RedM client** to that server.
4. **`spawn_fake_clients 40`**, then 80, then 128. Move the real client around, spawn
   entities, drive activity.
5. **Collect every crash dump.** Expect the client to crash once fake+real players exceed ~32.
   That first dump is Phase 0's deliverable.

Exit criteria for Phase 0: we have a reproducible local rig that pushes the client past 32
players and produces a crash dump we can `!analyze`.

## Phase 1 — Grind the crash list to zero (the main work)

Loop, per crash:
1. `!analyze -v` + decompile the faulting function.
2. Identify the overflowing structure — almost always a fixed 32/0x20-sized player array or a
   stack frame sized for 32. (Cross-reference EHBW's `PlayerArrayResizes.cpp` from PR #3477 —
   if the dump's function/pattern matches one he already found, reuse his pattern/offset;
   if it's new, it's a member of the long tail he missed.)
3. Write a **targeted** `hook::get_pattern`-anchored patch (or `IncreaseFunctionStack<N>()`)
   for that specific site — never a blind constant replace.
4. Rebuild, rerun the rig, confirm that crash is gone, collect the next dump.
5. Repeat until 128 fake + 1 real client runs stably with no new dumps.

Reference (don't copy blindly, verify each against our build): PR #3477's
`PlayerArrayResizes.cpp`, `SerialisationPatches.cpp`, `SyncedExtensions.cpp`, and the cap
changes (`kGamePlayerCap` 32→128, `kSlotIdStart` 30→127, `bigModeSlot ? 128 : 128`, object-ID
13→16 bit / `OneSyncBigIdEnabled`, `stub_seg` 0x100000→0x400000).

## Phase 2 — Multi-real-client correctness (small N)

Once the client survives 128, use `cl2` (multiple game clients on one machine —
`client/shared/CL2LaunchMode.h`) to run 2-4 REAL clients and verify they see each other
correctly as they cross scope boundaries. Force a **tiny scope/culling radius** (shrink the
424-unit focus zone) so 2-3 clients can trigger scope-enter/exit locally.

## Phase 3 — Sync-node fidelity (only what's measured, not all 90)

With scope-crossing reproducible, observe **which state fails to survive** a player/entity
leaving and re-entering scope. Each failure = one stubbed `SyncTrees_RDR3.h` node to
implement. Likely shortlist (10-20, not 90): `CPlayerGameStateDataNode`, ped
task/AI/appearance, vehicle script/damage, `CPhysicalAttachDataNode` /
`CVehicleProximityMigrationDataNode` (migration = what BigMode does constantly). RDR3-unique
systems you don't use (herds, incidents, guardzones) stay stubbed.

## Phase 4 — Real-scale desync validation (last)

Volunteers on a live test server for emergent desync — the only thing fake peers + cl2 can't
show. This is validation, not development.

---

## Rules
- **Local only. Nothing on this branch is ever pushed.** The fake-client spawner is a dev tool
  and would be rejected upstream (it's the capability bot services abuse); keep it out of any
  PR. Any *upstreamable* result (array patches, node implementations) gets cherry-picked to a
  clean branch later.
- **No blind constant sweeps.** Every 0x20→0x80 is a targeted, pattern-anchored patch.
- **Dump-driven.** We don't guess which arrays to patch; the crashes tell us.
- Consult EHBW's #3477 as a *reference/pattern source*, not a base to fork — verify every
  offset against our own build.

## Spawner spec (derived from reading the REAL admit path)

Confirmed against `GameServer::ProcessPacket` msgType-1 (`GameServer.cpp:659-761`),
`ClientRegistry::MakeClient`/`OnAssignPeer` (`ClientRegistry.cpp:76-160`, slot assign at
`96-134`), `Client::SetPeer`/`IsDead`/`GetPing` (`Client.cpp:14-77`), ENet peer map
(`GameServerNet.ENet.cpp:430-433,602`).

The real admit path needs a live ENet `peer` object (`peer->OnSendConnectOK()`, addresses).
A fake client has none, so the spawner **replays the state mutations directly** instead of
going through `ProcessPacket`. Per fake client:

1. `client = clientRegistry->MakeClient(fakeGuid)` — `guid` = synthetic unique string.
2. `client->SetData("passedValidation", true)`; `client->SetConnectionToken(tok)`.
3. `client->SetTcpEndPoint("127.0.0.1")` (optional, for maps).
4. `client->SetPeer(fakePeerId, net::PeerAddress::FromString("127.0.0.1:0"))`
   — **this fires `OnAssignPeer` → assigns a real OneSync slot** (the whole point). Use a
   fakePeerId range clearly disjoint from real ENet ids (e.g. start at 0x40000000).
5. `clientRegistry->HandleConnectingClient(client)` — assigns net id.
6. On main thread: `clientRegistry->HandleConnectedClient(client, oldNetId)` — fires
   playerJoining, marks connected.
7. `RequestObjectIdsPacketHandler::SendObjectIds(instance, client, IsBigMode() ? 4 : 64)` —
   SendPacket no-ops (no peer in `m_peerHandles`), harmless.

**Liveness (the one real coupling to defeat):** `Client::IsDead` (`Client.cpp:44-68`) reaps a
connected client whose ENet peer is gone — `gscomms_get_peer` returns a `NetPeerImplENet`
wrapper (GetPeer always constructs one, `ENet.cpp:430`) but its `GetPing()` returns -1 for an
id not in `m_peerHandles` → `IsDead()==true` → evicted. Cleanest defeat: mark fakes with
`SetData("fakeClient", true)` and add ONE surgical early-return in `Client::IsDead` (`if
fakeClient → keep alive as long as recently Touch()ed`). Also guard `Client::GetPing`
(`:70-77`) against the null peer for fakes (returns e.g. 1). Both changes are dev-only, clearly
`// FAKE CLIENT (ihatemylife-128, never upstream)` labelled. Keep fakes `Touch()`ed each tick.

**Slot ceiling note:** RDR3 `bigModeSlot` at `GameServer.cpp:729` is `... GTA5 ? 128 : 16` —
this `:16` is exactly EHBW's PR change target (→128). Confirms our repro lands in his territory.

**Command surface:** a `static ConsoleCommand spawnFakeClients("spawn_fake_clients", [](int n){...})`
in a new dev-only file in `citizen-server-impl` (model registration on
`GameServerNet.ENet.cpp:188`'s `force_enet_disconnect` command). Plus `clear_fake_clients`.

## Fake-client placement (so they're in-scope of the real player)

Fake clients are socketless and have no ped/position unless the server fabricates one. To
actually stress the real client's per-player arrays, the fakes must be **in scope** of the
real player, so the server injects a player-ped per fake around a fixed anchor.

- **Anchor:** Valentine main street — `SPAWN = (-321.0, 785.0, 117.0)` (publicly-known RedM
  starter-town coord; central, flat, default-ish spawn). One constant; change if the server
  spawns elsewhere.
- **Spread:** phyllotaxis (sunflower) spiral so N peds distribute evenly without overlap and
  all stay inside the OneSync focus zone (~424u scope):
  - `angle_i  = i * 137.5°` (golden angle)
  - `radius_i = min(3m + i*0.6m, 35m)` (spiral out, capped ~35m so every fake is in-scope)
  - `x = SPAWN.x + cos(angle_i)*radius_i`, `y = SPAWN.y + sin(angle_i)*radius_i`, `z = SPAWN.z`
- **Why in-scope matters:** out-of-scope fakes only cost server bookkeeping; in-scope fakes
  force the *real client* to create/render/sync N player peds → that's what overflows the
  hardcoded 32-player client arrays (the PlayerArrayResizes crash frontier).

## Immediate next action
Implement the **Phase 0 fake-peer spawner** per the spec above (new dev-only source in
`citizen-server-impl` + the two labelled `Client.cpp` liveness guards), build the server,
stand up a local RedM server, connect one real client, `spawn_fake_clients 40/80/128`.
First milestone: one crash dump from pushing past 32 players.
