# Multiplayer Foundation -- Design

## Purpose

`docs/STATE.md` has carried "Multiplayer/networking foundation -- nothing
exists yet" as unstarted work since before the editor pivot, explicitly
scoped as its own sub-project "once the editor's scene model exists to
replicate." The scene model now exists (multi-scene, the Scenes panel,
the Inspector's component editing). This project builds the first slice:
a real UDP transport, a client-server session model, opt-in entity
replication, and a manual RPC system -- engine-level (`GS/src/GS/
Network/`), exposed in the editor (a Network panel, two new Inspector
components), and proven with a real two-process demo.

**Explicitly out of scope, deferred to a named follow-up:** client-side
prediction and reconciliation for the locally-controlled entity. This
project sends client input to the server every fixed step (so prediction
can be added later without a protocol change), but for now every entity,
including the ones the client controls, only moves when the server's
snapshot says so -- there is a full round trip of latency on your own
movement until prediction lands. Also out of scope: a headless dedicated
server binary, matchmaking/NAT traversal (direct IP:port only), and
generic reflected component replication (only `TransformComponent`, via
the two components below, replicates automatically -- anything else is a
manual RPC).

## Topology

Host-as-server. One `TestEnv` process runs `NetServer`, which *is* the
authority: that process's own local player reads input straight into the
`Scene`, no network round trip for itself. Every other player runs
`NetClient`, connecting to the host's `NetServer` over UDP. There is no
separate dedicated-server build target.

## Module layout

New directory, `GS/src/GS/Network/`, built into the `GS` shared library
alongside `Physics/`, `Audio/`, `Scene/`:

- `Socket.h` / `.cpp` -- a thin POSIX UDP wrapper: `Bind(port)`,
  `SendTo(addr, data, len)`, `RecvFrom(buffer, maxLen, outAddr)`
  (non-blocking, returns 0 when nothing is waiting). Linux-only, matching
  where this project actually runs (`bin/<Config>-linux-x86_64`); no
  Windows shim.
- `ByteStream.h` / `.cpp` -- a small binary writer/reader:
  `Write(uint8_t/uint16_t/uint32_t/float/std::string)` and matching
  `Read*()`, plus `glm::vec3`/`glm::quat` overloads built on the
  primitives. Everything sent or received goes through this.
- `Protocol.h` / `.cpp` -- the packet header (below), sequence-number
  wraparound comparison, and the ack-bitfield math.
- `NetConnection.h` / `.cpp` -- per-peer state, used by both roles: send/
  receive sequence counters, the ack bitfield, RTT estimate, last-received
  timestamp (for timeout), and the reliable-message resend buffer.
- `NetServer.h` / `.cpp` -- binds a socket, accepts/rejects connecting
  clients, owns one `NetConnection` per connected `ClientId`, broadcasts
  snapshots, routes incoming client messages to registered handlers.
- `NetClient.h` / `.cpp` -- binds an ephemeral socket, connects to a
  server address, owns one `NetConnection` (the server), sends local
  input each fixed step, applies incoming snapshots/RPCs.
- `NetMessage.h` -- the RPC registration API (below).

## Packet format and reliability

Every UDP datagram starts with a fixed header:

```
uint16 Sequence      // this packet's own sequence number
uint16 Ack           // highest sequence number we've received from the peer
uint32 AckBits       // bit i set => (Ack - 1 - i) was also received
```

