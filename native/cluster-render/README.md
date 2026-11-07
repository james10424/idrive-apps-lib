# cluster-render

The one renderer every app on a BMW iDrive 7 or iDrive 8 head unit (MGU)
draws through on the instrument cluster and the HUD: maps and video,
widgets, overlays, 2D and 3D, and whole-cluster content. Apps and native
producers are its clients ([PROTOCOL.md](PROTOCOL.md)); it is the head
unit's one custom RAMSES participant and display client (Dcsm2 on
iDrive 7, RAMSES 27's DCSM on iDrive 8, "iDrive 8"), shares the display
slots between its clients, and spends one upload budget for all of them.
`kit/` is the pixel path it shares with producers that prepare frames.

Labels: **VERIFIED** (seen in the unit's binaries or configuration, or
observed running), **INFERRED** (reasoned from verified facts),
**CAR-ONLY** (needs the vehicle).

## Why a service

The display path has three things only one process can hold:

- **The RAMSES identity.** The KOMBI's renderer reaches one unused head-unit
  participant, 801 ([abi/ABI.md](abi/ABI.md)); two processes cannot both
  be it.
- **The Dcsm2 client.** Each client handle has its own 10 s KOMBI watchdog
  and its own view of the slots ([dcsm/DCSM.md](dcsm/DCSM.md)); a client
  per app would have every app fighting every other over the same slots.
- **The link to the KOMBI.** About 11.5 MB/s of TCP for every head-unit
  participant together, the HMI's own content included; above about 8 MB/s
  of texture writes the KOMBI dropped scenes. Per-app budgets add up past
  it.

So the shape is a hybrid:

| Part | What | Who links it |
|---|---|---|
| `kit/` | the proven pixel path: 4:2:0 atlas, dirty tiles, packed blocks, SIMD conversion and rounding, pacer, frame clock, map geometry | the service, and any producer that prepares frames |
| `src/` | the service: slot arbitration, the layer stack, layers, the upload governor, the iDrive 7 and iDrive 8 backends, the socket | — (one process, `hmi_5000`) |
| `include/`, `client/` | the C++ client and protocol, `libcluster_render_client.a` on the unit | native producers |
| `js/cluster_render_client.js` | the Node 8 client | OAP apps |
| `dcsm/`, `abi/`, `tools/`, `docker/` | the Dcsm2 client, the units' RAMSES 19, RAMSES 27 and vsomeip ABI, the ABI checks and the unit's toolchain | the service's build |

What it costs:
- frames cross a socket. A whole 1088x480 frame is 783 KB, so 30 a second
  is about 47 MB/s of kernel copies, and Waze's 16 a second about half that;
- an app's content cannot use arbitrary RAMSES objects, only the layer
  kinds and assets below.

What it gives:
- no two apps fight over a slot, and clients come and go without unmapping
  anything;
- one budget, steered by what the link shows;
- clients that need no `hmi_5000`;
- a renderer crash in one app takes nothing else down;
- one place to port to iDrive 8, and ported there ("iDrive 8").

```
 app / producer                     cluster-render (hmi_5000)                      KOMBI / HUD
 ----------------                   ------------------------------------------     -----------
 js client / C++ client --J/F/M-->  server ── compositor ── layers (video, image,   renderer draws
   layers, frames, meshes             │         │  effect, mesh), assets: a scene   the scene in
                         <--events--  │         └─ governor ── pacer (one budget)   its slot
                                      └─ slot manager ── Dcsm2 port/link ──Dcsm2──>  slots
                                                         RAMSES 19 graph ──scene──>
```

## Arbitration

A **target** is one slot with one scene: `map` (0x100, the central map
area under the gauges, 1088x480), `panel` (0x203, the whole cluster over
the gauges, 1920x720), `hud` (0x323, 800x480). Every client's layers on a
target are drawn in that target's scene (`src/stack.h`, `src/slots.h`).
This section is iDrive 7's KOMBI; on iDrive 8 a target is a content the
cluster is offered, and what differs is in "iDrive 8".

- **Layered stack.** Every layer is drawn at once, bottom to top by class
  (ambient, content, overlay, alert), then the client's `z`, then the newest
  on top. A layer marked `exclusive` hides the layers below it while it is
  drawn. They are suspended, not removed, and come back in the flush it
  goes. A change of the stack is one flush: no display traffic, so a client
  replacing another is instant.
- **Refcounted slots.** A target is requested while at least one visible
  layer has content (or is `prewarm`) and is released only when none has for
  `<target>_linger_ms`, after the fade-out below. While it lingers the scene
  stays mapped, empty and transparent, so a client that comes back within
  it is shown at once. The map's linger is 0: it is unmapped as soon as its
  video has stopped and faded out.
- **Warm scenes.** A target's scene is published at its first request and
  stays published while the target is released, taken by the display,
  refused or not answered: only the Dcsm2 fetch and map cycle. A released
  target stages and flushes nothing, and its writes are not upload
  activity. Asked for again, its layers continue from what they hold: a
  renderer that subscribes gets the scene as it was last flushed, so a
  video writes only the tiles that differ from it. The scenes go at exit,
  and when a target needs a scene id another target's released scene has
  (`panel` and `hud`).
- **The KOMBI's queue** (VERIFIED on the car, slot 0x100, class 0,
  [dcsm/DCSM.md](dcsm/DCSM.md)):
  - The KOMBI never takes a slot from the content it shows. The first
    content mapped into a slot is shown (map status 1). A later map is
    accepted (`E_DCSM_OK`) and parked, invisible (map status 3). When the
    shown content is unmapped, the oldest parked one is shown within about
    0.5 s. The class does not change this for 0x100. Class 1 was parked
    even with the slot free, so the map's class is 0.
  - The slot table has one row per mapping: slot, content, class and map
    status, with the slot's status in every row. The row with map status 1
    or 2 is the one shown, and so the holder. Rows stay listed, unmapped,
    until their content is unfetched.
  - The stock map is content 0x01000300, class 0. In Map view the HMI maps
    it into 0x100.
  - A phone reconnecting or a door-open warning locks 0x100 (status 0), and
    the KOMBI unmaps every mapping in it, ours and the stock map's. A map
    into the locked slot is answered `E_DCSM_OK` and never shown. Once it
    is free again, the first to ask is shown: the HMI's map, which stays
    fetched through the lock and only needs mapping, is there before ours.
- **Parked.** A target the KOMBI parks stays mapped and fetched, with no
  timeout, and is `parked` (behind the holder the table names) until the
  KOMBI shows it. A parked target stages and flushes nothing. Nobody
  wanting it, it is released at once, with no linger.
