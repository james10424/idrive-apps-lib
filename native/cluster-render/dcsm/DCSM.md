# Dcsm2 client

`dcsm_client.{h,cpp}` asks the instrument cluster (KOMBI) to show a RAMSES
scene in one of its display slots through `de.bmw.infotainment.dcsm.Dcsm2`. It
runs on the head unit's own libvsomeip3 3.1.34. It covers what is needed to
fetch, map, unmap and unfetch a scene and to watch the result.

Labels:
- **VERIFIED**: read from the unit's interface definitions, configuration or
  binaries (the HMI is HmiMain3D; the KOMBI's main application), seen in a
  vsomeip trace from the car, or observed on the car.
- **INFERRED**: reasoned from verified facts.
- **CAR-ONLY**: needs the vehicle.

## Use

```cpp
auto app = vsomeip::runtime::get()->create_application("cluster-demo");
app->init();
dcsm::Client dcsm(app);                 // clientID kDefaultClientHandle, stock heartbeats
// dcsm::Client dcsm(app, kDefaultClientHandle, dcsm::HeartbeatMode::kRequest);
//                                       // heartbeats as requests, replies checked
dcsm.onAvailability = ...;              // then fetch()
dcsm.onFetchStatus  = ...;              // E_FETCH_STATUS_FETCHED -> map()
dcsm.onMapStatus    = ...;              // final E_MAP_STATUS_MAPPED_FOREGROUND
dcsm.onSlots        = ...;              // slot E_SLOT_STATUS_OCCUPIED
dcsm.onReply        = ...;              // (method id, ok, EDcsm2Status)
dcsm.onHeartbeat    = ...;              // kRequest: first reply, bad reply, silence, resumed
dcsm.start();
std::thread([app] { app->start(); });   // vsomeip's loop
...
dcsm.unmap(id); dcsm.unfetch(id); dcsm.stop(); app->stop();
```

Callbacks run on vsomeip's dispatch thread. `status_name()` gives the
interface's name of every `EDcsm2Status`. The client sends
`reqSlotStatusUpdate` each time the service becomes available. Requests sent
while the service is unavailable are dropped, so wait for
`onAvailability(true)` before sending.

## Protocol

**Service** (VERIFIED):
- service 0xB05F, instance 1, interface version 1.0;
- all methods and events reliable (TCP);
- no registration method, so the client chooses its own `clientID`.

| Id | Member | Request payload | Reply |
|---|---|---|---|
| 0x0001 | `reqFetchContent` | clientID u64, ContentId2 | EDcsm2Status |
| 0x0002 | `reqMapContent` | clientID u64, ContentId2, contentClass u32, slotID u64 | EDcsm2Status |
| 0x0003 | `reqUnfetchContent` | clientID u64, ContentId2 | EDcsm2Status |
| 0x0004 | `reqUnmapContent` | clientID u64, ContentId2 | EDcsm2Status |
| 0x0005 | `heartbeat` (fire and forget; also answered as a request, see Heartbeat) | clientID u64, counter u32, timestamp u64 | — (empty RESPONSE to a REQUEST) |
| 0x0006 | `reqSlotStatusUpdate` (fire and forget) | empty | — |
| 0x8007 | event `fetchStatusChange` | ContentId2, EFetchStatus | eventgroup 1 |
| 0x8008 | event `mapStatusChange` | ContentId2, MapStatusTransition | eventgroup 1 |
| 0x8009 | event `slotStatusChange` | SlotStatusList2 | eventgroup 1 |

The events are plain events, not fields, so a subscriber gets no initial
value. The slot list arrives only after `reqSlotStatusUpdate` or a change.

**Encoding** (VERIFIED): CommonAPI SOME/IP 3.1.12 serialization, big-endian,
no padding; each struct is its members in order, with no length field.

| Type | Wire form |
|---|---|
| `ContentId2` | `sceneId` u64, `elementTag` u32 (12 bytes) |
| `MapStatusTransition` | `initial` u32, `final` u32, `startTime` u64, `endTime` u64, `hint` u32 (28 bytes) |
| `SlotStatus2` | `slotId` u64, `slotStatus` u32, `MappingInfo{ContentId2, contentClass u32, mapStatus u32}` (32 bytes) |
| `SlotStatusList2` | u32 **byte** length, then the elements; at most 1024 |
| enums | UInt32 |

**Enum values** (VERIFIED):
- `EDcsm2Status`: `E_DCSM_OK`=0, `E_DCSM_HANDLE_UNKNOWN`=1, `E_DCSM_REQUESTID_UNKNOWN`=2, `E_DCSM_REQUESTID_INUSE`=3, `E_DCSM_CONTENTID_UNKNOWN`=4, `E_DCSM_SLOTID_UNKNOWN`=5, `E_DCSM_PRIOTOOLOW`=6, `E_DCSM_CONTENT_ALREADYMAPPED`=7, `E_DCSM_CLIENT_ALREADYREGISTERED`=8.
- `EFetchStatus`: FAILED 0, FETCHING 1, FETCHED 2, REMOVED 3.
- `EMapStatus2`: UNMAPPED 0, MAPPED_FOREGROUND 1, MAPPED_BACKGROUND 2, MAPPED_INVISIBLE 3.
- `ESlotStatus`: DISABLED 0, AVAILABLE 1, OCCUPIED 2.

**Sequence** (VERIFIED, the HMI's):
1. Wait for availability, then send `reqSlotStatusUpdate` and receive the slot list.
2. `reqFetchContent` → `E_DCSM_OK`, then wait for `fetchStatusChange` FETCHED.
3. `reqMapContent(slot, contentClass)` → `E_DCSM_OK`, then `mapStatusChange` → MAPPED_FOREGROUND and `slotStatusChange` with that slot OCCUPIED.
4. `reqUnmapContent`, then `reqUnfetchContent`.
5. A refusal comes back in the reply as an `EDcsm2Status`.

**Releases and fetches** (from the service's own logs; the car once, a
test unit's stand-in every time):
- A release of a mapped content is answered by the unmap's reply, the
  unfetch's reply, `mapStatusChange` to UNMAPPED and then
  `fetchStatusChange` REMOVED, within about 35 ms of the unmap.
- The renderer subscribes to the scene at the fetch, 45-105 ms after it
  and before FETCHED, and unsubscribes after the unfetch, 150-170 ms after
  the REMOVED (the RAMSES client logs both). Seen with a test unit's
  stand-in renderer only.
- On the car, a fetch sent in the same tick as the scene's first
  publication was answered FETCH FAILED 12 ms later; the same fetch 2 s
  later was FETCHED.
- On the car, the KOMBI removed our shown map scene (`fetchStatusChange`
  REMOVED with healthy heartbeats and the slot not locked) after a minute
  of 5.8 MB/s of upload. A fetch 2 s after the removal was answered FETCH
  FAILED 125 ms after it was sent (24 ms after FETCHING); one 7 s after the
  removal was FETCHED and shown.

## Heartbeat

The client sends one every 250 ms (`kHeartbeatPeriod`) while Dcsm2 is
available, as the HMI does.

What the HMI sends (VERIFIED in a vsomeip trace from the car):
- clientID 1, `REQUEST_NO_RETURN`, interface version 1, from a vsomeip
  client of its own that carries nothing but heartbeats.
- Every 250 ms: the counter rises by exactly 4.000 per second, and
  consecutive payload timestamps are a median 250.003 ms apart.
- Counter: starts at 0 and adds 1 per beat, only while the proxy is
  available.
- Timestamp: `CLOCK_MONOTONIC` in nanoseconds.

What the KOMBI does with it (VERIFIED in its main application):
- The stub reads clientID, counter and timestamp and queues them with no
  check of the sender, the counter or the timestamp.
- Its worker finds or creates the entry of that full 64-bit clientID, marks
  it active and restarts that client's one-shot watchdog, about **10 s**.
  The counter is not used.
- `reqFetchContent`, `reqUnfetchContent`, `reqMapContent` and
  `reqUnmapContent` restart the same watchdog.
- On expiry it logs `DCSM2 WATCHDOG TIMEOUT (client)` and unfetches and
  unmaps every mapping of that clientID. VERIFIED on the car: the slot
  reported AVAILABLE with our content still in its mapping, then
  `fetchStatusChange` REMOVED, with no `mapStatusChange`.
- So one heartbeat or request per 10 s keeps a client's content.

`HeartbeatMode::kRequest` sends the same payload as an ordinary REQUEST. The
stub answers a REQUEST once the method has run, and for a heartbeat the
response has no body (VERIFIED), so an empty E_OK reply shows that a
heartbeat with this clientID reached the KOMBI's watchdog. The HMI never
sends heartbeats this way; that the KOMBI's SOME/IP stack delivers such a
REQUEST to the stub on the car is CAR-ONLY.

The client counts heartbeat requests and replies from each availability and
reports through `onHeartbeat` only the first empty E_OK reply, the first
E_OK RESPONSE with a body (`kBadReply`; later ones are counted in
`HeartbeatStats::badReplies`), a silence of `kHeartbeatSilence` (2 s) since
the last reply or the first request, and the first reply after it
(`kResumed`). `HeartbeatTracker` makes these decisions; the client runs it,
and each report, under a lock, so a reply racing the silence check cannot
produce a stray `kSilent` or `kResumed`. Heartbeat replies never reach
`onReply`.

An ERROR, or a RESPONSE whose return code is not E_OK (`kRefused`; e.g.
`E_UNKNOWN_METHOD`, `E_WRONG_MESSAGE_TYPE`), means the KOMBI did not take
the request, so its watchdog did not see that heartbeat. The client reports
the first one and sends the rest fire and forget, the stock form, until
Dcsm2 next becomes available.

## clientID

- The HMI sends the constant **1**, in the heartbeat and in
  `reqMapContent`/`reqUnmapContent` (VERIFIED).
- This client sends `kDefaultClientHandle` = `0x00636c7573746572`
  ("\0cluster"), a fixed value that never equals 1. Pass another value to
  the constructor to change it.
- VERIFIED on the car: the KOMBI accepts this second handle and maps scenes
  for it.

## Slots: one shown, the rest parked

VERIFIED on the car (slot 0x100, the central map area, content class 0):
- **No pre-emption.** The first content mapped into a slot is shown
  (`mapStatus` MAPPED_FOREGROUND). A later `reqMapContent` into it is
  answered `E_DCSM_OK` and parked: MAPPED_INVISIBLE. When the shown content
  is unmapped, the oldest parked one is shown within about 0.5 s. The
  content class does not change this for 0x100; class 1 was parked even
  with the slot free, so the service maps with class 0.
- **The table** (`slotStatusChange`) has one `SlotStatus2` per mapping:
  slot, content, class and `mapStatus`, with the slot's status copied into
  each. The row with MAPPED_FOREGROUND or MAPPED_BACKGROUND is the one
  shown. Rows stay listed, UNMAPPED, until their content is unfetched.
- **The stock map.** In Map view the HMI maps its map, content 0x01000300,
  class 0, into 0x100.
- **Locks.** A phone reconnecting or a door-open warning disables 0x100
  (DISABLED), and the KOMBI unmaps every mapping in it. When it is
  AVAILABLE again, whoever asks first is shown.

### Taking over from the stock HMI

`Client::unmapAs(handle, content)` sends `reqUnmapContent` with another
client's handle. VERIFIED on the car: an unmap of {0x01000300, tag 0} with
clientID 1 (the HMI's) is answered `E_DCSM_OK`, the KOMBI shows the content
parked behind it, and the HMI at once maps 0x01000300 again, which is parked
behind that content. When that content is unmapped, the stock map is shown
again within 1 s.

`contentClass` is an opaque u32 defined by the KOMBI; the HMI takes it and
the slot from per-state records. The service's values are in
`../cluster-render.conf`.

## Running as uid 5000

- Only uid 5000 / gid 5000 (`hmi_5000`) may request 0xB05F, and the unit's
  vsomeip enforces credentials (VERIFIED: its security policies).
- The policy is keyed by uid/gid, not application name, so a second
  uid-5000 process gets the same rights (INFERRED); the HMI's two processes
  already run as `hmi_5000`.
- Run as `User=hmi_5000`, `Group=hmi_5000`, with the supplementary groups
  from `/etc/group`, so `mgu_someip` (902, the routing manager's group) is
  included. systemd does this when `User=` is set; with `setpriv`, pass
  `--init-groups`.
- Set **no** `VSOMEIP_CONFIGURATION`, so the process loads `/etc/vsomeip` as
  the HMI does, and leave `VSOMEIP_APPLICATION_NAME` unset. The application
  name `cluster-demo` is not among the unit's fixed client ids, so the
  routing manager assigns one that cannot collide with the HMI's.
- Use clientID `kDefaultClientHandle`, never 1.

## Build and ABI

- **Toolchain (VERIFIED): Ubuntu 18.04 amd64, gcc 7.5, glibc 2.27**
  (`../docker/Dockerfile`, pinned to the amd64 manifest).
- **Not libc++.** The unit's libvsomeip3 takes `std::string`,
  `std::shared_ptr` and `std::function` across its interface, and libc++
  (`zig c++`) is not ABI-compatible with the unit's libstdc++ 6.0.24:

  | | `std::string` | `std::function` | mangling |
  |---|---|---|---|
  | libc++ | 24 bytes | 48 bytes | `std::__1` |
  | libstdc++ | 32 bytes | 32 bytes | `std::__cxx11` |

  Most calls are virtual, so a libc++ build would link cleanly and then
  corrupt memory at run time.
- **Headers.** `third_party/vsomeip/interface` holds the interface headers
  of COVESA vsomeip **3.1.37.1** (https://github.com/COVESA/vsomeip,
  MPL-2.0, `third_party/vsomeip/LICENSE`). Upstream has no 3.1.34 tag;
  3.1.20.3 and 3.1.37.1 have identical interface headers apart from
  annotations in `plugin.hpp`. `tools/abi.py vtable` VERIFIED all 52
  `application` vtable slots of the unit's library against
  `application.hpp`.
- **Linking.** The build links against a stub generated from
  `abi/libvsomeip3.so.3.symbols`, the exported symbols of the unit's
  `libvsomeip3.so.3.1.34` (its sha256 is in the file). The client's only
  by-name import is `vsomeip_v3::runtime::get()`.
- **Binary check.** `tools/abi.py check` passes when a binary meets all of
  these:
  - loader `/lib/ld-linux-x86-64.so.2`;
  - every vsomeip import is exported by the unit's library;
  - no symbol needs glibc newer than 2.27, `GLIBCXX` newer than 3.4.24 or
    `CXXABI` newer than 1.3.11 (the unit's limits).