followed by one or more length-prefixed messages, each tagged reliable or
unreliable. `AckBits` covers the 32 sequence numbers before `Ack`, so a
single lost ack packet doesn't lose track of what was actually delivered
-- the next packet's bitfield re-confirms it. Sequence numbers are
`uint16` and compared with wraparound arithmetic (`Protocol::IsMoreRecent`,
the same trick as TCP's), which is enough range at expected send rates
(20-60 Hz) for a session to run for hours before wraparound could even
become ambiguous.

**Unreliable channel:** fire-and-forget, latest-wins. Used for
`NetworkTransform` snapshots and client input -- a lost one is superseded
by the next within one send interval, so retransmitting it would only add
stale data.

**Reliable-ordered channel:** the sender keeps every reliable message in a
resend buffer keyed by sequence number; a resend timer (250 ms) re-sends
any message not yet confirmed by the peer's ack bitfield. The receiver
delivers reliable messages to handlers in sequence order, buffering any
that arrive out of order until the gap is filled. Used for the connect
handshake, disconnect notice, and every RPC.

**Packet size:** messages are packed into the smallest number of packets
that stay under 1200 bytes each (comfortably under the common 1500-byte
MTU minus headers), avoiding IP fragmentation. A single message larger
than that does not fit this version -- fragmentation of one oversized
message across packets is not built; nothing this project sends
(transform snapshots, small RPC payloads) is anywhere near the limit.

## Connection lifecycle

1. Client sends `ConnectRequest` (reliable) to the server's address.
2. Server responds `ConnectAccepted{ClientId}` or `ConnectRejected{Reason}`
   (server full, or a protocol-version mismatch -- a `uint16` constant
   bumped whenever the packet format changes).
3. Both sides send a keepalive (empty reliable-channel packet, just the
   header) at 1 Hz whenever nothing else was sent that tick, so a healthy
   idle connection still acks regularly.
4. No packet received from a peer for 5 seconds => timeout, treated as a
   disconnect.
5. A clean quit sends `Disconnect` (reliable, best-effort -- not retried
   past one resend cycle, since the peer will time out anyway if it's
   lost).

`NetServer` exposes `OnClientConnected(ClientId)` and
`OnClientDisconnected(ClientId)` callbacks. `NetClient` exposes
`OnConnected()`, `OnDisconnected(Reason)`.

## Replication

Two new components in `GS/src/GS/Scene/Components.h`, next to the
existing ones:

```cpp
using ClientId = uint16_t;
constexpr ClientId ServerOwned = 0;   // no client owns this entity's input

struct NetworkIdentity
{
    uint32_t NetworkId = 0;    // assigned by the server when the entity is spawned
    ClientId Owner = ServerOwned;
};

struct NetworkTransform
{
    float SendRate = 20.0f;    // Hz -- independent of the fixed-step rate
    bool Interpolate = true;
};
```

Every fixed step, `NetServer` walks entities that have both
`NetworkIdentity` and `NetworkTransform`, and for any whose `SendRate`
interval has elapsed, writes `TransformComponent` (Position, Rotation)
into an unreliable snapshot message keyed by `NetworkId`, broadcast to
every connected client. `NetClient` looks up the local entity for each
`NetworkId` in the snapshot (a `std::unordered_map<uint32_t, EntityId>`
it maintains from spawn/despawn messages) and either snaps
`TransformComponent` directly to the received value, or -- when
`Interpolate` is true -- buffers the last two received snapshots with
their arrival times and lerps between them each render frame. Scale is
not replicated; nothing in this project's target use case (moving
players/objects) animates scale over the network, and adding it later is
a one-line addition to the snapshot struct.

Spawning a networked entity is server-only: the server creates the
entity, assigns the next `NetworkId`, and sends a reliable `SpawnEntity
{NetworkId, Owner, initial Transform}` to every client, which creates a
local entity with matching `NetworkIdentity`/`TransformComponent` and
registers it in its `NetworkId -> EntityId` map. Destroying it sends a
reliable `DespawnEntity{NetworkId}`; the client removes the entity and
the map entry.

**Input, sent but not yet consumed as prediction:** each fixed step, every
client sends its current input state (an application-defined
`ByteStream` payload, opaque to the network layer) to the server on the
unreliable channel. The server is the only thing that reads it to drive
simulation. This wiring exists now specifically so the prediction
follow-up project doesn't need a protocol change -- the client isn't
doing anything with its own input locally yet beyond sending it.

## RPCs

For anything that isn't a `TransformComponent` (score, "ball bounced",
"player fired"), a small manual registration API, `GS/src/GS/Network/
NetMessage.h`:

```cpp
namespace GS::Net {
    using MessageHandler = std::function<void(ClientId from, ByteStream& payload)>;

    void RegisterHandler(const std::string& name, MessageHandler handler);

    // Server: To = a specific ClientId, or Broadcast for everyone.
    void SendRPC(ClientId to, const std::string& name, ByteStream& payload, Reliability reliability);
    // Client: always addressed to the server.
    void SendRPC(const std::string& name, ByteStream& payload, Reliability reliability);
}
```