- **Giving way** (contributed by [eroobeeroo](https://github.com/eroobeeroo)).
  A target with `<target>_yield_slots` (the HUD: `0x322`, where its media
  and list pop-ups show, VERIFIED on a G05) gives way while the stock HMI
  shows content there: asking for the target again while a pop-up showed
  pushed the pop-up from the HUD to the cluster. It is asked for again
  `<target>_yield_ms` (1500) after the pop-up goes, and an overlay-only
  target gives way to stock content parked behind it the same way.
  `<target>_yield_contents` gives way the same way to the stock contents it
  lists, shown in any slot: the HUD's is BMW's guidance, `0x01000210`, which
  does not show while our HUD scene is mapped even though it has slots of its
  own (an X3's HUD, Nov 7 2026: the layers kept clear of the turn map and it
  still did not show), so the HUD steps aside for the route and is asked for
  again `yield_ms` after it ends.
- **The HUD** (iDrive 7, `hud`): slot 0x323 only, the slot the HUD map of
  [eroobeeroo](https://github.com/eroobeeroo) was shown in on a G80 and a
  G05 (VERIFIED there), scene 0x01000400, class 0.
  - Another `hud_slot` or `hud_scene_id`, or `hud_take_over_stock` 1,
    leaves the HUD out: the
    service runs without it and logs why (`target hud left out: …`). It
    never takes a slot from the stock HMI, so BMW's own HUD content (its
    guidance, 0x01000210, in 0x330-0x332) is never unmapped; it gives way
    to the HUD's pop-ups (below).
  - Its scene id is `panel`'s: the two are never asked for at once
    ("Scene ids").
  - It has the HUD's share of the budget ("Upload budget"): up to half of
    it, only as much as it writes, and nothing it shows the governor moves
    the cap.
  - The mapping guard disables it after two strikes, and only it.
- **Take-over.** A target with `<target>_take_over_stock` (the map's
  default) takes the slot from the stock HMI, as long as a wanting layer
  has `contend`, such as the Waze map:
  - When the target is parked and the row shown in its own slot is the
    stock HMI's content, the service unmaps that content once with the
    HMI's client handle (`dcsm_hmi_client_handle`, 1). VERIFIED on the
    car: the KOMBI then shows ours, and the HMI maps its map again at once,
    parked behind ours. When ours is unmapped, the stock map is shown again
    within 1 s.
  - It is a race: about 100 ms after the unmap, either the KOMBI has shown
    ours, or the HMI's map comes back first and is shown again. On the car
    (2026-11-01 to 11-03) 10 of 33 take-overs lost it, up to four in a row,
    with the HMI mapping its map again within about 100 ms every time and
    nothing else changing.
  - Take-overs are 1.5 s apart four times, then 10 s, then every 30 s while
    the stock content keeps coming back on top; never two less than 1.5 s
    apart. The count starts over once the target has been shown for 10 s,
    and when it is released because nothing wants it (not a guide hold,
    whose map can come and go faster); a target shown and parked again
    sooner keeps backing off.
  - Never content of anything other than the stock HMI, never without a
    contending layer, never in any other slot. Each one is logged with
    what was unmapped, and so is the KOMBI's reply. Unmap replies are
    matched in a queue of their own, so they never answer a fetch or a map.
- **The stock HMI and other apps** without a parked mapping (not asked yet,
  or a target that does not take over): content of an app that does not
  use this service is always yielded to: the slot is asked for again only
  once the table says it is free. The stock HMI's (scene ids 0x0100xxxx
  that are not ours) is yielded to the same way, unless a wanting layer has
  `contend`. A contending layer whose request was refused or whose scene was
  lost retries after 2, 5, 10, 20, then every 30 s, and the ladder starts
  over after 30 s shown. Nothing is asked for while the table reports the
  slot disabled (`slot locked`), unless that report is 30 s old with none
  since; a request sent before or during the lock and still unanswered when
  the slot is free again is taken down and asked for again at once. A slot
  the table reports disabled or with another content mapped, and then free
  again (a door-open warning come and gone), is asked for at once, whatever
  step the ladder is on. After a lock, a target that takes over from the
  stock HMI is asked for as soon as the slot is reported not locked, even
  with stock content shown there (the car's door sequence: 0x105 from status
  0 straight to 2, the box shown), which it then takes over. A loss with the
  slot reported locked does not climb the ladder. Without a contending
  layer, a slot the table names as held is not asked for, even when a retry
  is due.
- **Refusals, failed fetches and no answer** (neither shown nor parked
  within 5 s) retry on the same ladder; they are not upload evidence. A
  shown target whose scene the display removes with nobody else in the slot
  (not locked, not our release) asks again on the ladder too, and no sooner
  than 2 s after the removal. A fetch that fails within 300 ms of a request made within 10 s
  of such a removal is the display not ready yet: asked again after 1.5 s,
  four times, before the ladder (the KOMBI once took 7 s to accept the
  scene again). Requests to a slot are
  at least 1 s apart, with one outstanding. A fetch failed within 2 s of the
  scene's publication (the renderer not having seen it yet) is asked again
  after 250 ms, three times, before the ladder.
- **Releases confirmed.** After a release, the next request of that scene
  id waits until the KOMBI confirms the release: the replies to the unmap
  and the unfetch, and the REMOVED of the fetch when there was one (about
  35 ms on the car). When subscriptions are observable (below) it also
  waits for the renderer to unsubscribe from the scene, which in the
  emulator follows the REMOVED by 150-170 ms. Without both within 1 s it
  goes anyway, and the log says which was missing. Every request after a
  release logs how long after it went.
- **Rendering verified.** The service reads the RAMSES client's console
  (`src/ramses_console.h`), where the library logs each renderer
  subscribing to or unsubscribing from our scenes, and writes those lines
  to its log. A shown target is `verified` once the renderer is subscribed
  to its scene, which it normally is before the display shows it. Once any
  subscription has been seen since start, a shown target whose scene is not
  subscribed 1.5 s after the display showed it (or after the renderer
  unsubscribed) is logged (`the display reports scene 0x10004f0 mapped but
  its renderer is not subscribed`), unmapped and unfetched, and asked for
  again once the KOMBI confirms that, at once the first time and on the
  ladder after that; its scene stays published. Until a subscription has
  been seen, nothing is verified and the status says `renderer subscription
  not observable`. The state is in each `target` event (`rendering`:
  `none`, `checking`, `verified`, `unobservable`) and in the status's target
  lines. The reader counts every console line by its context and the
  status says so (`renderer console: 1290 lines (ramses.Periodic 960,
  ramses.Client 3, ramses.Framework 1, other 326), the last 1 s ago`), so a
  console that goes quiet is told from a renderer whose subscriptions are
  not logged. The first 20 RAMSES lines other than the 2-second statistics
  (`ramses.Periodic`) go to the log as they are, within the 30 raw lines a
  minute.
- **Fades.** A target's video and image layers are drawn at their own
  opacity times the target's fade. The fade is 0 until the target is shown
  and rises to 1 over `fade_ms` (250) once it is. When nothing wants the
  target any more, the layers last drawn stay drawn and fall to 0 over
  `fade_ms`, and only then does the linger start. A fade steps at most
  every 50 ms, in the target's ordinary flush, with no pixel writes. Effect
  and mesh layers have no opacity and do not fade; a target with only those
  is released without one. A layer a client drops, or whose client goes,
  goes at once. A slot the display takes is gone without a fade, and so is
  everything on SIGTERM.
- **Guide hold** (iDrive 7, `guide_hold = 1`, the default). While `map`
  is shown, and only then, the service holds the slot of the stock HMI's
  CarPlay maneuver panel (content 0x01000310, class 0) with an empty 16x16
  scene of its own, never drawn, so the box is hidden on the cluster while
  the map shows. The box is in 0x105 on one car and in 0x103 on another,
  whose KOMBI reports 0x105 disabled and empty (both VERIFIED on the car).
  With `guide_slots = auto`, the default, the slot is the one the slot
  table reports the box in, or before the box shows, the one the HMI
  chooses (below), held with `guide_scene_id`
  (0x010004f1); a list of slot ids pins a hold to each instead, the first
  with `guide_scene_id`, each next with the next id (only 0x010004f1 is in
  the shipped `scene_ids_allowed`). VERIFIED on the car with the slot
  named (2026-11-03): the KOMBI takes 0x010004f1 in 0x105, the box is
  hidden, the HUD keeps its guidance, and the box is back after the
  release; on the other car, with `guide_slots = 0x103`, the box is
  hidden and a seat-belt warning still shows on the cluster with the map
  up (its HUD and a head-unit restart not checked). `auto` itself has not
  run on a car (`slots_test` only). The HUD's slots are not touched.
  - Each hold is a target of its own (`guide_auto`, or `guide_0x105` for a
    listed slot), in the status but in no client's hello or events, and no
    client can put a layer on it.
  - `auto`: the hold's slot is 0x105 or 0x103; no other slot is ever asked
    for, and `auto` is refused when another target has either. Before the
    box is mapped, the slot is the stock HMI's choice for it: 0x105 when the
    slot table reports it enabled (status 1 or 2), else 0x103 when that one
    is (`hold moves to slot 0x105, the first of the box's slots the slot
    table reports enabled ...`), so the hold is in place before the first
    route, as a fixed slot is. With neither enabled it is not asked for (`no
    slot: 0x105 0x103 not reported enabled`). A report with a content of
    `guide_take_over` mapped (shown or parked) in the other slot, the slot
    not disabled, moves the hold there (`hold moves to slot 0x103, where the
    slot table reports 0x1000310 ...`); shown wins over parked, then the
    slot it has, then 0x105. It is released from the old one first when it
    is held, so it never holds two slots, then asked for in the new one like
    any request
    (at least 1 s after the last, after the release is confirmed, and no
    sooner than a wait on the retry ladder it was in). The box unmapped (the
    route over) or mapped only in another slot, the hold keeps its slot.
    Each move counts as a loss towards the give-up (below), never as a
    fault, so a box that keeps moving gives the hold up. The mapping guard
    counts the hold as one mapping in both slots (`guide_auto auto 0x10004f1
    0`): a move never escapes a strike, and two in either or both slots
    disable it. The box's slot is in the log either way: `dcsm: slot 0x103
    status 2 content 0x1000310 class 0 map 1`. A car that needs a fixed slot
    sets `guide_slots = 0x103` (or any slot) in `cluster-render.local.conf`.
  - The map's clients choose: while any layer that wants `map` has
    `guide_hold` 0 (PROTOCOL.md), the hold is not asked for, and one held is
    released; the status names such a layer (`guide_hold 0`). A client
    that never sends the field gets the hold.
  - It is requested, contending and taking over from the stock HMI, as
    soon as `map` is shown. Parked behind the HMI's content, it takes over
    as the map does, but only content in `guide_take_over` (0x01000310;
    0x01000314, the seat-belt-warning panel, is refused there): it
    unmaps that content shown in its slot and that can be parked ahead of
    ours, so none of it is shown in its place; the HMI maps it again behind
    ours. VERIFIED on the car, with `panel` taking over in 0x105: with a
    route 0x105 shows 0x01000310 with 0x01000314 always parked behind it;
    one take-over of 0x01000310 has the KOMBI show ours, the HMI maps
    0x01000310 again behind ours within about 100 ms, and 0x01000314 stays
    parked. Which ones are ahead
    comes from when ours was asked for, since the order of the table's rows
    is not known to be the slot's queue: those mapped when it was asked
    for or while it was being fetched and mapped. Content mapped after ours
    is left parked behind it.
  - It yields to any other content shown in its slot (map status 1 or 2),
    such as the seat-belt-warning panel 0x01000314 during a warning, or
    when the KOMBI shows it instead of ours: released at once (`hold yields
    to …`), and not asked for while that is shown there. Other content
    only parked there (map status 3) is neither taken over nor yielded to.
  - More than 3 losses within 60 s (our scene removed, unmapped, parked
    again behind other content, or released to yield, not to a lock; or a
    move with `auto`) give it up until the service next starts (`hold given
    up`). When none of those losses was BMW's own content taking the slot
    (a yield, a re-park behind it, or our scene unmapped or removed while
    content off `guide_take_over` is shown there, such as the seat-belt
    panel) or a move with `auto`, the give-up is also a strike of the
    mapping guard against it (below).
  - The KOMBI's watchdog for our client is fed by our own heartbeats every
    250 ms (`dcsm/dcsm_client.cpp`); a take-over's unmap goes with the
    HMI's handle, so it does not feed ours (INFERRED: the KOMBI restarts
    the watchdog of the client handle a request names, `dcsm/DCSM.md`
    "Heartbeat").
  - Rendering verification never releases it: the KOMBI's renderer
    subscribing to an empty scene or not shows nothing either way.
  - It is released as soon as `map` is not shown (within three passes of
    the loop; one still being asked for first waits up to 5 s for the
    KOMBI's answer), and on stop; the KOMBI then shows the box again.
  - Its take-overs' count starts over only after 10 s shown, not when it
    is released, so the map coming and going keeps them on their 1.5 s,
    10 s and 30 s steps.
  - It is asked for only once a slot table since the display service came
    up has named its slot, and not while the latest report of the slot
    says disabled: a slot no table names (`slot not reported`, a slot the
    unit's coding does not have) or reported disabled (`slot locked`, a
    lock or a slot the coding does not use) is not asked for, however long
    ago that report came. After a lock it is asked for as soon as the table
    reports the slot not locked, and takes over the box shown there. A map
    the KOMBI refuses with E_DCSM_SLOTID_UNKNOWN is not asked for again
    until the service next starts, and is a strike against it. A door opened was seen on the car only with `panel`
    taking over in 0x105, a target that does not follow the map: the KOMBI
    disabled 0x105 and unmapped everything in it, and `panel` was asked for
    again and took over again by itself. That the hold, after such a lock,
    is asked for again and takes over again once the slot is free and `map`
    is shown comes from `slots_test` only.
  - The map shown and hidden quickly cannot make it flap faster than the
    slot manager's spacing: requests at least 1 s apart, take-overs at
    least 1.5 s apart.
  - The log has a line for each change of its state, each take-over (with
    what it unmapped, what it left parked, and the count since start) and
    each content of the HMI mapped again behind it, with our scene id and
    the slot's rows (content, class, map status: 1 shown, 3 parked, 0
    unmapped). `dcsm: slot ...` lines log every new or changed row of every
    slot the KOMBI reports, so the box's slot can be read from the log
    without the hold.
  - Settings take effect at the service's next start (`systemctl restart
    cluster-render`). `guide_hold` other than 0 or 1 counts as 1. A
    malformed slot, scene id, class or take-over list (`auto` with slot
    ids, a slot that is not whole hex from 0x1 to 0xfff, listed twice, or
    a scene id not in
    `scene_ids_allowed` or another target's) keeps the hold off, the safe
    choice: the service runs without it, and the log says why (`guide hold
    off: …`).
- **Mapping guard** (`src/guard.h`). A mapping the KOMBI cannot take can
  make it restart the cluster and the HUD (scene ids 7702 and 7703 did), and
  the service would map it again after every restart and every boot. Only
  a guide hold and the HUD can be disabled; the map and `panel` only have
  their strikes logged, and the map's mapping in 0x100 with 0x010004f0, the one
  the car has shown for longest, is never disabled whatever the file says.
  - A mapping event is our request of a target and the display's first
    showing of it after that request. A re-park or re-show the KOMBI makes
    on its own is none.
  - The display service (Dcsm2, or iDrive 8's DCSM) going away within 10 s
    of a hold's or the HUD's mapping event, held still or released since, is
    a strike against that mapping (its slot, scene id and class), even when
    the map had a later event; the map and `panel` are never blamed while
    such an event is in the window. With a hold's and the HUD's both in it,
    the latest is struck. With neither, the target with the latest event
    gets the strike, logged only. A KOMBI restart takes its SOME/IP
    service, and with it Dcsm2's TCP connection, away (INFERRED;
    CAR-ONLY). A scene removed, a silent heartbeat or a slot table without
    our rows also come from the stock HMI re-mapping its content, a full
    link or the KOMBI's own arbitration, so they are no strike by
    themselves. A restart that does not follow a mapping event within the
    window (the KOMBI also restarts on its own) is no strike.
  - A hold giving up on faults alone (more than 3 losses within 60 s, none
    of them BMW's own content taking the slot or a move, above) is a strike
    too, which covers a KOMBI renderer that restarts with Dcsm2 staying up,
    and so is a map refused with E_DCSM_SLOTID_UNKNOWN. A give-up with a
    yield among its losses (a seat-belt or passenger-seat alert that keeps
    coming back) only stops the hold until the service next starts.
  - Two strikes disable the hold's mapping: never asked for again, the
    box shown, the map's clients told (`guide_disabled` in the map's
    `target` event, PROTOCOL.md), and `status` says so (`guard: guide_auto
    auto 0x10004f1 0: disabled, 2 strikes`). Two strikes disable the HUD's
    the same way: never asked for again, however often its layers come and
    go, its clients told (`disabled` in the HUD's `target` event), the map
    untouched (`guard: hud 0x323 0x1000400 0: disabled, 2 strikes`).
  - A strike stops counting after three boots with no other against the
    same mapping; a disabled mapping stays disabled.
  - The strikes are kept in `guard_path`
    (`/var/opt/cluster-render/state/guard.txt`, written whole, renamed into
    place, its folder synced), so a boot never asks for a disabled mapping.
    A new cluster-render version starts them over. As root, `cluster-render
    guard-reset` removes the file and restarts the service if it runs; no
    client can reset it. A file that does not read starts over; one that
    cannot be written leaves the guard working from memory until the
    service stops.
- **Scene ids** come from `scene_ids_allowed`. Ids 7702 and 7703 made the
  KOMBI restart the cluster and the HUD. Two targets with one id (`panel`
  and `hud`) are never requested at once; the second's request unpublishes
  the first's released scene and publishes its own.
- **Our own id in the slot.** A target can be lost while the table still
  names its scene id. That is a report from before the loss, or the stock
  HMI's content with the same id (0x01000400 is also the speech dialog). The
  next report after our unfetch decides: still named, it is the stock HMI's;
  other content, its holder's; free, or no report within 1 s, the loss was
  ours. The ladder counts from the loss either way.
- **Recovery.** Dcsm2 going away takes every target down; the scenes stay
  published. When it comes back, the targets still wanted are asked for
  again after 1 s. A scene the display removes is asked for again on the
  ladder (2 s after the removal at the earliest), in the same scene, its layers continuing from what they hold. In
  a new scene each layer starts from what it last showed: the video's
  newest frame, written whole and exact as soon as the budget allows, its
  bytes charged to the budget as debt.

## Upload budget

Every layer of every client writes within one budget (`src/governor.h`,
`kit/upload_pacer.h`). Its ceiling, `upload_kb_per_s`, is 5500 KB/s: on the
car the cluster flashed black at 8000 KB/s, and once removed a shown map
after a minute averaging 5774 KB/s. The cap follows what the link shows while writes go (64 KB
or more written within the last second, whether or not the flush went);
evidence at other times is logged, not counted:

| Evidence in the last 30 s | Points |
|---|---|
| a shown scene removed by the display, with no other content in its slot and the slot not locked | 6 |
| heartbeat replies silent after replies came (the link stalled) | 3 |
| a flush or a setData refused | 2 |
| a flush that took `slow_flush_ms` (150) or longer | 1 |

- At 6 points the cap steps down 500 KB/s, to the 2000 KB/s floor at most,
  and the window starts empty. A removal reaches 6 alone: on the car the
  KOMBI removed a shown map, with healthy heartbeats and no lock, after the
  busiest minute of upload (5774 KB/s with a 1.3 MB whole-scene resend). No
  other single event does, so one transient error of another kind never
  lowers the cap.
- Another step down waits 15 s.
- The stock HMI or another app taking a slot, the display locking it (the
  slot reported disabled, as for a door-open warning), a refused request or
  one not answered is arbitration, not evidence.
- The cap steps back up 500 KB/s after each minute of healthy upload (at
  least 64 KB a second written, nothing in the window), to the ceiling
  `upload_kb_per_s` (5500). Idle time neither counts nor resets that minute.
- A step down within a minute of a step up doubles the minute (up to
  10 min); 10 min without a step down restores it.
- A removal that steps the cap down, or comes at the floor, holds the cap
  for 10 min before any step up: on the car the KOMBI dropped the map again
  within a minute of the cap going back up.

**The HUD's share** (`kit/upload_pacer.h` share_of, `TargetSpec::share`).
While the HUD is asked for and has a layer, its layers and assets write from
a budget of their own: up to half of each refill of the budget, but only as
much as they spend; what the HUD leaves goes back to the other targets in
the same refill, so a still or small HUD costs the cluster almost nothing
(while the cluster writes, the HUD takes at most half of each refill). The
cluster never gets less than half while the HUD writes, and everything when
it does not. While the
cluster's budget sits full, nothing written since a refill filled it (a
still cluster map, or the HUD alone), the HUD gets the whole refill: then
the whole cap. The two together never hold more than one burst (what the
HUD holds comes off what the cluster may hold), so together they send at
most the burst and the cap's rate over any time, as the cluster alone; the
ceiling is unchanged. With no HUD layer, the cluster's writes go exactly as
they would without a HUD, byte for byte and tick for tick
(`compositor_test`). The HUD's resend is charged to its share too, evidence
from it (a flush, a refusal, a scene removed) is logged, never counted, and
its writes are not the link's load the governor counts evidence and healthy
time by: a HUD the KOMBI drops never lowers the cluster's cap, and the HUD
alone writing never raises it.

Down is quick and up is slow, so the cap does not oscillate. Within the cap,
the top layers write first, and in a video layer a
frame whose exact write the budget cannot cover goes packed, the exact atlas
comes back when the budget allows, and only the newest frame waits.

The cap and why it is what it is are in the status
(`cluster-render status`, `/run/cluster-render/status.txt`: `upload: cap
5000 KB/s (ceiling 5500, floor 2000): lowered from 5500 KB/s: in 30 s, 2 x
scene removed`). Every client also gets a `budget` event when it changes.
Once a minute, each shown target logs what its layers wrote since its last
such line, and whether it is rendered: `target map: in the last 60 s its
layers wrote 1800 frames (18 ms from offer to flush on average), 52000 KB, 0
failed, 12 budget holds; rendered:
renderer 00000000-0000-0000-0000-000000000001 subscribed for 412 s`.

## Layers

| Kind | What the client sends | What the service does |
|---|---|---|
| video | 4:2:0 atlases (`w*h*3/2` bytes) | `src/video_layer.h`: one persistent R8 buffer and sampler; dirty 64x64 tiles over a noise filter, settled after `settle_ms`; packed 4x4 blocks in two persistent RGBA8 buffers for motion the budget cannot cover; exact return; the frame clock; newest-frame-only; checked `setData` and flush |
| image | RGBA rectangles that changed | one persistent RGBA8 buffer, written in those rectangles at most `fps` a second |
| effect | a GLSL ES 1.00 fragment shader, uniforms (arrays too), binds to assets | a quad drawn by it; uniforms in the next flush; `u_time` at `time_fps`, or from the renderer's own clock |
| mesh | parts: shaders, geometry (`M`), transforms, uniforms, binds to assets | a render pass of its own over its rect at its place in the stack, with its own perspective or orthographic camera (`src/mesh_layer.h`); each part's geometry immutable resources, replaced at most once a second within the budget; depth cleared, never colour |

Beside the layers:
- **Assets** (`src/asset.h`): RGBA8 or R8 textures a client uploads once
  and its effect layers and mesh parts on the same target sample, such as a
  glyph atlas or sprites. Not drawn, so they never make a target wanted;
  kept on the CPU, written whole into each new scene and in place as they
  change, before the layers, within the budget. A layer sampling one is
  drawn only while its buffer holds it whole.
- **Turns**: video, image and effect quads rotate and scale about a pivot
  (a needle), each under two group nodes in the scene.
- **Animations**: keys the renderer plays by itself (one real-time
  animation system per scene) on a quad's rotation, offset and scale, a
  part's pose, a mesh camera, or a uniform; nothing is sent per frame. Kept
  and started again in a new scene.

Each target's assets and layers are staged, assets first and then layers
top first, and flushed once a tick, so a tick's changes reach the display
together. A `setData` a layer cannot finish holds that tick's flush back.
A video layer or an exclusive image whose buffer that left half written
then holds the target: nothing else is staged or flushed until its whole
rewrite goes, as soon as the budget allows, so the cluster keeps showing
the last frame and never a half-written one. Any other image layer is
hidden until it is written whole again. A video layer's first frame in a
scene goes whole and exact even when the budget's allowance cannot cover
it, its bytes charged as debt.

Quads and mesh layers share one draw order. In RAMSES 19 each mesh layer is
a render pass with its own camera, so the quads between two mesh layers
share a 2D pass, and the passes' render order follows the stack exactly
(`plan_passes` in `src/graph.h`). Without mesh layers a scene is one pass.

## Lifecycle

- `cluster-render.socket` listens from boot; the first client starts the
  service. Clients reconnect by themselves (0.25, 0.5, 1, then every 2 s).
  They make their assets and layers again with their last settings:
  uniforms, mesh parts with their geometry and transforms, binds and
  animations; and send their images and assets whole, and a video layer's
  last frame whole in place of its next repeat.
- A client's layers go with its connection, then its assets. A video layer whose frames
  (or repeats) stop for `stall_ms` stops wanting its slot, which is released
  after it fades out and the linger.
- On SIGTERM every target is unmapped and unfetched, the guide hold's
  included. If the service dies, the KOMBI's watchdog drops its content
  within about 10 s and systemd starts it again: the watchdog is per client
  handle and unmaps every mapping of it (`dcsm/DCSM.md` "Heartbeat"), and
  the guide hold is a mapping of the same client as the map.

## Supported head units

Every head-unit variant runs, HIGH F included (0.3.9 refused it). On a
head unit whose variant cannot be read, a kernel command line with no
`hwvariant=` or two that disagree (`src/head_unit.cpp`, the rule
waze-on-cluster's patches follow too), cluster-render starts nothing that
reaches the cluster: neither the generation's service nor Dcsm2, vsomeip
or RAMSES. It
logs the reason once and stays up holding only its socket, so neither the
socket nor `Restart=on-failure` starts it again: a status request gets
`not supported on this car (head unit: ...)`, any other message an `error`
event with that reason, and the status file says the same. A SIGTERM ends
it with 0.

## Identity and packaging

`hmi_5000` (the one uid allowed Dcsm2 and the RAMSES offers), participant
801, vsomeip application `cluster-demo` and clientID `"\0cluster"`: the
identity proven on the car. A standalone renderer with the same identity
(`cluster-demo.service`) cannot run beside it, so the service conflicts
with it (`Conflicts=`). iDrive 8 takes the same uid and participant
("iDrive 8"), so one systemd unit serves both: it runs `cluster-render`,
which starts `cluster-render-idrive7` or `cluster-render-idrive8` in its
place.
`component.json` makes it a Store tool for iDrive 7 and iDrive 8
(`mguHmi: ["ID7", "ID8"]`), with the Node client shipped beside it for apps
(`/var/opt/cluster-render/current/cluster_render_client.js`).

## Install

From the Store ([robertnisipeanu/mgu-store](https://github.com/robertnisipeanu/mgu-store))
on the head unit: add the GitHub source `robertnisipeanu/idrive-apps`
(a private repository also needs a GitHub token the Store holds). The Store
reads the `catalog` release's `catalog.json` and installs the newest
version as a root tool, after the deploy tool it requires: the binaries,
its settings and the Node client in `/var/opt/cluster-render/`,
`cluster-render.socket` enabled. The first client starts the service. The
Store can remove it again. The same kit installs on iDrive 7 and iDrive 8;
the service tells which it runs on.

For development, the deploy tool of mgu-store installs a kit built from the
working tree over SSH (from the repository's root):

```sh
node release/tool_artifact.js native/cluster-render --out /tmp/kit   # runs make target, writes /tmp/kit/cluster-render@<version>.tgz
<mgu-store>/deploy/hu install --artifact /tmp/kit/cluster-render@<version>.tgz
```

## Configure

[`cluster-render.conf`](cluster-render.conf) lists every setting with its
built-in default: the generation (`auto`, `7` or `8`), the identity, the
targets (slot, content class, scene id, size, linger; on iDrive 8 the
`id8_` ones: category, content, scene id, size, linger, and the widget's
menu entry), the guide hold, the upload budget and the tick. A unit's own settings go in
`/var/opt/cluster-render/cluster-render.local.conf`, which updates keep;
each key there replaces the shipped value. `-s key=value` on the command
line overrides both. `cluster-render status` prints the targets, Dcsm2's
availability, the subscription's answer per event and the count of what
Dcsm2 sent (fetch, map and slot events, notifications that did not decode,
replies), every client's layers and the upload cap.

Every log line also goes to `/var/opt/cluster-render/log/cluster-render.log`
(`log_file`) with its UTC time: the unit's journal keeps only minutes, and
the file outlives restarts and reboots. Past `log_file_kb` (512) the file
becomes `cluster-render.log.1`, so it takes at most about twice that. The
service makes the directory, owned by `hmi_5000`, at each start; the deploy
tool removes it with the rest of `/var/opt/cluster-render` on uninstall.
For Dcsm2 the log has each change of availability and of the subscription's
answer, notifications that did not decode (their start in hex, at most every
10 s per event id), and, for a request that got no answer, what arrived
since it and the last slot table.

## iDrive 8

iDrive 8 draws its cluster and HUD with RAMSES 27 (`.so.27.0`). That
library has DCSM built in over SOME/IP RemoteContentManager (0xB0A3),
beside RamsesService (0xB0BE): the head unit offers content in a category,
and the cluster decides when to show it. On iDrive 8, uid 5000 has no Dcsm2
and no RAMSES 19 services, and nothing on the unit links the RAMSES 19
library it also ships. So the service has a second pair of backends, and
everything above them stays the same ([abi/ABI27.md](abi/ABI27.md) has
the library):

- **Two programs and a starter** (`src/unit.h`). The two libramses export
  the same names, so they cannot share a process, and an iDrive 7 unit has
  no RAMSES 27. `cluster-render`, which links neither, starts
  `cluster-render-idrive7` (RAMSES 19, Dcsm2) or `cluster-render-idrive8`
  (RAMSES 27, DCSM) from its own folder with its own arguments, in its own
  process (exec). `generation` (`auto`, `7`, `8`) decides; `auto` tells the
  unit as mgu-store's deploy tool does: MainApp's folder
  (`/usr/lib/oap-mainapp` iDrive 7, `/usr/lib/oap-mainapp21` iDrive 8), and
  with both (as on the iDrive 8 unit, VERIFIED) or neither, the running HMI
  (`HmiMain3D`, `Hmi8`). Not known yet (at boot), it asks again every 5 s,
  and `cluster-render-idrive8` tries a failed connect again every 5 s:
  neither exits for systemd to restart it (CAR-ONLY below).
- **The graph** (`src/graph_ramses27.h`): the same scenes as on iDrive 7,
  on RAMSES 27's API: effects and array resources per scene, texture
  buffers written with `updateData`, a flush with only a tag. No pass
  clears colour: the cluster draws the scene into its own area (VERIFIED on
  the car with icwidget, a test widget of ours whose moving text left no
  trail).
- **The port** (`src/dcsm27_port.h`) on the library's `DcsmProvider`. A
  target's slot is a category and `<target>_content` the content offered
  in it, as the target's scene:
  - A request is an offer (with metadata for a widget). Accepted, the
    target is `parked`: offered, not shown, with no timeout. The cluster
    shows some categories by itself (the middle, the HUD, the guiding
    panel) and a widget once it is picked in its widget menu (VERIFIED on
    the car).
  - `contentReadyRequested` is answered at once (the scene is published
    before the offer); `contentShow` is shown; `contentHide` and
    `contentRelease` park it again and keep the offer. The cluster takes a
    shown widget back on every layout change and shows it again 1 to 7 s
    later (VERIFIED on the car).
  - A release is `requestStopOfferContent`, confirmed once the cluster
    accepts the stop. The library refuses a new offer of the content until
    then (even from inside `stopOfferAccepted`), so the next request waits
    for the confirmation (at most 1 s, as on iDrive 7), and one that comes
    anyway is refused and waits on the ladder. The scene stays published,
    warm, as on iDrive 7. A scene taken down while its offer stands or is
    stopping stays published until the stop is accepted (unpublished at
    once, the emulator's stand-in cluster logged errors unassigning it), and
    no new scene with its id is built meanwhile. At shutdown the service
    waits 1 s for the stops.
  - Rendering is not verified: the RAMSES 27 console is not read, so the
    status says the renderer's subscription is not observable.
  - A refused offer (another provider has the content) waits on the
    ladder. There is no slot table, no take-over and no heartbeat: parked
    behind other content and the take-over are Dcsm2's. A cluster that
    goes away takes its assignments back (`contentRelease`), and the offer
    is there when it comes back.
  - The area the cluster gives each content (canvas, category rect, safe
    rect, layout) is logged when it changes, and a canvas other than the
    target's size as a warning.
- **The identity** is iDrive 7's: `hmi_5000` and participant 801. On
  iDrive 8, 801 is the communication user `MGUTestUser_1` (RamsesService
  instance 7, DCSM 5), and its offers reach the cluster (VERIFIED on the
  car). The unit's vsomeip policies let uid 5000 (as Hmi8 runs) and 6031
  (`nav_didui`, DidUi's) offer both services, and not root (VERIFIED). As
  `hmi_5000` the service connected and its offers were shown, in the
  emulator and on the car (VERIFIED, run by hand).

The targets on iDrive 8 (the `id8_` settings), each VERIFIED on the car
with this category, content, size and scene id (offered as user 801, shown
by the cluster), but `guide`: its category, size and place VERIFIED with
another content, its own content and scene id CAR-ONLY (below):

| Target | Category | Content | Size | Where |
|---|---|---|---|---|
| `map` | `Ca_ComCenter_IC` 43100201 | 43204041 | 874x749, all shown | the middle of the cluster, between the gauges; the cluster draws its media placeholder over it |
| `hud` | `Ca_ComCenter_HUD` 43100202 | 43204012 | 353x203, all shown | the HUD, above its speed bar |
| `guide` | `Ca_GuidingPanel_IC` 43100401 | 43204051, a dynamic content | 702x213, all shown | the top centre of the cluster, over the map in the normal layout |
| `widget` | `Ca_Widget_IC` 43100301 | 43204001, a dynamic widget (43202001-43205000) | canvas 2240x896, most of it shown (below) | the middle of the cluster once picked in its widget menu, with the entry `id8_widget_description`, `_order` and `_preview` |
| `popup` | `Ca_Widget_IC` 43100301 | 43204003, a dynamic widget | canvas 2240x896, as `widget` | the middle of the cluster, on a black middle, by itself while a client draws on it: a focus request with the offer |

- Target names are iDrive 7's where the place matches, so a client asking
  for `map` gets the cluster's middle. waze-on-cluster stays iDrive 7 only:
  its map is 1088x480, not the middle's 874x749. iDrive 8 has no
  `panel`: a layer on it is refused as on any unknown target. The hello
  names the backend (`id8`) and the targets with their sizes.
- The widget's safe rect is in canvas coordinates and moves with the
  cluster's layout (layout 4: 630x570 at 805,125; layout 5: 612x527 at
  814,170; VERIFIED on the car). The journal logs it; clients are not told
  it yet.
- The widget's canvas is the whole cluster, and far more of it than the
  safe rect is shown: behind the gauges, dimmed by the cluster's own gauge
  panels and bottom bar, as the stock navigation map is. Shown, in canvas
  coordinates: about x 400-1720, y 143-765 in the normal layout and x
  690-1530, y 185-700 in the M layout, with a black notch at the top middle
  down to about y 167 (VERIFIED on the car with a grid drawn over the whole
  canvas).
- A widget's offer can carry its background (`id8_<target>_background_id`)
  and the cluster views it is shown in (`_layouts`), and ask the cluster to
  show it (`_focus` 1: `enableFocusRequest` with each offer). VERIFIED on
  the car (an M4 in its ROAD view, layout 4) with icwidget, a test provider
  of ours making the same calls as communication user 802:
  - a focus request on a widget nobody picked: the cluster switched to it
    by itself, asking for it 0.1 s and showing it 0.4 to 0.9 s after the
    offer; it answers with the widget's focus status (focused). A new
    request (another handle) takes the widget back and shows it again
    within 0.25 s;
  - background 43400001, 43400004, 43400005 or 43400006: the cluster blacks
    out its own artwork in the middle behind the widget, the gauges stay,
    no new area is sent; 43400002 and 43400003 changed nothing visible;
  - layouts is a mask of the cluster's views: 1 (ROAD) kept the widget
    shown, 2 alone made the cluster take it away at once, 255 brought it
    back; the M SPORT view is layout 5 (category rect 670x527 at 785,170);
  - a big picture fits best at 1140x480 at 550,220 in layout 4: centred on
    the cluster (x 1120), its top and bottom edges shown, its sides faded
    behind the gauge panels;
  - nothing visible from the displayed-data flags (bits 0-31), the
    exclusive background, the car model's visibility or id, or the
    configurator priority (a member of the unit's DcsmMetadataCreator not in
    the open-source headers); the voice assistant's category
    (`Ca_IPA_IC` 43100501) and `Ca_Widget_L_Mutex_ComCenter_IC` 43100341
    were never assigned to a dynamic content.
- A second participant with the service's communication user gets no
  answer from any other participant (no offers seen, nothing assigned;
  VERIFIED on the car with the service running as 801): a test provider
  run beside the service takes another one (802, the second test user,
  VERIFIED on the car).
- In `Ca_GuidingPanel_IC` the cluster showed, by itself and at the place
  in the table, the head unit's own content `C_GuidingPanels_IC` 43201101
  offered as user 801 (VERIFIED on the car: canvas 702x213, the category
  rect the whole canvas, no safe rect). DidUi
  offers that content itself during route guidance, so `guide` offers a
  dynamic content in the same category instead; whether the cluster shows
  it, and which of the two it shows during guidance, is CAR-ONLY.
- Content ids the head unit offers itself (`C_NaviMap_IC` 43200501,
  `C_GuidingPanels_IC` 43201101, `C_Media_IC` 43202120, ...) are never
  offered: two providers of one content fight. Scene ids are `0x4d4755……`
  ("MGU"), allowed by `id8_scene_ids_allowed`.

VERIFIED on the car with the service itself (before 0.2.2 was merged),
run by hand as `hmi_5000` with `showcase.js all` on `map` and on `widget`:
- `map` offered, parked and shown within 1 s; every layer kind drawn:
  plasma, dial and needle, the lit cube, glyph text, the video and the
  exclusive alert;
- `widget` listed in the cluster's widget menu without a picture, picked
  and shown, with the same layers;
- a release and a request 30 ms later: the cluster accepted the stop
  within about 30 ms, and the next offer followed after the 1 s request
  gap and was shown;
- SIGTERM: the offers stopped, exit 0, the head unit kept running;
- with the same build, a video client drew a camera stream in `widget`,
  hardware-decoded, at 15 fps.

VERIFIED on the car (an M4) with 0.4.1, run by hand as `hmi_5000` beside
the installed 0.4.0 (communication user 802, its own socket, `id8_targets =
popup`) with `showcase.js all` on `popup`, the widget picked in the
cluster's menu another one: `popup` offered, asked for, and shown by the
cluster by itself 0.44 s after the offer (focus status focused), every
layer shown; the client gone, its offer stopped after the linger and the
stop accepted within 60 ms, and the cluster showed the picked widget
again; SIGTERM: stopped, the head unit kept running.

CAR-ONLY on iDrive 8:
- the release since 0.2.2 (host-tested only, not in the emulator): the
  scene kept published, warm, and the next request waiting for the stop's
  acceptance;
- the `hud` target, and how the upload budget fits the cluster's link
  over a long run;
- how a focus request, the backgrounds and the views mask behave on other
  iDrive 8 cars than an M4;
- the `guide` target: its dynamic content shown in `Ca_GuidingPanel_IC`,
  and which content the cluster shows there during route guidance, when
  the head unit offers its own guiding panel in the same category;
- how long the cluster shows the middle and the HUD, and what takes them
  away (guidance, media, a call);
- whether RemoteContentManager has a watchdog (none seen in the library: an
  offer stands while the participant is connected);
- what the unit does when `cluster-render.service` exits or is restarted:
  on the car, stopping a service restarted the head unit. The service
  waits for the generation and for RAMSES instead of exiting; a
  configuration fault or a refused communication user still exits 1.

## Clients

- **Native producers** link `libcluster_render_client.a` and include
  `include/cluster_render/client.h`.
- **OAP apps** copy `js/cluster_render_client.js` into their sources (Node 8,
  only the `net` and `events` builtins).
- Any other language speaks [PROTOCOL.md](PROTOCOL.md) over the socket.

A client needs only the socket's group, `tm_oap_app`, which every OAP app
has: no participant, no Dcsm2 client and no `hmi_5000`.

- **waze-on-cluster** ([native/waze-on-cluster](../waze-on-cluster))
  draws the CarPlay map: its map feed hands each decoded atlas to a
  `cr::Client` video layer on `map` with `contend`. It needs only the
  `video`, `mgu_ufipc` and `tm_oap_app` groups, holds no participant or
  Dcsm2 client, and requires cluster-render through its `component.json`
  `requires`.
- **`js/showcase.js`** shows every layer kind on a target: a 3D cube in a
  mesh layer the renderer turns, plasma in an effect, labels in effects
  over a glyph-atlas asset, a needle, an alert and a video.

## Building and testing

In `native/cluster-render`:

```sh
make test                       # every host test (below)
make test-x86                   # the C++ tests with the unit's gcc 7.5 in Docker (SSE2 paths)
make target                     # build/target/cluster-render, cluster-render-idrive7, cluster-render-idrive8
                                # and libcluster_render_client.a, every import checked against the units' libraries
make test-ramses ROOTFS=…       # every layer kind on the unit's own libramses
make test-ramses27 ROOTFS=…     # the same on an iDrive 8 unit's libramses 27
```

The host tests need a C++14 compiler and Node; the rest need Docker
(linux/amd64, the image `cluster-render-build` from `docker/Dockerfile`).
`ROOTFS` is a copy of the unit's root filesystem, which is not part of this
repository.

In an emulator of the unit whose stand-in KOMBI has no slot 0x100, run the
binary as `hmi_5000` with `-s map_slot=0x201`. In an emulator of an
iDrive 8 unit with a stand-in cluster for a category, run `cluster-render`
as `hmi_5000`: it starts the iDrive 8 service.
`js/showcase.js` puts content of every layer kind on a target through
the service and prints every event: `node showcase.js [--socket PATH]
[all|cube|text|needle|plasma|alert|video] [--target panel|map|hud|guide|widget]
[--size WxH]` (iDrive 8's `map`: `--size 874x749`).

The host tests:
- `log_test`: the log file's timestamps, start line, rotation to `.1` and a
  path that cannot be opened;
- `slots_test`: arbitration, on slot tables of one row per mapping as the
  car reports them: parked targets, take-overs and their spacing, locks
  and the car's lock and take-over sequences;
  warm scenes and a shared scene id, requests waiting for a release's
  confirmation, quick retries of a fetch failed after the publication, the
  rendering verdicts and their recovery, and the guide hold following the
  map: held only while it is shown, released when it is not, not asked for
  while its slot is disabled, held again after a door lock once the slot is
  free and the map shown, again at once after a second lock, its take-overs'
  count starting over after 10 s shown, the stock contents ahead of it taken
  over and those behind it left, the HMI mapping it again logged and taken
  over no more, the seat-belt panel left alone while parked and yielded to
  while shown, never released by rendering verification, and no flapping;
  never asked for while the table reports its slot disabled, however long
  ago, or does not name it; the HUD under the guard: a display drop after its
  mapping striking it and not the map, also with the map mapped after it,
  two disabling it and only it, in that boot and the next, wanting it again
  not undoing that; `panel` and `hud` never asked for at once; the mapping
  guard: the hold, released since,
  blamed for the display service going away over a map asked for after it, a re-park and
  re-show by the display no mapping event, a slot refused as unknown a
  strike and never asked for again, a give-up on unmaps alone in each of
  two boots disabling it, give-ups on yields to the seat-belt panel in two
  boots no strike; with `auto`, the hold in 0x103 or 0x105, whichever has
  the box with the other disabled, and before the box in 0x105 when it is
  enabled, else 0x103, never in any other slot or a disabled one, moving
  with the box and keeping a ladder wait, a box that keeps moving giving
  it up with no strike, and Dcsm2 drops after its mappings in both slots
  disabling it at the second, in that boot and the next;
- `ramses_console_test`: the RAMSES console lines the car's libramses
  prints (subscriptions, unsubscriptions, a renderer gone, resource
  requests), which lines are kept raw, and the raw lines' rate limit;
- `governor_test`: the budget's steps, cooldown, recovery and probation;
- `map_upload_test` (the kit's): the HUD's share: its source deciding
  exactly as alone with the share off, never more than half for a greedy
  share and never less than half for the source, an unspent share going
  back, an idle source's whole rate for it, within a burst, and the two
  together never holding more than one burst (both still, then both
  greedy);
- `compositor_test`: the map's flushes byte for byte as without a HUD
  while the HUD has no layer; with a HUD layer and the map moving at the
  cap, at least half the bytes for the map and at most half for the HUD at
  its 10 a second; the HUD alone at its rate, within the cap;
- `compositor_test`: layers on a fake renderer that keeps pending and
  flushed state apart. It covers dirty, packed and exact writes, failure
  parking, the stack and the closed budget loop: slow flushes lower the cap
  and the bytes written follow it. Also assets (written before the layers
  sampling them, charged to the budget, bound again, refused as a drop
  while sampled), mesh parts and their looks, geometry paced and replaced
  at most once a second, passes following the stack with an exclusive
  layer, turns, uniform arrays, animations, and a scene published again
  rebuilding all of it; fades in and out and their timing, a parked
  target staging nothing, and a released target's warm scene; the limits
  (64 layers a client, 96 in all) and 96 small layers changing every tick
  past the 5500 KB/s ceiling, written within it, each an even share;
- `dcsm2_port_test`: the Dcsm2 request sequence, parked and promoted
  mappings, take-overs, reply matching and refusal kinds; the undecoded
  notifications' rate limit, subscription answers, the no-answer line and
  the status text; releases confirmed by REMOVED or the unfetch's reply;
  slot rows logged for slots no target uses;
- `dcsm27_port_test`: iDrive 8's offers under the slot manager: parked
  offers, ready, show, hide and release, a stop and its scene kept until it
  is accepted, a request while it is not, refusals, two targets;
- `generation_test`: telling iDrive 7 from iDrive 8 by MainApp's folders and
  the running HMI;
- `holds_test`: the HUD's rules (0x323 and its share, any other slot or a
  take-over refused); the guide hold's settings, on as shipped, `guide_hold`
  other than 0 or 1 taken as 1 with a warning, and every other malformed
  value refused with no hold added (a slot that is not whole hex from 0x1
  to 0xfff, listed twice or another target's, a scene id not allowed or
  another target's);
- `guard_test`: the mapping guard's file: a strike read back, a hold's
  mapping disabled at its limit and after a reboot, the default limit never
  disabling, the map's mapping never disabled whatever its limit or the
  file, another version started over, strikes expiring after three clean
  boots while a disabled mapping stays, the file removed, a file that does
  not read or cannot be written;
- `service_test`: the whole service over its socket with the C++ client,
  on a fake KOMBI that parks and promotes as the car's does (the map taking
  over from the stock map and falling back to it, and asked for again in
  the same scene once the KOMBI confirms its release),
  including a restart under the client that makes assets, binds, uniform
  arrays, mesh parts, geometry and animations again, and malformed or
  oversized mesh messages and bad keys refused; the guide hold on a KOMBI
  showing the box: no client's target, held with the box parked behind it
  while the map is shown, and with the seat-belt panel parked there,
  released with the map and at stop, released and taking nothing over while
  the map's layer has `guide_hold` 0 and held again with 1, held for a
  client that never sends the field; the mapping guard: a cluster restart
  right after the hold's mapping strikes the hold and not the map, also
  while the hold is still being asked for in the tick its map was shown, two
  disable it (the map shown without it, also after a service restart, and
  the map's client told), a restart 15 s after any mapping is no strike,
  the map's strikes never disable it, and a client's `guard_reset` is an
  unknown op;
- `js/test/client_test.js`: the Node client against `host_service`,
  assets and a mesh layer included.

## Verified, and open

Verified off the car:
- **The kit.** waze-on-cluster built from the kit gives a binary whose
  loaded sections are byte-identical to its 0.4.24 release.
- **Host tests.** Every one above passes on the host and with the unit's
  gcc 7.5 (`make test-x86`).
- **The unit's own libramses 19.0.14** (`make test-ramses`) took everything
  the service does with no error logged:
  - every layer kind, and the effect shaders compiled by its glslang;
  - whole, partial and packed video writes and the return to exact;
  - draw-order changes, an exclusive layer and removals;
  - a scene published again;
  - perspective and orthographic views between quads in five passes, one
    reaching past the target's edge and one moved off it, reordered into
    three;
  - parts with every attribute and every uniform semantic (`u_normal` as a
    mat4), depth, culling and blending, geometry made and replaced;
  - R8 and RGBA8 buffers, repeating and clamped, sampled by one effect with
    a vec4 array, and mat4, vec3 and float uniforms;
  - turned and scaled quads;
  - animations of every property, linear and step, on quads, parts and
    cameras, uniforms included, stopped and dropped with their objects;
  - through the compositor, a lit cube turning in a mesh layer, labels over
    a glyph-atlas asset, a needle sweeping and plasma on the renderer's
    clock, all back after the panel was taken and published again.
- **The target binary** links against the unit's libraries within its
  glibc 2.27 and libstdc++ limits.

Verified in an emulator of the unit: its stand-in KOMBI renderer and Dcsm2
provider (not the KOMBI), with the map on 0x201 and clients using the Node
client under the guest's Node 8.
- **Startup and drawing.** The service joined as participant 801, found
  Dcsm2, and drew on both targets at once: a 15 fps video with an overlay
  image on `map`, and a uniform-driven effect on `panel`.
- **Requests.** Each target took one fetch, one map, one unmap and one
  unfetch.
- **A client leaving.** Its panel lingered 3.0 s and was released; the map
  sent nothing.
- **An exclusive alert.** It suspended the video, which was back within
  2 ms of the alert going, with no Dcsm2 traffic.
- **Frames stopping.** The video stalled after 6 s and the map was released
  after its linger.
- **Shutdown.** SIGTERM shut down cleanly.
- **Warm scenes and rendering** (the car's libramses build, a video client
  on `map` wanted for 12 s and gone for 10 s, three times): the scene was
  published once and unpublished only at SIGTERM. Each time the client came
  back the service asked again (the KOMBI stand-in had confirmed the
  release), the renderer subscribed 45-105 ms after the fetch, and the
  service logged the map rendered 200-400 ms before the display showed it.
  Screenshots showed the client's frame each time. After each release the
  renderer unsubscribed 150-170 ms after the REMOVED. RAMSES's console was
  at info, its lines left stdout, and the subscription lines reached the
  service's log.
- **3D and the rest** (`js/showcase.js` on `panel` and `map`, and one-off
  check clients for depth, the cut view, angles and mat4 order, checked on
  screenshots):
  - **Meshes.** A lit cube turned by the renderer draws with depth: a
    smaller cube behind it is hidden. Its pass leaves the colour below it
    and stays under a label drawn over it. An exclusive alert hides it, and
    a view cut at the target's edge draws pixel for pixel as the uncut part.
  - **Turns** go clockwise on screen for video, image and effect quads.
    Mesh and camera angles are right-handed, posed or animated.
  - **Text and needle.** Glyph-atlas text with a sprite updates once a
    second, and the needle sweeps about its pivot.
  - **Clock.** `u_time` runs on the renderer's clock: with only animations
    running, no flush is sent for 14 s.
  - **Matrices.** A mat4 uniform is read in column order.
  - **Restart.** Everything is back after the service restarts under the
    client.

Not verified, needs the car:
  - how the KOMBI's renderer draws the same: the extra passes and their
    depth clear, render order, the cut view, and mat4 order on its GPU;
  - animations against the KOMBI's clock;
  - slot 0x100, and the map and the panel shown at once, from one
    participant;
  - fades on the KOMBI. The take-over and the parked state are VERIFIED
    on the car (above), and the HMI took up to five take-overs in a row
    within about 73 s (0, 3, 13, 43 and 73 s); the emulated HMI never takes
    a cluster slot;
  - the take-overs' 1.5 s pace: five within 6 s;
  - what the HMI does after a take-over in views other than Map view;
  - heartbeat replies: the emulator does not answer heartbeat requests;
  - 0x01000400 on the panel while the HMI's speech dialog exists;
  - how a flush's duration and the heartbeat replies behave as the link
    fills, which sets `slow_flush_ms`;
  - the governor's steps on a real drive;
  - the Waze map and other clients through the service at once;
  - the KOMBI's renderer subscribing to and unsubscribing from our scenes
    as the stand-in does: the subscription before the display shows the
    scene (verification), and the unsubscription within 1 s of a release
    (the re-request's wait). The same library logs both on the car, but
    when the KOMBI subscribes has not been seen there;
  - the guide hold: `guide_slots = auto` choosing the box's slot (VERIFIED
    only with 0x105 and 0x103 named by hand), the box's slot and whether
    the KOMBI takes 0x010004f1 there on other units (the mapping guard
    disables a hold the KOMBI restarts on), and the hold after a
    door-open lock (seen only with `panel`). If the KOMBI ever shows the
    parked 0x01000314 instead of ours after a take-over, the hold yields,
    and that panel stays shown until the HMI parks it again; on the car
    the KOMBI showed ours;
  - the mapping guard's signal: that a KOMBI restart takes Dcsm2 away
    (INFERRED; the emulator's stand-in KOMBI does it on demand);
  - whether a hold's loss is a fault or BMW's content is judged from the
    slot table at the loss, which can lag the KOMBI;
  - `cluster-render guard-reset` restarts the service with no recmgr check:
    run it only on a unit that has booted with cluster-render installed;
  - that the KOMBI draws a warm scene asked for again. Once on the car, a
    scene destroyed and published again with the same id within a second
    was reported mapped and never drawn; `panel` and `hud` handing their
    shared scene id over still do that.

On iDrive 8 ("iDrive 8"), verified on the car with icwidget, a test widget
of ours on the same calls (RAMSES 27 scene, DCSM offer, user 801), as
`nav_didui`, 2026-10-25 and 27:
- the cluster takes offers from user 801: the widget in its widget menu,
  and in `Ca_ComCenter_IC`, `Ca_ComCenter_HUD` and the other categories of
  the table, shown by the cluster by itself;
- the areas: canvas, category rect and safe rect (canvas coordinates) per
  layout; the widget taken back and shown again on a layout change;
- a scene of ours drawn in the widget with no colour clear: moving text
  left no trail;
- the stop: offer stopped, accepted, the scene unpublished, the process
  gone without a crash (any crash on the unit restarts the head unit, so
  `cluster-render-idrive8` leaves with `_exit()` once the offers are
  stopped).

Verified off the car:
- **The unit's own libramses 27.0.116r1** (`make test-ramses27`, on the
  emulator's copy of the unit's root filesystem): every check of
  `make test-ramses` passes, nothing logged as an error.
- **The target binary** loads with the unit's own loader and libraries,
  every symbol bound (`LD_BIND_NOW`).
- **iDrive 7 unchanged:** its program builds from the same sources with
  the same checks, and `make test-ramses` passes on the iDrive 8 unit's own
  copy of libramses 19 too (another build than iDrive 7's).

Verified in an emulator of the iDrive 8 unit (its root filesystem with
Hmi8 running, its vsomeip and policies; a stand-in cluster, a DCSM consumer
and renderer on the unit's libramses, for one category: no KOMBI):
- **Start.** `cluster-render` found iDrive 8 (both MainApp folders, Hmi8
  running) and started `cluster-render-idrive8`, which connected as user
  801 as `hmi_5000`.
- **`map`.** Offered in `Ca_ComCenter_IC`, parked, asked for, ready and
  shown about 125 ms after the offer; `js/showcase.js all` on it drawn by
  the stand-in (plasma, dial and needle, the lit cube with depth, text over
  a glyph atlas, video, then the exclusive alert); checked on a screenshot.
- **`widget`.** Offered with its menu entry (the stand-in got "Cluster
  renderer", order 50) and shown.
- **Release and shutdown.** The client gone, the offer stopped and
  accepted, then the scene unpublished, with no error from the stand-in;
  SIGTERM exited 0. (Before 0.2.2's warm scenes were merged: a released
  scene now stays published until shutdown.)

Not verified, needs the car (iDrive 8): the CAR-ONLY list of "iDrive 8".

## Releases

A version is `component.json` `version` (`x.y.z`), published once; the
release sets it ([`CONTRIBUTING.md`](../../CONTRIBUTING.md)). The
repository's release tooling (`release/`, its README) builds, checks and
publishes cluster-render with the repository's other components, from the
repository's root:

```sh
node release/release.js check cluster-render              # build twice, compare the artifacts byte for byte
node release/release.js publish cluster-render --dry-run  # what publish would do
```

Builds run on a `git archive` export of HEAD, never the working tree, with
`SOURCE_DATE_EPOCH` the time of the last commit that touched what ships
(everything but `test/` and the notes at its top level).
`release/tool_artifact.js` runs `make target` and packs the tool kit
`cluster-render@<version>.tgz` in the deploy tool's format (mgu-store's
`deploy/hu artifact` packs the same bytes): its `component.json` (each
file's sha256, `mguHmi`), `files/` and `units/`, sorted, owned 0:0, one
mtime, gzip without a name or time.
Node's bundled zlib is the one outside input, so CI pins Node 22.12.0.

The repository's catalog (schema 2) lists cluster-render with `requires`,
`uninstallable` and `mguHmi` from `component.json`:

```json
{ "id": "cluster-render", "kind": "tool", "title": "Cluster renderer", "description": "…", "version": "0.1.0",
  "artifact": { "name": "cluster-render@0.1.0.tgz", "size": 2242698, "sha256": "…",
                "url": "https://api.github.com/repos/robertnisipeanu/idrive-apps/releases/assets/<id>" },
  "requires": { "mgu-deploy": "0.4.0" }, "uninstallable": true, "mguHmi": ["ID7", "ID8"] }
```

mgu-deploy checks the kit's `mguHmi` from 0.3.0 and, from 0.4.0, applies
an update's changed socket unit: at once while the service is stopped,
else with the next boot.

## License

The service is GPL-3.0-or-later ([LICENSE](LICENSE)). What apps compile in
or copy, `include/`, `client/`, `kit/`, `js/cluster_render_client.js` and
[PROTOCOL.md](PROTOCOL.md), is MPL-2.0
([LICENSES/MPL-2.0.txt](LICENSES/MPL-2.0.txt)), so apps under any license
can use them; they reach the service over a socket. `abi/include/ramses19/`
declares the API of RAMSES (BMW Car IT, MPL-2.0) and the symbol lists name
the exports of RAMSES and vsomeip, so they are MPL-2.0 too;
`abi/include/ramses27/` is RAMSES 27.0.116's own headers, unchanged, with
their notices (BMW Car IT, MPL-2.0);
`dcsm/third_party/vsomeip/` is COVESA's vsomeip, MPL-2.0, with its own
notices. Source, script and build files name their license in an SPDX
header; the rest is GPL-3.0-or-later.