`name` is hashed once (FNV-1a -- simple, no dependency, good enough
distribution for the handful of message names one game defines) into a
`uint32_t` message type carried on the wire; the string is never sent. `Reliability` is `Reliable` or
`Unreliable`, defaulting to `Reliable` since most RPCs are one-shot
events where a drop would be a real desync (a missed "ball bounced" stays
missed), unlike a transform snapshot which self-corrects next tick.

## Editor integration

**Network panel** (`TestEnv/src/NetworkPanel.h`), a new dockable `Layer`
alongside the existing Scenes/Appearance panels: a port field and a
**Host** button; an address + port field and a **Join** button; once
connected, the role (Host/Client), connection state, and -- for a host --
a table of connected clients with `ClientId` and RTT read straight off
each `NetConnection`. Hidden/disabled the same way Scenes-panel actions
are gated when there's nothing to show (no active session).

**Inspector support:** `NetworkIdentity` and `NetworkTransform` are drawn
by the Inspector's existing per-component editing exactly like
`TransformComponent`/`SpriteComponent` today -- `NetworkId`/`Owner` shown
read-only once assigned by a live server, `SendRate`/`Interpolate`
editable. Adding networking to an entity is "Add Component", the same
verb as everything else.

## Demo

A new `TestEnv` demo, `NetworkDemo` (`TestEnv/src/NetworkDemo.h`,
registered in `DemoRegistry.h` like every other demo): two players, each
a coloured cube that moves on WASD, each with `NetworkIdentity` +
`NetworkTransform`. Run as two real processes:

```sh
./TestEnv --demo NetworkDemo --host 7777
./TestEnv --demo NetworkDemo --join 127.0.0.1:7777
```

`--host <port>` and `--join <addr:port>` are parsed the same way
`Application.cpp` already parses `--capture`/`--play` (a `valueOf`-style
scan of `argv`), and are read once at startup to call `NetServer::Host`
or `NetClient::Connect` before the demo's first fixed step. Pressing
Space broadcasts a `"Flash"` RPC (reliable) that both processes render as
a screen flash, exercising the RPC path alongside replication so the demo
proves both mechanisms, not just transform sync.

## Testing

Temporary self-tests, deleted after verifying (per the self-test
pattern):

1. **Reliability under loss** (`NetReliabilityTest.h`): two
   `NetConnection`s talking over real loopback UDP sockets, with an
   injected shim that drops a configured percentage of outgoing packets
   before they reach the socket. Assert every reliable message sent is
   delivered exactly once and in the order sent, at loss rates up to 30%.
   This is the "formula the code knows nothing about" check for this
   subsystem: the shim's loss rate is set by the test and never read by
   the code under test.
2. **RTT accuracy**: a responder that delays its reply by a known,
   injected amount (e.g. 40 ms); assert the measured RTT lands within a
   few ms of `2 * delay`.
3. **Replication convergence**: the actual two-process `NetworkDemo`,
   driven via `--lockstep`, server moves its cube a known distance over N
   fixed steps, client captured after enough real time has passed for
   the send interval plus one interpolation step; assert the client's
   local copy of the cube is within a small position tolerance of the
   server's.
4. **Spawn/despawn bookkeeping**: server spawns and then despawns a
   networked entity; assert the client's `NetworkId -> EntityId` map
   gains and then loses the entry, and the entity is actually created/
   destroyed in the client's `Scene`.

**Not covered by the existing `--record`/`--play` system:** that system
replays one process's own local input deterministically; it says nothing
about two independent processes' real, non-deterministic network timing
against each other. Each side's local input remains individually
recordable, but a multiplayer session as a whole is not bit-reproducible
-- a property of real networking, not a gap this project introduces.

Visual verification: a `--lockstep --hide-ui --capture-step N` capture of
each process in `NetworkDemo` showing both cubes at their converged
positions, and a capture of the Network panel mid-session showing a
connected client's RTT.
