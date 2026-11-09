# The carplay patch, byte for byte

Four reversible edits to `/usr/bin/carplay` let waze-on-cluster receive the
CarPlay instrument-cluster map, at the size it displays, on a head unit coded
`KI_KOMBI_VARIANT` 7. `carplay_patch.js` applies, verifies and reverts them;
`patchd.js` runs it for the app and the install hooks, `woc-patch.sh` from a
shell.

## Supported head units

Every head-unit variant is supported, HIGH F included: HIGH, the owner's
car, MID, and HIGH F, which a client's car runs (0.8.2 refused HIGH F; it
runs again). The tool refuses only a head unit whose variant it cannot
read, from the bootloader's `hwvariant=` on `/proc/cmdline`, which every
user can read, so every part decides the same way: none or only empty
ones, two that disagree, or a command line it cannot read (`head unit:
unknown variant is not supported (...)`). BMW's runtimevariantprovider
requires `hwvariant=`, so a real unit always has it. `head_unit.js` holds
the rule for the patchers, patchd and boot recovery, cluster-render's
`src/head_unit.cpp` the same rule for the two native services; both run
`cluster-render/test/head_unit_cases.jsonl`.

0.8.2's boot recovery put the patches of every HIGH F car that took it back
to the original, keeping our backups, which looks on disk like a Remove
patch. So the first install on a HIGH F unit applies the patch again, as a
fresh install does on a proven build (pending, through the same checks),
to each of CarPlay and Android Auto whose file is exactly the original its
trusted backup records (a record whose SHA-256 is the backup's and the
file's). A patch boot recovery removed after failed head-unit starts (its
record says so) is the exception: it stays removed until its Patch button
applies it. `/var/opt/waze-on-cluster/highf-repatched` names each of the two
once an install applied it this way or found nothing to put back, or once
its Patch or Remove patch button was used; an install that could not
finish one leaves it for the next. Every later install leaves a removed
patch removed, on HIGH F as elsewhere. The app
shows `Status: Patched after restart`, and the map returns at the next
head-unit start.

On a refused head unit:

- `carplay_patch.js apply` and `aapd_patch.js apply` refuse with
  `error: refusing: head unit: ...`, whatever the flags (`--unproven`
  included); `revert` works.
- patchd reports the conflict `not supported on this car (head unit:
  ...)` for CarPlay and Android Auto, which the app shows as `Status: Not
  supported on this car`; it refuses an apply with the reason. The install
  hook writes nothing and leaves no pending marker; the removal reverts.
- With a backup of ours, the boot steps restore a restorable patch before
  carplay or aapd starts, whatever the crash count ("Boot recovery").
- cluster-render and waze-on-cluster's capture start idle: no Dcsm2,
  vsomeip, RAMSES, AVB join, UDP or FIFO source, VA-API or MSDK. They stay
  up holding only their sockets, answer with not supported, and stop
  cleanly; `Restart=on-failure` does not start them again.

## Builds

`carplay_locate.js` finds the four edit sites in each build by its code and
data, as each edit's "Found by" says, so the patch applies to any build in
which every site is found exactly once and passes its checks. A build where a
site is missing, found more than once or fails a check is not patchable; the
reason names what was not found, and `patchd` reports it as a conflict
(`not patchable: <reason>`). File offsets and virtual addresses are converted
through the ELF program headers, and function bounds come from the
`.eh_frame_hdr` search table; without that table no site but the sink is
found. A site a dynamic relocation covers is refused, and every site in a
build with text relocations or relocations the locator cannot read (REL,
RELR): the loader would write over the edit.

`carplay_patch.js probe [<file>]` (`woc-patch.sh probe`) reads a binary and
prints its size, SHA-256, GNU build-id, the carplay version its debug source
paths name, where each site is, how it reads,
and `patchable: yes` or `no` with every reason; it never writes.
`collect-logs.sh` includes its output.

**Proven builds**, listed by build-id in `carplay_patch.js` (carplay
22.28.3, and 21.27.4, confirmed on a G07 X7 through CarPlay's Patch button,
[#95](https://github.com/robertnisipeanu/idrive-apps/issues/95)): the first edit
needs the whole file to be the shipped one, and any other content reads as
modified by something else. Another build counts as pristine when all four
sites read stock, but its first edit needs `apply --unproven`: its sites pass
every check, yet no car has run it patched, and two edits run at every
CarPlay start (below). `patchd` reports such a build without our backup as
the conflict `untested carplay build`, so an install leaves it as it is, and
CarPlay's Patch button applies it with `--unproven`. Once our backup exists, later applies
and installs patch it like a proven build.

**At CarPlay start**, with or without a phone, `ApplicationPrivate::start()`
builds the CarPlay info: it converts the variant (Edit 1), constructs
`ICSafeAreaProvider` from it, reads the IC_HIGH geometry through its getters
(Edit 4) and hands it to `mCarPlay->SetInitialInfoResponse(…)`, whose result
check throws on an error. A build whose Cinemo rejects the geometry, or whose
IC_HIGH start path needs more than a variant-7 car has, fails there at every
start. Edit 3 runs only on the phone's `onSuggestUi` event, Edit 2 only when
the second stream starts.

The offsets and bytes below are those of carplay 22.28.3, the build the
patch was first proven on:

| | |
|---|---|
| package | `carplay 22.28.3+gitr624+993efc1-r0.14` |
| file | `/usr/bin/carplay`, ELF x86-64 PIE, stripped |
| size | 3 484 912 bytes |
| sha256 | `ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50` |
| build-id | `5de62156575d737391ea42254e778a97dbbb51f8` |

In it, file offset equals virtual address in the ranges used here.

## Edit 1 — variant (ungate the second display)

CarPlay maps the raw coding value `KI_KOMBI_VARIANT` through a converter
jump table at `0x2ba4d4` (12 signed int32 offsets from the table base):

| raw | handler | internal value |
|---|---|---|
| 9 `ic_box_high` | `0x239790` | IC_HIGH (0) |
| 10 `ic_box_mid` | `0x2397e0` | IC_MID (1) |
| 0 | `0x2397d0` | NOT_ACTIVE (3) |
| everything else, incl. 7 `high_4_1` | `0x2397c0` | OTHER (2) |

OTHER fails the accessory-feature gate and the `ICSafeAreaProvider`
resolution builder, so the alternate display is never advertised. The patch
copies the IC_HIGH (or IC_MID) slot value into slot 7; only variant 7 is
affected.

| | offset | bytes |
|---|---|---|
| slot 7 original | `0x2ba4f0` | `ec f2 f7 ff` → OTHER |
| → IC_HIGH | `0x2ba4f0` | `bc f2 f7 ff` (slot 9's value) |
| → IC_MID | `0x2ba4f0` | `0c f3 f7 ff` (slot 10's value) |

**Found by** the converter's name and code. The converter is the one function
loading its name string, `bool …codingToInternal(const
…eKI_KOMBI_VARIANT&, …KombiVariant::Values&)`; its switch is the jump
`cmp byte [rdi],n ; ja ; movzx eax,byte [rdi] ; lea rdx,[rip+table] ; movsxd
rax,[rdx+rax*4] ; add rax,rdx ; jmp rax` inside it. A switch of that shape
elsewhere that decodes to the same mapping is another copy of the converter,
which the edit would leave unpatched, and refuses the build. Other switches
share the shape, so each table of at least 11 slots is decoded through its
handlers (`mov dword [rsi],value`); the converter is the
only switch mapping 9 → IC_HIGH, 10 → IC_MID, 0 → NOT_ACTIVE and every other
value to one shared OTHER table value, with slot 7 holding that OTHER value
(stock) or slot 9's or 10's (patched). The stock and patched slot values are
read from the table itself.

The app applies IC_HIGH; IC_MID (`--panel mid`) keeps its stock 1920x720
geometry.

## Edit 2 — sink (where the stream goes)

The second-stream Cinemo sink URL at `0x2a20b0` is, as shipped:

```
avb://media_transport.avb_streaming.15?mime_type=video/H264    (59 bytes + NUL)
```

`mime_type=video/H264` keeps the H.264 undecoded; the stream is RTP (payload
type 96, single NAL units and FU-A; VERIFIED on a car capture). The modes:

- **avb** (what the app applies; an apply without `--sink`): the stock sink,
  which the render joins as an extra receiver.
- **udp**: `--sink udp://127.0.0.1:20557?mime_type=video/H264`. `udp://` and
  `tcp://` map to Cinemo's `NmeVfsSocket`, a stream sink. That the bridge
  accepts it and sends the same RTP is INFERRED, not verified on a car.
- **fifo**: `--capture <fifo>` writes `file://<fifo>?mime_type=video/H264`.
  The projection bridge rejects a `file://` sink on the car
  (`configureProjectionBridge` fails).

A replacement is NUL-padded to 59 bytes; a longer URL is refused. The NUL
run after the slot and the next string are never written. The render listens
on all three sinks.

**Found by** the string after it, `mCarPlay->GetDisplaySource( …`: the slot
is its own NUL-terminated string ending 1 to 16 NULs before that string, and
holds either the stock URL above or a `udp://`, `tcp://` or `file://` URL no
longer than it. Exactly one such slot must exist, loaded by exactly one
rip-relative `lea`, with no `lea` loading a string inside it (a merged string
that a redirect would change). A build whose stock URL
differs is not patchable: the render joins stream 15.

## Edit 3 — self-start

With Edits 1 and 2 CarPlay advertises the display, but the stream never
starts: `AlternateDisplays` waits for NavController to forward a
`requestStream` command (`ProjectedModeGuiding.streamCommandChanged`), and on
this build NavController's `Guidance.requestStream` is an unimplemented stub.
The fix makes CarPlay start the stream itself.

`requestStream()` (`0x181810`) needs no stored config: it calls `ShowUI` and
`ForceKeyframe` through `mCinemoCarPlay` (`[this+0x28]`) with the URL and UUID
from `CarPlayConstantsRepository`, skips when `mStreamStatus` (`[this+0x48]`)
is already 2, and ends with `setStreamStatus(STREAMING)`.

`AlternateDisplays::onSuggestUi()` (`0x1816f0`), once the display is
available and not streaming, calls `setStreamStatus(this, AVAILABLE)` at
`0x18176c` with `this` in `rdi`. The patch retargets that call:

| | offset | bytes |
|---|---|---|
| original | `0x18176c` | `e8 ef fd ff ff` → `call setStreamStatus` (`0x181560`) |
| self-start | `0x18176c` | `e8 9f 00 00 00` → `call requestStream` (`0x181810`) |

Only the rel32 changes. NavController then receives STREAMING where stock
sends AVAILABLE; it is inert either way, and these messages never reach the
phone. Both guards make it one-shot per session. Every mode applies it.

The stream thus starts with the session, before any navigation app draws on
the cluster, and the phone sends a blank picture until one does (on the car
about 100 blank frames in the first 2 s, then a pause). The render keeps the
stock map up until the first non-blank picture (README).

**Found by** name. setStreamStatus, requestStream and onSuggestUi are the
functions that load their own function-name strings (`void
…AlternateDisplays::setStreamStatus(…`, `…::requestStream()`,
`…::onSuggestUi()`) with a rip-relative `lea`, each string from one function
only and within its first 0x60 bytes (the logging prologue; an inlined copy
or a split-off part loads it deeper), the three functions within 64 KiB of
each other. requestStream must also load the `mCinemoCarPlay->ShowUI(` and
`mCinemoCarPlay->ForceKeyframe(` checks, call setStreamStatus, and be called
directly from outside onSuggestUi: a callback that inlined it has another
argument contract. The site is
onSuggestUi's one call to either function, with AVAILABLE stored as its
argument (`mov dword [rsp+…],1`) in the 24 bytes before it. Its stock and
self-start rel32 are computed from the functions' virtual addresses.

**Exception risk.** `ShowUI` and `ForceKeyframe` are each followed by a
result check (`0x18189b`, `0x1818da`) that calls the throw helper `0x250980`
on error, and the call site has only a cleanup handler. An uncaught throw
would kill CarPlay, which reboots the head unit ("Boot recovery"), and could
repeat at every phone connection. CarPlay's Remove patch button, or boot
recovery (below), restores the stock call path, which has no throw.

### Keyframe cadence

CarPlay forces exactly one keyframe, inside `requestStream` (`0x1818ca`, the
binary's only `ForceKeyframe` reference). On a 90 s car capture the phone
sent SPS, PPS and an IDR only at stream start, twice within the first second,
then non-IDR slices only. A decoder that misses the start never syncs. So
the render runs from boot with its sources listening, starts the decoder at
the stream's first SPS, and decodes whether or not the map is shown. A lost
RTP packet costs only the NAL unit it carried: the decoder decodes through it
and the map shows smudges until the phone codes those areas again (README
"Losses"). `probe.txt` counts `seq_gaps` and its `late-join:` line reads
`RISK` on this stream.

A render that missed the start (it restarted), or whose decoder gave up its
sequence after decode errors in a row, decodes nothing until the next
stream, and nothing on the head unit asks the phone for a keyframe. Its
status says `Stream lost`; the map returns with the next
CarPlay session, when the phone reconnects.

## Edit 4 — resolution

Stock IC_HIGH advertises a 1920x900 display (234x110 mm), so the phone renders
at that size and the map would be scaled twice. Edit 4 advertises the size
the map is shown at: the cluster's central map area, 1088x480 px at
138x61 mm, the view area the full frame. The decoder then passes the picture
through unscaled.

The IC_HIGH builder in `ICSafeAreaProvider` (`0x20ca30`) fills its object from
four `movabs` immediates; only the 8 immediate bytes after each 2-byte opcode
change:

| field | immediate at | stock | patched (default) |
|---|---|---|---|
| mm (→ `+0x10`) | `0x20ca32` | `ea000000 6e000000` (234x110) | `8a000000 3d000000` (138x61) |
| px (→ `+0x08`, `+0x18`) | `0x20ca54` | `80070000 84030000` (1920x900) | `40040000 e0010000` (1088x480) |
| view x,y (u16) + safe width | `0x20ca66` | `00000000 64020000` | `00000000 f8010000` |
| safe height + safe x,y (u16) | `0x20ca74` | `0e020000 0202c000` | `70010000 1e014800` |

The getters at `0x20ce00`–`0x20cf40` read these fields; the config built from
them (`0x2078f1`) is what `SetVideoParams` and the view-area report carry.
The IC_MID builder (`0x20caa0`) is never written: `--panel mid` advertises
stock 1920x720, and switching high → mid restores the IC_HIGH immediates, so
the binary carries one panel's geometry at a time. `--res WxH` (even, up to
1920x1080) picks another pixel size at the same physical size; `--res stock`
keeps stock. The size is negotiated at phone connection, so it applies from
the next CarPlay session after CarPlay restarts.

**Found by** the `ICSafeAreaProvider(const …KombiVariant::Values&)`
constructor, the function that loads its name string. It calls exactly two
geometry builders, within 64 KiB of it: functions of at most 0x80 bytes
opening with `movabs rdx` (the mm pair) and holding `mov [rdi+0x10],rdx ;
movabs rax ; mov [rdi+8],rax ; mov [rdi+0x18],rax ; movabs rax ; mov
[rdi+0x20],rax ; movabs rax ; mov [rdi+0x28],rax` once. The IC_HIGH builder
is the one called on the true branch of the constructor's one call to its
IC_HIGH check, a small function doing `mov eax,[rdi] ; test eax,eax ; sete
al` (or `cmp dword [rdi],0 ; sete al`); the IC_MID builder, the other one, on
the true branch of its one call to the IC_MID check (`cmp dword [rdi],1 ;
sete al`). The `test al,al` and `je` or `jne` after each call give the
branch. A caller of the constructor then reads the fields the edit writes
through one-field getters: `+0x18` (pixel size), `+0x20`, `+0x22` (view x,
y), `+0x24` (safe width and height), `+0x2c`, `+0x2e` (safe x, y).

This tool's geometry has its 138x61 mm physical size, the view area at (0,0)
and a safe area inside the frame. A stock geometry has another physical size
and a safe area inside the frame; any other geometry is not recognised.
`--res stock` and `--panel mid` write the backup's stock immediates back.

### Safe area

The phone draws the map across the whole view area (Cinemo's `viewAreas`)
and keeps its UI, such as the manoeuvre banner, inside the safe area; a car
capture of the stock geometry shows exactly that. With a full-frame safe area
the banner lands in the top-left corner, under the cluster's gauge mask.

The default safe area is **504x368 at (286, 72)** (x 286–790, y 72–440):

- x: BMW's own IC_MID safe area (504x456 at (708, 153) on the panel) lies
  inside the map area at (422, 134); in map-area coordinates it spans
  x 286–790, between the native speed readout (to x ≈ 170) and consumption
  readout (from x ≈ 800).
- y: the native model badge covers the top of the area to y ≈ 64; at y 72 the
  gauge mask's top-left corner is already lit; 440 is where the mask's lower
  falloff starts.

`--safe WxH+X+Y` writes another (at least 16x16, inside the frame), `--safe
full` the whole frame; `--res` scales the default. `patchd` reports `res` and
`safe`, and the app offers CarPlay's Patch button when either differs from the default.
A high-panel binary with stock geometry reads as partially patched.

## Activation

The running CarPlay keeps its old binary until it next starts. A `systemctl
stop` or `restart` of `carplay.service` power-cycles the head unit: CarPlay
dumps core on SIGTERM. Two more things restart the head unit when CarPlay
ends:

- recmgr, when a service its restart list (read only at boot) does not name
  fails or exits;
- the firmware's `carplay.service` has `OnFailure=mediaddpserver_kill.service`,
  which runs `kill -s SIGQUIT $(pidof mediaddpserver)`. `mediaddpserver`
  dumps core on it, and recmgr and crashhandler, on neither of whose
  `[Whitelist]` it is, request a head-unit restart. Any failed end of a
  CarPlay run does this, a SIGKILL included, whatever recmgr's list says.

So `patchd` restarts CarPlay itself only when both are in effect:

- `carplay` is in both `[Whitelist]` and `[WhitelistExitSuccess]` of
  `/etc/recmgr-restartlist.conf` as recmgr loaded it this boot (the file
  while it is older than the boot, else the deploy tool's copy taken at
  boot; `recmgr_list.js`);
- systemd loaded the tool's drop-in
  `/etc/systemd/system/carplay.service.d/50-waze-on-cluster.conf`
  (`systemctl show carplay.service -p DropInPaths` names it), whose
  `SuccessExitStatus=SIGKILL` makes the kill a successful exit: the unit
  does not fail and `OnFailure=` does not run. The deploy tool writes it at
  install and removes it at removal, each with a `daemon-reload`.

`carplay.service` has no `Restart=`, so `patchd` starts it itself: it
records the running CarPlay's `InvocationID` in
`/dev/shm/waze-on-cluster/carplay-killed` (boot recovery does not count the
end of that run as a crash), SIGKILLs its main PID, waits for systemd to
reap it, runs `systemctl start carplay.service` and waits for `is-active`. No main PID, a
failed start or a unit that stays inactive is an error, never a fallback to
stop or restart.

Otherwise a change is deferred:

- the binary is written (backup, verify, atomic rename) with `/` read-write
  only for the write, and the running binary is kept hard-linked as
  `<target>.woc-running`, so no replaced file stays open and `/` can return
  to read-only;
- `/dev/shm/waze-on-cluster/activation-pending`
  (`{"action":"apply"|"revert","reason":…,"boot":…}`, 0644, readable by the
  app while `patchd` is not running) marks it until the next start. A marker
  whose `boot` is not this boot's id (`/proc/sys/kernel/random/boot_id`) is
  from an earlier boot whose start activated the change: `patchd` removes it
  and reads the files' state. One without `boot` (written by an older
  `patchd`) counts: `/dev/shm` starts empty at every boot. Android Auto's
  marker, `aa-pending`, also holds `run`, the density and turn card aapd runs
  until then (`null` without a patch), read before the change writes
  anything and kept by later changes before the start;
- `patchd` reports `carplay: deferred` and `pending: apply|revert`; the app
  shows `Status: Patched after restart` or `Status: Removed after restart` and a **Restart head unit**
  button.

A change that puts back the bytes CarPlay is running moves the kept link
back to `<target>`; nothing is pending.

**`reboot`.** `patchd` takes `{"cmd":"reboot"}` only while a change,
CarPlay's or Android Auto's, is pending and no other operation runs, one
request at a time, and at most once every 10 minutes. The time of each
request is written to `/var/opt/waze-on-cluster/reboots.json` before the
command runs, so a restarted `patchd` keeps the limit; a reboot command that
fails takes out only its own time. It answers with `reboot-reply` and runs
`/usr/bin/nsm_control --requestRestart 7 --normal`, the Node State Manager's
restart the HMI's own restart sends, never a CarPlay or aapd stop or
restart. A command that exits non-zero, runs longer than 30 s or prints
nsm_control's `[Error. <why>]` (it exits 0 then too), or a unit still up
3 minutes after the request, ends the restart with a `reboot-reply`
`ok: false` and its reason. A failed command takes out the request's
time; a unit that stayed up keeps it. Every status carries `restart`:
`{state: idle|restarting|failed, code: refused|stayed|error, reason,
possibleAt}`, `possibleAt` while the limit holds; a failed restart changes
no patch state.

**Busy and refusals.** Every status carries `busy` (`carplay`, `aa`,
`restart` or null) while an operation runs. A command `patchd` does not
take (another operation runs, the 10 s spacing between density and card
changes, a value off the steps) gets `{"ev":"refused","cmd","reason"}`, to
its asker only, and changes no state; while the spacing holds, `aa.changeAt`
says when a change is taken again. `aa.error` holds why the last Android
Auto change failed, until one succeeds.

### The `.woc-running` link

It exists only while a process runs the binary it links and that binary is
not `<target>`'s own file. A process runs a file when its `/proc/<pid>/exe`
is that file or it maps the file (a binary translator, Rosetta in the
emulator, runs it so, its `exe` the translator):

| When | What happens to it |
|---|---|
| a change is written while CarPlay runs `<target>` | created; kept while CarPlay runs it |
| a change puts CarPlay's running bytes back | moved back to `<target>` |
| `patchd` restarts CarPlay | removed |
| `patchd` starts, or any later write window | removed once nothing runs it |
| the tool is removed | `deactivate` removes one that holds nothing; a held one goes to the deploy tool (`"removeWhenUnused"`), which removes it once nothing runs it, at the latest at a later start |

It never outlives its process by more than one head-unit start.

For the length of a change, `<target>.woc-running.1` and `.2` link the file
the change replaces and, while `patchd` puts the running file back, the
file the change wrote. A crash restart in that time may start the service
from either one. After the change each that a process runs is kept,
renamed to `<target>.woc-running` when that name is free, and every other
goes, so no file a process runs is left without a name and `/` goes
read-only again. They are removed and handed over like the link above.
With no name free for the file a change wrote, the running file is not
put back: the written one stays at `<target>` and the change is handled
as any other. A patcher that fails after writing a file leaves the change
waiting for the start (the pending marker) while the service runs another
file than the one on disk, or read aapd.conf before it changed; the
service is not restarted for it.

**Remove patch while a patch waits.** `patchd` reports `removable` (the
revert would change a file now) for CarPlay and Android Auto, and the app
offers Remove patch from it, also while a patch waits for the head-unit
start. A patch waiting over the original has not run: its revert writes
the running bytes back, so the link is moved back to `<target>`, the
marker goes, nothing waits, and the Status reads `Not patched` at once.
Over a running patch the revert writes other bytes than run, so the
removal waits for the start (`Removed after restart`). When the service
started from the replaced file while the revert ran (a crash restart),
that file stays linked and `patchd` restarts the service where it may
restart it; otherwise the removal waits for the start. When it started
from the file the revert wrote, before `patchd` put the running file back,
that file stays linked: CarPlay runs the reverted bytes and nothing waits;
aapd may have read aapd.conf before the revert reached it, so Android
Auto's removal waits for the start.

### Install-time activation

The deploy tool (`mgu-deploy` 0.7.0 and later) runs `patchd.js activate` as
root after each install and update, and `patchd.js deactivate` before a
removal deletes anything. Both write as above under the shared remount lock
and **never stop, kill or restart CarPlay**, even where recmgr would allow
it. The hook's last stdout line, `{"status":…,"reboot":"patch written; takes
effect at the next head-unit start"}`, makes the deploy tool mark a restart
pending, which the Store shows.

| On disk | `activate` | `deactivate` |
|---|---|---|
| the pristine proven build, no backup of ours | applies CarPlay's Patch button's patch (pending) | nothing |
| a build outside the proven list, no backup of ours | nothing: a conflict (`untested carplay build`, `untested aapd build`) | nothing |
| a patch of ours, any configuration | brings it to CarPlay's Patch button's (pending), or nothing if it is already | reverts our edits (pending), drops our backup |
| the original with our backup (CarPlay's Remove patch button was used) | nothing: the map stays off; the first install on HIGH F applies it again (pending; "Supported head units"), except where boot recovery removed the patch after failed starts | drops our backup |
| a change already pending | nothing (still reports the restart); after a removal's revert, applies again (a build outside the proven list stays reverted: a conflict) | a pending apply is undone (nothing pending); a pending revert becomes the removal's; our backup goes |
| a patch of ours plus another tool's edit elsewhere | updates ours, keeps theirs | reverts ours, keeps theirs, drops our backup |
| a build with an edit site not found or not recognised | nothing: a conflict | with our verified backup of the same build differing from it (`restorable`), writes the backup whole (pending) and drops it; else nothing |
| a proven build with one of our edits and no backup of ours to trust | nothing: a conflict | rebuilds it from its shipped bytes (pending); carplay 21.27.4, whose shipped bytes are not all listed: the removal is refused (exit 1) |
| another build with one of our edits and no backup of ours to trust | nothing: a conflict | nothing: the removal is refused (exit 1) |
| a proven build modified by something else, none of our edits in it | nothing: a conflict | nothing |
| `carplay.service` starting another program (an `ExecStart` in a unit or drop-in under `/etc` or `/run/systemd/system`) | nothing: a conflict | reverts our patch |

A conflict is not a failure: the hook exits 0 with `conflict` in its result,
`patchd` reports it, and the app shows it with CarPlay's Patch button, which applies only
on request (the patcher still refuses a binary it cannot patch). A hook exits
non-zero only when it changed nothing and could not do its job (`/` would not
remount, the patcher failed), so the deploy tool rolls the install back or
keeps the tool installed. Once the binary is written the hook exits 0, even
if `/` stayed writable (the next boot mounts it read-only).

Removal drops the backup (`carplay_patch.js revert --drop-backup`), so a
later install patches like a fresh one. A removal passes only when, after
its step, no edit of ours reads in carplay and its state could be read;
otherwise `deactivate` exits non-zero and the tool stays with its boot
recovery.

**Removal.** The Store removes the app, then the tool. After `deactivate`
the deploy tool stops and removes the tool's units, recmgr entries and
`/var/opt/waze-on-cluster`; the renderer releases the map's slot, and the
capture restores the DLT tracing mode and default log level as it stops.

## Reversibility

Before the first edit the patcher writes the backup under a temporary name and
checks it, writes the record of the original (SHA-256, build-id and version)
to `<target>.woc-orig.json` in full by rename, and then renames the backup to
`<target>.woc-orig`: a backup never exists without its record. The
temporary files of an apply cut short go at the next first edit and at a
removal (`revert --drop-backup`). `apply`
re-checks the backup: its recorded SHA-256, a pristine build (above), and the
same build as the target (size, build-id and every site). The stock geometry
`apply` writes back (`--res stock`, `--panel mid`) is the backup's.

`revert` needs no locator to trust the backup: its SHA-256 and GNU build-id
must be the recorded ones (each when recorded). The target is the backup's
build when both have the same build-id and size, or, when either has no
build-id, the same size and at most 200 bytes apart (twice the edits'
bytes). When the locator recognises the target and the backup as the same
build, `revert` restores only the edits' regions, so another tool's change
elsewhere stays, and confirms all four edits read original. Otherwise, over
the backup's build, it writes the whole backup: that undoes a patch any
version of this tool wrote, also one whose sites the locator now refuses.
Over another build (a firmware update) it refuses; `revert --force` writes
the whole backup there too. A backup without a record (an older release's)
is written whole only when the locator reads it as the pristine build or it
has the target's size, `--force` or not.

**Without a backup to trust** (none, or one whose record does not vouch for
it), a proven build in which an edit of ours reads is rebuilt from its own
bytes: the shipped bytes of every offset any version of this tool writes in
it (`PROVEN[...].stock`) are written over the target, and the result is
kept only when its SHA-256 is the shipped file's. A proven build with none
of our edits in it is left as it is, and reads `restorable: no`. Another
build still needs a verified backup, and so does carplay 21.27.4: its list
lacks the variant slot and three of the geometry immediates.

`status` prints `restorable: yes` when the revert would change the target,
else `no` with why. Every write is an atomic rename, with the original's
mode and owner (file_mode.js); a pristine original that carplay.service's
user cannot run gets only those from `revert`, or from an `apply` that
changes nothing. After each write to the target the patcher records it in
`<target>.woc-patch.json` for boot recovery ("Boot recovery"), and a
removal drops that record with the backup.

## Android Auto

The same pipeline shows Android Auto's cluster view: Google Maps renders it
for the display aapd declares to the phone as Android Auto's cluster display,
and aapd sends it to the same AVB stream 15. `aapd_patch.js` makes four edits
to `/usr/bin/aapd` and sets the geometry in `/etc/aapd.conf`
(`aapd_conf.js`); `patchd` runs it for the app and the install hooks.

### Builds and the coding

`aapd_locate.js` finds the four edit sites in each build by its code, as
each edit's "Found by" below says, in their stock or patched form, with
`carplay_locate.js`'s ELF reading: function bounds from the `.eh_frame_hdr`
search table, functions by the name strings their logging prologues load
(each string once, loaded by one function only, within 0x80 bytes of its
start), a site a dynamic relocation covers refused. A build where a site is
not found, is found more than once or fails a check is refused with the
reason; `patchd` reports it as `not patchable: <reason>` (`Status: Not
supported on this car` in the app's Android Auto section). Each check has a
name in `aapd_locate.js` (`CHECKS`), and the mutation test shows each one alone refusing a mutation
the locator would otherwise patch.

**Proven builds**, listed by build-id in `aapd_patch.js`: the first edit
needs the whole file to be the shipped one. Another build counts as
pristine when all four sites read stock, but its first edit needs `apply
--unproven`: its sites pass every check, yet no car has run it patched.
`patchd` reports such a build without our backup as the conflict
`untested aapd build`, so an install leaves it as it is, and Android
Auto's Patch button applies it with `--unproven`. Once our backup exists, later
applies and installs patch it like a proven build. `status` prints the
build-id, the version its debug source paths name, and `proven: yes|no`.

| | |
|---|---|
| package | `aapd 22.39.3+gitrAUTOINC+d5a1db18c4-r0` (MGU P18_23w44.2-1-3) |
| file | `/usr/bin/aapd`, 7 367 952 bytes |
| sha256 | `6880f54c3abba40c1368f2bf254c77bd4a169323c0d4cab8f9662a7c912dcf89` |
| build-id | `2dfb9881706bff435b05f621486e8eb8b6c55984` |
| aapd.conf sha256 | `9584c540f287bb79744e79cf973795208719ca798f13d881c4dac582656b0f16` |

The other build known, aapd 23.16.5 of ID8 (build-id
`ef291b7185f210a24d8e68855d94d44e9f32cb62`), is refused: its
`isSecondVideoEnabled()` also weighs a `SECOND_VIDEO_SCREEN_MODE` setting
and keeps the variant check in another register, and its
`AlternateDisplays` has no `onNewNavigationStatus()`. Only the screen-size
helper is found there.

aapd maps the raw `KI_KOMBI_VARIANT` through
`codingToInternal(const CodingKOMBI_HUD::eKI_KOMBI_VARIANT&, KombiVariant&)`
(`0x4f8840`): 9 → 0, 10 → 1, 1–8 and 11 → 2, 0 → 3. The gate edit admits
class 2 whole, so `apply` refuses unless the raw value is 7, the coding the
patch was proven on. It reads it as aapd and carplay do: `CodingKOMBI_HUD`
is `CodingBase(0x75a, 150)` of the coding image (`0x4f9e5b`; carplay
`0x23ad70`) and the variant `getBits(0, 0x7e, 1)` of it (`0x4f9c88`), so
byte `0x2c + 0x75a` of `/dev/shm/rawcodingdata` (4144 bytes, header flags
at `0x28` reading `0x09`, valid coding), bits 1–6. A coding that cannot be
read refuses too. `patchd` reports another value as `not supported on this
car (KI_KOMBI_VARIANT n)`.

### The edits

In the proven build:

| Edit | Offset | Stock | Patched |
|---|---|---|---|
| gate | `0x998bd` | `83bb1c020000 01` (`cmp dword [rbx+0x21c],1`) | `…02` |
| selfstart call | `0x1d43eb` | `e800f8ffff` (`call setNaviStreamStatus`) | `e840fbffff` (`call requestStream`) |
| selfstart guard | `0x1d3f5d` | `0f848d000000` (`je 0x1d3ff0`) | `e98e00000090` (`jmp 0x1d3ff0 ; nop`) |
| screen | `0x15b440` | `660fefc0 ebe1 662e0f1f840000000000` | `83f802 740b 660fefc0 ebdc 0f1f440000` |

- **gate.** `Application::secondVideoEnabled()` returns `featureFlag &&
  KombiVariant <= 1` (`cmp … ; setbe al`); a variant-7 car is class 2, so
  the cluster screen is off. `<= 2` admits it; class 3 (no cluster) stays
  off.
- **selfstart.** With the gate alone aapd sets up the cluster display and
  then waits for a `ProjectedModeRequestStreamCommand` the car never sends,
  as CarPlay waits for NavController. `onNewNavigationStatus()`'s shared
  tail calls `requestStream(this)` instead of `setNaviStreamStatus(this,
  status)` (the extra argument is unused), and `requestStream()` no longer
  waits on the `[rbx+0x44] == 2` a `StreamCommandChanged` event would set;
  its own check that it is not streaming already (`[rbx+0x38] != 2`) stays,
  and it still calls `setNaviStreamStatus(STREAMING)`. The two go together.
- **screen.** `loadKombiDisplayDefinition()` turns the class into the screen
  size it looks up in aapd.conf (`0x15b400`: 0 → 12.3, 1 → 10.25, else
  0.0); with 0.0 it skips aapd.conf and sends an 800x480 stream with the UI
  in its top 800x300. The fall-through's 16 bytes (10 of them padding)
  become `cmp eax,2 ; je <the 12.3 load> ; pxor xmm0,xmm0 ; jmp <return>`:
  class 2 reads 12.3, every other class as before.

**Found by.**

- *the variant mapping* (gate and screen): `codingToInternal(const
  CodingKOMBI_HUD::eKI_KOMBI_VARIANT&, KombiVariant::Values&)`, by its own
  name string, holds one jump-table switch whose handlers store the class:
  raw 9 must store 0, 10 store 1 and 7 store 2. A switch elsewhere decoding
  to that mapping (another copy of the converter) refuses the build.
- *screen*: `loadKombiDisplayDefinition()` reads the variant through the
  coding getter and, right after, calls the one helper of this shape,
  handing it in the same preserved register the address the getter wrote:
  `mov eax,[rdi] ; test eax,eax ; je L0 ; movsd xmm0,[rip+C1] ; cmp eax,1 ;
  jne Lf`, with `C1` the double 10.25, `L0` `movsd xmm0,[rip+C0] ; jmp Lret`
  with `C0` 12.3, and `Lf` the stock fall-through (`pxor ; jmp Lret`, NOP
  padding to `L0`) or the patched one, 11 to 16 bytes. The patched bytes
  are built from the located `L0` and `Lret`, padded with GNU NOPs.
- *gate*: `initCodingProvider()` calls that same coding getter once,
  with the address of one field of its object (`lea rsi,[reg+disp] ;
  call`): the variant. `prepareCinemoConfiguration()` calls one short function holding
  `test al,al ; je +10 ; cmp dword [reg+field],1|2 ; setbe al` and keeps
  its result as a byte of its object. No other code compares the field with
  1 or 2.
- *selfstart*: `requestStream()`, which `onStreamCommandChanged()` calls,
  opens with `cmp [this+A],2 ; je +10 ; cmp [this+B],2 ; je Ldo` (or the
  patched `jmp Ldo ; nop`); before `Ldo` it logs `Ignoring event. Already
  streaming...`, after it `Request Stream Command event received`, calls
  `setNaviStreamStatus(STREAMING)` (2) and sets `B` to 1.
  `setNaviStreamStatus` copies its argument's value (`[rsi+X]`) into `A`;
  where the build has its name string, it is the function loading it.
  `onNewNavigationStatus()` calls `setNaviStreamStatus` (stock) or
  `requestStream` (patched) exactly once: that call. On every path to it,
  `rsi` holds `rsp+B`, its value followed through the function by
  `x86_flow.js`: set from `rsp` (`lea R,[rsp+d]`, `mov R,rsp`), copied
  between registers (`mov R2,R1`, `lea R2,[R1+d]`), moved by `add` or `sub
  R,imm`, and kept across pushes, pops and `add`/`sub rsp,imm` as `rsp`
  moves; any other write to a register, or a call for the ones a call may
  change, makes its value unknown, and paths that disagree make it unknown.
  The argument's stores and the call see the same `rsp` (the same depth
  below the function's entry) on every path. The pass follows paths into
  code outside the function a branch or a fall-through reaches (up to 2000
  instructions); a function, or code reached from it, it cannot decode, a
  branch into the middle of one of the function's instructions or out of
  the executable code, a reachable indirect jump, or a `ret` with `rsp`
  known and away from the entry depth (a push and ret) refuses the build.
  Its argument's vtable is stored at `[rsp+B]` in the 24 bytes before it,
  the UNAVAILABLE (0) status at `[rsp+B+X]` right before that tail,
  falling into it, and the
  AVAILABLE (1) status in the same slot by the one store that jumps to the
  tail; STREAMING in `requestStream()` is stored the same way. The three
  functions lie within 64 KiB of each other. 22.39.3 has no
  `setNaviStreamStatus` name string (23.16.5 has); there the STREAMING,
  argument and field checks identify it.

**The named checks** (`CHECKS`; each one alone refuses a mutation in
`aapd_locate_test.js`):

- `name-once`: each function's name string is in the build once.
- `name-one-function`: only one function loads it.
- `name-at-entry`: it loads it within 0x80 bytes of its start.
- `conv-copy`: no switch outside the converter decodes to its mapping.
- `conv-switch-once`: the converter holds one jump-table switch.
- `conv-mapping`: its handlers map raw 9 → 0, 10 → 1, 7 → 2.
- `getter-shared`: loadKombiDisplayDefinition and initCodingProvider read the variant with the same getter.
- `getter-once`: initCodingProvider calls that getter once.
- `helper-arg`: the helper is handed, in the same preserved register, the address the getter just wrote.
- `helper-once`: loadKombiDisplayDefinition calls one screen-size helper.
- `size-1`: class 1 reads 10.25.
- `size-0`: class 0 reads 12.3.
- `class0-shape`: class 0 loads its size and returns through the shared exit.
- `room`: the fall-through is the 11 to 16 bytes the edit fills.
- `fall-through`: the fall-through is the stock or the patched code, padded with NOPs.
- `gate-field`: the gate compares the field initCodingProvider fills.
- `gate-flag`: the comparison is skipped when the feature flag is off.
- `gate-once`: prepareCinemoConfiguration calls one gate.
- `gate-kept`: it keeps the gate's result.
- `gate-unique`: no other code compares the field with 1 or 2.
- `near`: requestStream, onNewNavigationStatus and onStreamCommandChanged lie within 64 KiB.
- `rs-called`: onStreamCommandChanged calls requestStream.
- `guard-once`: requestStream opens with one guard.
- `guard-form`: its second branch is `je` (stock) or `jmp ; nop` (patched).
- `already-log`: the streaming-already log is on the guard's other path.
- `request-log`: the request log follows the guard's target.
- `request-log-once`: requestStream loads it there once.
- `ss-call-once`: after it, requestStream calls one function beside it (setNaviStreamStatus).
- `ss-field`: setNaviStreamStatus copies its argument's value into the field the guard checks.
- `ss-field-once`: it does so once.
- `streaming`: requestStream builds a STREAMING argument for it.
- `mark`: requestStream then marks the request field.
- `ss-name`: where a string names setNaviStreamStatus, the function loading it is the one called.
- `onnav-once`: onNewNavigationStatus calls setNaviStreamStatus or requestStream once.
- `arg-pointer`: on every path to each call, rsi holds the address of the argument it builds.
- `arg-frame`: the argument's stores and the call see the same rsp on every path.
- `status-unavailable`: UNAVAILABLE is stored in that argument right before the call's tail.
- `status-available`: AVAILABLE is stored there by the one store that jumps to the tail.
- `reloc`: no dynamic relocation covers an edit.

On the car (BMW X5 G05, P18_23w44, KombiVariant class 2) the edits ran with
no aapd restart and the phone sent its cluster view.

**The coding gate stays at 7.** The gate edit admits KombiVariant class 2
whole (raw 1–8 and 11), and the screen edit gives class 2 the 12.3"
geometry this tool's 1088x480 map slot is cut from; the slot, the safe area
and the crop were measured on a variant-7 cluster only. Classes 0 and 1
(raw 9, 10) already have aapd's own cluster display, which the self-start
and geometry edits would change. So `apply` refuses every raw value but 7,
on every build.

### The geometry

aapd builds the cluster display from aapd.conf's `KOMBI_DISPLAY`: the
`DIMENSIONS` entry for the screen size (`12.3`) and the codec definition
`CODEC_SUPPORTED` names for it (`HIGH_1080P`). The stream follows
`CODEC_HEIGHT` (720 → 1280x720); the margins are `OFFSET_LEFT/TOP_TARGET_VIEWPORT`
and `HORIZONTAL/VERTICAL_MARGIN_SIZE` less them; the content insets are the
entry's `SA_*PIXELS` rectangle; the density is `REPORTED_SCREEN_DENSITY`.
`aapd_conf.js` replaces those numbers in place, the rest of the file byte
for byte. It works on any aapd.conf that is JSON with no key twice in one
object, names exactly one codec for `12.3` in `CODEC_SUPPORTED`, and holds
each number it sets in that codec definition and the `12.3` `DIMENSIONS`
entry; another is refused with what is missing, and `patchd` reports it as
`aapd.conf not usable: <why>`. Without a backup of ours, a file that
already holds one of our geometries is refused too: its original is
unknown. The edit:

- a 1280x720 stream with the UI in its top 1280x564 (margins 0x156 at
  0,0), the map slot's 2.27:1;
- the safe area 592x432 at (336, 84): the CarPlay safe area of the map
  slot, 504x368 at (286, 72), at the stream's scale, so the turn card stays
  where the gauges leave the slot clear;
- the density chosen (150–400 dpi in steps of 10, 180 by default; kept in
  `/var/opt/waze-on-cluster/aa-density`). A lower one makes the phone take
  the screen for a larger one: more map, smaller text.

The decoder crops the picture to the UI (`src_crop = 1280x720:0,0,1280,564`
in `waze-on-cluster.conf`, which applies to a 1280x720 sequence only; CarPlay's
1088x480 is used whole) and scales it 0.85x into the 1088x480 map area. On
the car (P18_23w44) the phone drew rows 0–561 of the 720, the map filled the
slot unstretched and the turn card sat top-centre inside the safe area. The
`10.25` entry and `MID_1080P` are left stock.

**Turn card hidden** (`/var/opt/waze-on-cluster/aa-card`). This aapd has no
setting for Android Auto's own flag to hide the card, so the UI is the whole
picture with the safe area from its top (592x672 at 336,0), where Maps pins
the card, and the crop takes the rows under it. The card's bottom edge is
about 0.62 x dpi rows down, so the cut is 156 rows up to 200 dpi, then
0.62 x dpi + 10 rounded up to even, the crop narrowed and centred to keep
1088:480 (240 dpi `4,160,1270,560`, 300 `46,196,1188,524`, 400
`116,258,1048,462`). The render reads its crop at start: `patchd` writes this
one to `/var/opt/waze-on-cluster/aa-crop.env`, which `waze-on-cluster.service`
passes as `-s src_crop=…`, and a change of it waits, with aapd.conf, for the
next head-unit start. Showing the card again removes the file. A render
that restarts before that start (an update of the tool, a crash) reads the
new crop early, while the phone still draws the old geometry: the map is
cropped wrongly until the head-unit start.

### Backups and undo

Before the first change of each file the patcher writes its backup under a
temporary name, fsyncs and verifies it, writes the record (the SHA-256) in
full by rename, then renames the backup: `/usr/bin/aapd.woc-orig` and
`/etc/aapd.conf.woc-orig`, each with `.woc-orig.json`. The record also keeps
the original's mode and owner, which every later write of the file gives it
(aapd at least 0755, aapd.conf at least 0644: aapd runs as `cia_13006`). A backup is trusted
only when its SHA-256 is the recorded one; one without a record is not. It
needs 2 x 7.4 MB free on `/` (the backup and the temporary copy) plus 1 MiB,
read with `df`, or it refuses.

`revert` needs no locator. aapd is restored over a file of the backup's
build (the same size and build-id, or at most 68 bytes apart without one):
the edits' bytes only when the locator reads it and the pristine backup as
the same build at the same sites, else the whole backup.
aapd.conf is restored whole over a file that differs from the backup only in
numbers inside `KOMBI_DISPLAY`, whatever geometry wrote them. Each file is
restored on its own: one whose backup is not trusted, or that is another
build (a firmware update; `--force` writes over that), is left as it is.
Without a backup to trust, the proven aapd is rebuilt from its own bytes
(`PROVEN[...].stock`) when an edit of ours reads in it, and the proven
aapd.conf from its own text when its numbers are one of the geometries this
tool writes (a density and turn-card setting of its edit), the shipped value
of every number any version of this tool sets written back as it was
(`aapd_conf.js` `PROVEN`); each is kept only when its SHA-256 is the
shipped file's. A file with nothing of ours in it, such as an aapd.conf
another tool changed, is left as it is and reads nothing to undo. Another
build still needs a verified backup.
`revert` exits non-zero unless each file ends at its original, with no
backup of ours, or another build than its backup; `--drop-backup` drops the
backups of those files only. `status` prints `restorable: yes` when the
revert would change either file, or only its mode, and `undo:` with each
file's state (`original`, `none`, `other`, `restorable` or `untrusted`).
Each file is written with its original's mode and owner (above); a file
that is exactly its original but lacks the access aapd.service's user needs
reads restorable, and its repair counts as nothing to do, so `patchd`
restarts nothing for it.

### Activation

aapd reads both files when it starts. `aapd.service` (`Type=simple`,
`WatchdogSec=30`, `User=cia_13006`) has no `Restart=` and no `OnFailure=`,
and the firmware's recmgr and crash-handler lists do not name it. So its
end matters only to recmgr, which restarts the head unit for a service it
does not list, and, for a core dump, to the crash handler. `patchd` restarts
aapd itself only where recmgr loaded `aapd` in both lists this boot: SIGKILL
of the main PID (no core dump, so no crash handler), the run's
`InvocationID` recorded in `/dev/shm/waze-on-cluster/aapd-killed`, then
`systemctl start aapd.service` and `is-active`. No drop-in is needed: a
SIGKILL fails the unit (`Result=signal`), which runs nothing. Otherwise the
change waits for the next head-unit start, the running aapd kept linked as
`/usr/bin/aapd.woc-running`, `/dev/shm/waze-on-cluster/aa-pending` marking
it. A change to aapd.conf alone waits the same way. The marker records the
SHA-256 of aapd.conf as the running aapd read it (`extra`), while that aapd
still runs (its file kept linked; one started since read the files as they
are), so Remove patch of a waiting patch, which restores both files, is
known to leave aapd as it runs: nothing waits.

aapd.conf has no such link. In the emulator the stock aapd reads it at start
and keeps it closed; this is not checked on the car. If aapd keeps it open
during a session, a density or turn-card change replaces an open file, and
`/` stays writable until the next head-unit start.

The deploy tool adds `aapd` to recmgr's lists and to the crash handler's
(`crashNames`), from the next head-unit start.

### Install and removal

`patchd.js activate` and `deactivate` do CarPlay first, then Android Auto,
with the rules of the table above for each, and never stop or restart
either service. Android Auto's outcome never fails an install: a car with
another coding, an aapd build outside the proven list Android Auto's Patch
button has not patched, one the locator refuses, no aapd or an aapd.conf without
what the geometry sets is reported in the hook's result (`"aa": {"status",
"conflict"}`) and left as it is. When CarPlay's step fails, Android Auto's
does not run.

The deploy tool SIGKILLs a hook after 120 s. A hook ends within 105 s:
every command it runs is cut to what is left, keeping 22 s to put `/` back
read-only, and Android Auto's step starts only with 40 s left. A hook that
cannot take the remount lock within 20 s changes nothing and exits
non-zero.

A removal goes ahead only when Android Auto is undone or known not to be
ours: after its step `deactivate` reads the status again, and each of aapd
and aapd.conf must be at its original, have no backup of ours, or be
another build than our backup (a firmware update; left as it is, like
carplay's), and no edit of ours may read in either; carplay's status
likewise. Either status counts only when read whole: the patcher's last
line, `end: complete`, and exit code 0. Otherwise (a state unreadable or
cut short, no time left for its step, a file still restorable or with a
backup that is not trusted, an edit of ours left) `deactivate` exits
non-zero and the tool stays with its boot recovery and whitelists.

`patchd` holds the remount lock for every write: a user's change waits
15 s for it, then gives up and writes nothing. It is busy from the moment
it accepts a change (Zoom and the turn card included), so no second change
and no head-unit restart starts meanwhile.

## Boot recovery

On a head unit that refuses our parts ("Supported head units"), a patch of
ours (by the patch record below, or a binary of our backup's build the
record does not describe, or an aapd.conf that still differs from its
verified backup) is restored at the boot step before CarPlay or aapd
starts, whatever the counter, with the same verified restore; the log
says `head unit: ...; restoring the original carplay` and the result. Where
nothing is restorable `/` is not remounted. A head unit that runs our parts
is not touched by this.

A patch can keep the head unit from starting: the patched CarPlay crashes,
or it makes another process crash, and the head unit restarts with the
patched binary still on disk, every few seconds, with no HMI to turn the map
off. A CarPlay crash (a core dump, or a watchdog timeout) fails
`carplay.service`, so its `OnFailure=` restarts the head unit
("Activation"), and the core dump goes to crashhandler. Its `[Whitelist]` is
in `/etc/crashhandler-restartlist.conf` (the default of
`Restart/RestartListPath`, which `/etc/crashhandler.ini` does not set), a
list of its own, not recmgr's, and the firmware's has no `carplay`; the
recmgr entry and the drop-in do not prevent it; the drop-in adds SIGKILL to
the exits that count as successful, not a core dump. Another process's
failure restarts the head unit through recmgr. Boot recovery counts the
starts that fail with the patch on disk, whatever crashed, and restores the
original binary before CarPlay starts.

**How the head unit goes down.** recmgr and crashhandler ask the
NodeStateManager for a fast shutdown; it runs `var-opt-ro.service`, which
syncs and remounts `/var/opt` read-only, and the IOC then resets the SoC.
systemd stops no service: there is no SIGTERM, no `ExecStop` and no
`shutdown.target`. A switch-off takes the same path. What tells them apart
is the restart record each of recmgr and crashhandler writes when it asks
for the restart, `/var/opt/public/sin/recmgr/restartInfo.xml` and
`/var/opt/public/sin/crashhandler/restartInfo.xml`, which each deletes when
it starts at the next boot. Restart head unit asks the NodeStateManager for
a normal restart, which runs `var-opt-ro.service` and writes no restart
record; a `systemctl reboot` is an orderly systemd shutdown, which stops
every service and does not run `var-opt-ro.service`.

The emulator, with the firmware's recmgr, crashhandler and NodeStateManager,
showed: recmgr writes its record when a unit fails, before it asks for the
restart; crashhandler writes its record just before its restart request
(after up to 60 s waiting for a crash report there); both are on disk when
`var-opt-ro.service` runs; each deletes its own record some 10 to 20 s
into the next boot, after the boot step has run. What only the car can
show: whether a record reaches the disk when the IOC resets the unit
without `var-opt-ro.service` (crashhandler's fallback when the
NodeStateManager does not take the restart), and the deletion's timing
there against the boot step.

Android Auto has the same recovery, with a counter of its own, and the
sections below apply to aapd as to CarPlay, with `aapd.service`,
`/var/opt/waze-on-cluster/aa-boot-recovery.json`, the shutdown record
`shutdown-aapd` and the boot step `waze-on-cluster-aa-boot.service`
(`recovery.js boot aapd`, `Before=aapd.service`). The restore puts back
aapd and aapd.conf. A SIGKILL of aapd ends it failed with `Result=signal`,
`ExecMainCode=2`, `ExecMainStatus=9`; that is no crash, and the watcher
starts aapd again as after a crash. The shutdown record writes such an end
as `stopped`, so the boot step does not count it either. The watcher
follows aapd only on a unit that has it.

**What is recorded.**

- The patch record, `<target>.woc-patch.json` (`patch_record.js`), beside
  the binary: `{patched, proven, ino, size, mtime}`, written by the
  patcher after each write to the target (and by an `apply` or `revert`
  with nothing to write when the file has changed since the record was
  written, or there is none), by the restore below with `by: "recovery"`,
  `at` and `fails` after failed starts, or `by: "refused"` and `at` on a
  head unit that refuses our parts, and removed with the backup. It describes the target while its inode, size and
  modification time are the recorded ones. `proven` is whether the
  build is on the patcher's proven list (`PROVEN`, for aapd its one known
  build).
- `/var/opt/waze-on-cluster/boot-recovery.json`, written whole: a
  temporary file, fsync'd, renamed over it, the folder fsync'd. A write
  that fails leaves the previous state as it was. It holds:
  - `fails`: failed patched starts and crashes of the patched CarPlay since
    the last healthy one;
  - `unknown`: patched starts in a row since then that ended with no
    record;
  - `arm`: this start's record, `{boot, at, patch, crashes, ran, confirmed,
    clean}`, written before a patched CarPlay starts; `at` is when (ms
    since the epoch), `patch` is `proven`, `unproven` or `unknown`, `ran`
    that the watcher saw CarPlay running;
  - `prev`: the last start counted;
  - `starts`: the CarPlay starts made this boot;
  - `ended`: the last CarPlay run whose end was handled.
- The shutdown record, `/var/opt/waze-on-cluster/shutdown`, written by
  `shutdown_mark.sh`, run by a drop-in for `var-opt-ro.service`
  (`50-waze-on-cluster.conf`) before `/var/opt` goes read-only: `<boot id>
  running|failed|stopped` from `carplay.service`'s `ActiveState` and
  `Result` (`failed` is any `Result` other than `success`), followed by
  ` crash` when a restart record of recmgr or crashhandler is there and
  was written in this boot (its modification time, by `stat`, not before
  `/proc/stat`'s `btime` and not after now, a second of slack each way),
  ` ran` when systemd started the service in that boot
  (`ExecMainStartTimestampMonotonic` above 0), and ` up <s>`, the boot's
  system uptime. It writes nothing for a service
  `systemctl` does not answer for within 3 s, and always exits 0, so it
  never holds up the remount. `var-opt-ro.service` syncs `/var/opt` next.
- `/var/opt/waze-on-cluster/restart-requested`: the boot `patchd`'s
  `reboot` was asked in, written before the reboot runs.

**The boot step** (`recovery.js boot`, `waze-on-cluster-boot.service`, a
oneshot wanted by `multi-user.target`, `Before=carplay.service`, which
`late.target` starts after `multi-user.target`). It reads the state, the
previous start's shutdown record, `restart-requested`, the other service's
state, whether our backup exists, the patch record, a `stat` of the
binary and, with no shutdown record, a `stat` of the two restart records,
and nothing else; it reads the binary only to restore it.

A start counts as failed only on evidence of a crash. The evidence is the
restart record recmgr or crashhandler writes when it asks for a head-unit
restart (`restartInfo.xml`, above, deleted by each at its next start):
the shutdown mark's ` crash` says it was there at the shutdown, and with no
mark, the record itself stands in when it is still there at the next boot
step and was written in the armed boot: its modification time at or after
`at`, before this boot's `btime` and not after now, each within a second of
slack for the file system's clock. Whatever process
it names counts: a patch can take another one down. Neither crashhandler's
core dump folder (it also keeps dumps of processes on its whitelist, which
restart nothing), NodeStateManager's last shutdown reason (kept inside
NSM, reachable only over SOME/IP) nor a journal (it is volatile) serves.
The previous start, when it was armed, counts in this order:

| How it ended | `fails` | `unknown` |
|---|---|---|
| CarPlay crashed in it (seen by the watcher, or the shutdown record says `failed`) | + its crashes (at least 1) | 0 |
| otherwise, CarPlay ran healthy (60 s without a break) | 0 | 0 |
| CarPlay never ran in it (the shutdown record says `stopped` without ` ran`, the watcher never saw it running) | kept | kept |
| Android Auto, patched in it, crashed in it | kept (counted for Android Auto) | 0 |
| a restart recmgr or crashhandler recorded (` crash`, or its record with no mark) | + 1 | 0 |
| a switch-off or an NSM restart after the healthy window (a shutdown record without ` crash` from a boot whose system uptime, `up`, is 60 s or more), or a restart the user asked for (`restart-requested` names it, or the watcher stopped while `shutdown.target` was queued) | kept | 0 |
| no record: a power cut, a 12 V disconnect, a dealer tool, a restart whose records were lost, or a shutdown record without ` crash` from a boot shorter than the healthy window | kept, or + 1 when it is the second such start in a row | + 1, or 0 when it counted |

A start with no record is unknown, not failed: one never removes a patch.
Two in a row count one failure, so a loop that leaves no record (a reset
before `var-opt-ro.service` runs and before recmgr's record reaches the
disk, or a record the shutdown record missed) is still caught: one start
later than a loop with records for a build outside the proven list, two
starts later for a proven one. The cost is accepted: two short boots in a
row with no record (two quick switch-offs, a dealer tool's or the IOC's
resets) count one failure, which removes an unproven patch, or a proven
one at the second such pair with no healthy boot between.

Then, by the binary about to start:

| On disk | The step |
|---|---|
| no backup of ours, or the patch record says original | clears the counter; arms nothing |
| our patch, `fails` below the threshold | arms this start |
| our patch, `fails` at the threshold | arms this start, restores the original |
| this start's arm cannot be saved | restores the original at once |

The threshold is 1 failed start for a build outside the proven list (`proven:
false`; CarPlay's Patch button patches one with `--unproven`) and 2 for a
proven build or a binary of the backup's build the record does not
describe (`unknown`, an older release's patch). Only a healthy start
clears the counter: a proven patch is restored after two failed starts, or
crashes of the patched service, with no healthy start between them, the
switch-offs, restarts asked for and starts it never ran in between counting
nothing either way. A single crash restart of some BMW process within the
window never removes it. A state written by 0.8.1 or earlier (no
`unknown`) was counted by other rules, so its counts start again at 0. When both patches are on disk and a start fails with no crash
of either recorded, it counts for both, each against its own threshold:
nothing tells which patch made another process crash, and a loop costs the
car its HMI where a restored patch costs one press of Patch. The crash of
one counts for it alone.

| Loop | unproven restored before the start | proven |
|---|---|---|
| the patched service crashes at start, or another process does, with records | 2nd | 3rd |
| the same, every record lost | 3rd | 5th |

60 s is the healthy window: past the head unit's own start-up, when its
services come up and connect, and past the start of a CarPlay or Android
Auto session of a phone already attached; a failure after it is the
watcher's to see, as a crash of its own. A healthy start is the service
running 60 s without a break, as the watcher sees it; the shutdown
record's 60 s is the boot's system uptime (`/proc/uptime`), so a
switch-off within 60 s of the head unit's start, with no restart record,
counts as a start with no record.

Before it counts anything, the step gives back the mode and owner of a
file that holds exactly its original (the verified backup, or the proven
build's shipped file) but lacks the access its service's user needs:
carplay or aapd not readable and executable by group and other, or
aapd.conf not readable by them. carplay runs as `cia_13002` and aapd as `cia_13006`, so a stock
carplay at `0700` fails at `status=203/EXEC` and the head unit restarts as
for a crash. Only the mode and owner change, under the remount lock, with
`/` read-write for that alone. A patch of ours that lacks them is left to
the count and the restore. With nothing wrong this is a stat per file.

With nothing to restore the step's own work takes under 0.15 s; the rest
of its time is Node's start-up. A restore reads the
backup, checks it against the SHA-256 its record took when it was made (a
backup without its record only when it is the shipped file of a proven
build: carplay 22.28.3 or 21.27.4, aapd 22.39.3 and its aapd.conf, by the patchers'
tables), and checks that the binary is the backup's build (the same size and GNU
build-id, or, without a build-id, at most the patcher's bound of bytes
apart); then, under the remount lock, with `/` read-write only for the
write, it writes the backup whole to a fresh temporary file, sets the
original's owner and mode on it (as its record keeps them, else the
binary's, with at least 0755 for a binary and 0644 for aapd.conf) and checks
them, fsyncs it, renames it over the binary, fsyncs the folder,
writes the patch record, removes the pending marker and remounts `/`
read-only, checked. It refuses a backup that does not match its record:
CarPlay starts as it is, the refusal is logged and the next start tries
again. Over another build (a firmware update) it writes nothing and clears
the counter. aapd.conf another file than its backup, or with a backup that
does not match its record, is left while aapd is restored. A restore takes
a second or two; every remount is bounded (10 s), and the step gives up at
46 s whatever hangs (10 s waiting for the other boot step's restore, the two
run side by side and share the remount lock, then the restore's bound) and
exits by 56 s, after one bounded try to put `/` back read-only and
releasing the lock. CarPlay waits for the step 75 s at most
(`TimeoutStartSec`). CarPlay does
not require the step: it starts when the step has exited, failed or timed
out. The step exits 0 whatever happens, and it is in recmgr's lists, so a
timeout does not reboot the unit. Its `RequiresMountsFor=/var/opt` adds no
wait to CarPlay: `/var/opt` is mounted from fstab before `local-fs.target`,
which CarPlay is already ordered after.

After a restore the original is on disk with our backup: install-time
activation leaves the patch off while the patch record names the restore,
whatever else is left of the patch (an aapd.conf the restore did not get
to), `patchd` reports `recovered` with the count, and the app's Status row
reads `Removed — head unit kept restarting`; the Patch button applies the
patch again. A restore on a head unit that refuses our parts is recorded
`by: "refused"`, not as `recovered`, and the app says `Not supported on
this car` there.

**The watcher** (`recovery.js`, `waze-on-cluster-recovery.service`, after
the boot step) follows CarPlay by run (systemd's `InvocationID`). While a
run is active it checks every second that its main PID is in `/proc` and
asks systemd every 60 s, or at once when the PID is gone; while CarPlay is
down it asks every 5 s. A run that ended `failed` (a `Result` other than
`success`) is a crash, counted once per run, also when it ended before the
watcher first looked: unless `patchd` killed that run to activate a change
(it records the run's `InvocationID` in
`/dev/shm/waze-on-cluster/carplay-killed`), or the system is stopping: a
systemd shutdown (`shutdown.target` queued or active) or an NSM shutdown
(`var-opt-ro.service` queued or running, or this boot's shutdown record
already written, since CarPlay can die in it after the record). In the
emulator nothing in systemd marks the first seconds of an NSM shutdown,
before `var-opt-ro.service` starts (about 3 s, while the NodeStateManager
sends its clients the shutdown event); a CarPlay death there would still
count, which has not been seen. A
run a SIGKILL ended under the drop-in ends `success` (`ExecMainCode=2`,
`ExecMainStatus=9`) and is no crash either; unless `patchd` killed it (it
starts CarPlay itself) or the system is stopping, the watcher starts CarPlay
again, as after a crash below. A run that exited by SIGTERM or with code 0
is left as it is. A run active for 60 s marks the start healthy. A SIGTERM
while the system stops marks a start without a crash orderly.
The crash is written to `arm` at once, before anything else, since the next
moment can be the reboot. When that write fails, the watcher restores the
patch at once, whatever the count; this boot's crashes and starts are also
kept in memory, so an unwritable state file does not lift the start limit.

Otherwise the watcher only records the crash, and the boot step restores at
the next start. Where recmgr loaded its `carplay` entry this boot and the
head unit stays up, for a patch at `fails` plus this start's crashes at the
threshold the watcher restores the original, then starts CarPlay with
`systemctl start carplay.service`, 5 s, 15 s and 60 s after a crash, at
most three times per head-unit start, only while CarPlay is down, the
system is not stopping and `patchd` does not hold the remount lock. The run
it starts is probed at the next tick. The watcher also runs the boot step's
logic at its own start, so a start whose boot step did not run or finish is
counted, and a restore it left undone is retried. The watcher keeps the
patched binary linked as `<target>.woc-running`, since CarPlay could start
between its check and the rename; the boot step, which CarPlay waits for,
links it only if a process runs it. `patchd` removes a link nothing runs. A
shared remount lock (`remount_lock.js`) keeps it and `patchd` from
remounting `/` at once; the watcher yields to `patchd`, a boot step waits up
to 10 s for it. A stop waits for a restore in progress. Recovery never
stops, kills, restarts or reboots anything; its only action on a service is
the start above.

**By hand.** Keep the phone from connecting (unplug it, or turn off its
Wi-Fi and Bluetooth) so neither phone has a session to crash in, then over
SSH run `sh /var/opt/waze-on-cluster/current/woc-patch.sh revert` (CarPlay)
and `... woc-patch.sh aa-revert` (Android Auto: aapd and aapd.conf) and
restart the head unit. `status` and `aa-status` show what is on disk first.
Both reverts work without our backups on carplay 22.28.3 and aapd 22.39.3,
from each file's own shipped content (above).

**After a factory reset.** The reset wipes `/var/opt/public` and
`/var/opt/personal`: the app and the Store go. The tool, its patches, its
state (`/var/opt/waze-on-cluster`), the deploy tool and the restart lists in
`/etc` stay, and nothing of the tool lives in the wiped folders, so `patchd`,
both boot steps and the watcher run as before and still restore a patch
that crash-loops. Without the app, the patches are removed by installing the
Store again (the deploy tool's installer, from a computer or a USB stick)
and then the app, whose Remove patch rows work again, or by removing the
tool in the Store, or over SSH with `woc-patch.sh` as above.

## Tests

`make test` runs them all. The binary tests take `CARPLAY_BIN`, a copy of the
unit's carplay 22.28.3 (the SHA above), and are skipped without it; the
repository carries no BMW binary.

- `head_unit_test.js`: the head-unit rules on the cases cluster-render's
  `head_unit_test` runs too (`cluster-render/test/head_unit_cases.jsonl`);
  on a synthetic build HIGH, MID and HIGH F patch; no `hwvariant=` and no
  command line refuse apply with the reason and write nothing,
  `--unproven` included, and revert works there; aapd's apply refuses
  before it reads a file. `recovery_test.js` restores a patched carplay at
  a refused boot below the threshold, leaves it on HIGH, MID and HIGH F, and leaves
  `/` alone over an untrusted backup; `aapd_test.js` and
  `patchd_activate_test.js` refuse apply and the install on a refused unit,
  revert there and, for aapd, restore at boot and remount no more once
  nothing is restorable.
- `locate_test.js`: on synthetic builds written by `synthetic_carplay.js`
  (no BMW bytes), every site found, also at other offsets, addresses and code
  order and in its patched state; a signature found twice refused as
  ambiguous; damaged sites refused with their reason; apply, verify, probe
  and a byte-exact revert on a build outside the proven list; a file with
  carplay 21.27.4's build-id but not its shipped bytes read as proven, its
  first edit refused, and that build's stock bytes as its probe reads them. With
  `CARPLAY_BIN`, the sites are the offsets above.
- `mutation_test.js` (`CARPLAY_BIN`): the reference build perturbed in
  memory the way another build could differ (other addresses, a duplicated
  name string or converter, a decoy switch, swapped checks or builders, a
  third builder, a getter reading another field, inlining, a split-off part,
  a second or merged sink string, a relocation over an edit); each is found
  where it really is or refused, never found elsewhere.
- `patch_test.js`: the sites the probe reports, the SHA gate, `--unproven`,
  both panels, the sink inside its slot, the call retarget, region-bounded
  diffs, idempotence, partial patches (an Edit-1-only binary among them), the
  geometry and safe area, byte-exact revert, a binary of the backup's build
  the locator does not recognise restored whole, another build over the backup
  refused unless `--force`, a build outside the proven list, a backup of
  another build refused.
- `undo_test.js` (`CARPLAY_BIN`): builds an older release patched where the
  locator now refuses a site (`carplay_mutations.js`): `status` restorable,
  `revert` writes the original whole without `--force`, also with no
  record; a backup that is not the recorded original, or another build over
  it, refused; `apply` refused. `recovery_test.js`, `patchd_patch_test.js`
  and `patchd_activate_test.js` restore the same builds at the crash
  threshold, by CarPlay's Remove patch button and by the removal.
- `patchd_patch_test.js`, `patchd_activate_test.js`: `patchd` and both hooks
  on a copy, against a fake `/proc`, `mount`, `systemctl` and recmgr list:
  both activation branches (a restart only with the recmgr entry and the
  drop-in loaded), every row of the install table, the marker, the
  link's lifetime, `/` read-only after every path, no `systemctl`, `kill`
  or `reboot` from the hooks, and boot recovery's restore reported as
  `recovered` and left off by an install.
- `x86_flow_test.js`: the decoder and the flow pass on hand-written code,
  no firmware: byte and string forms that alias other registers, the
  encodings refused, register and stack tracking along every path, and
  each case it cannot follow refused (an undecodable byte, a branch into an
  instruction, a reachable indirect jump, code outside the function that
  writes the register and comes back, or too much of it).
- `aapd_locate_test.js` (`AAPD_BIN`, `AAPD_BINS`): every aapd build given
  located or refused as recorded (22.39.3 at the offsets above, 23.16.5
  refused with only the screen helper found), the reference's patched
  state found at the same sites, and the reference perturbed in memory the
  way another build could differ (other addresses, another helper layout, a
  duplicated or misplaced name string, the variant mapping changed or
  copied, each check of each edit broken, a second copy of a check, a call
  retargeted, a size constant changed, a relocation over an edit, no
  function table); each is found where it really is or refused, never found
  elsewhere. A mutation that one check alone refuses is run again with that
  check off, where the build is patched; every check has one.
- `aapd_test.js` (`AAPD_BIN`, `AAPD_CONF`, and `CARPLAY_BIN` when set): the
  coding refused unless 7 or unreadable; a site not found and a modified
  proven build refused with nothing written; a build outside the proven
  list (the reference with its build-id changed) refused without
  `--unproven`, patched with it, then like a proven one, and undone
  byte-exact; an aapd.conf that is not JSON, lacks a number, names two
  codecs, holds a key twice or already holds our geometry, a density off
  the steps and too little space refused; the backups and their records, only the edits' bytes changed,
  every density, the turn card hidden and its crops; undo with a site the locator refuses and the geometry changed; a backup not
  its record's, or without one, never written; another build left while
  aapd.conf is restored, `--force`; an apply cut short. `patchd`: both hooks
  next to carplay (installed together, removed byte-exact; another coding,
  an untested aapd, one not patchable, a modified one, an unusable aapd.conf
  or none leave CarPlay patched as ever; Android Auto's Patch button patching an
  untested aapd, which an install then keeps and a removal undoes; the running aapd
  linked and handed over; no `systemctl`, kill or reboot), a removal that
  cannot undo Android Auto refused, as is one out of time for it, with
  its state unreadable, or with aapd.conf not restored or its backup
  unrecorded, one over an aapd firmware update passing, the socket's apply deferred or with a
  SIGKILL and start where recmgr allows it, a turn-card change deferred with
  its crop override, the density's range and rate,
  and the restart guard (one at a time, 10 minutes across a restarted
  `patchd`, a failed reboot's record taken out), Zoom followed by another
  change or a restart refused as busy, a change giving up without the
  remount lock, a pending marker that cannot be written. Boot recovery: a foreign
  SIGKILL started again, crashes counted once per run, the restore at the
  second, three starts a boot, `patchd`'s kill, no start without recmgr's
  entry, crashes across boots versus a switch-off, the real boot step
  restoring copies with `/` read-only after, SIGTERM during a restore, the
  unit's timeouts and order, component.json's lists, the shutdown
  record of a SIGKILLed aapd and of an oom-killed one, the restart
  `reboot` records for boot recovery, and an install after a restore that
  left aapd.conf patched.
- `recovery_test.js`, on folders of real files (targets, backups, patch
  records, state files, the marks `shutdown_mark.sh` writes) with systemd
  and mount faked: each threshold for carplay and aapd (unproven at the
  second boot, proven and a binary the record does not describe at the
  third), a failure another process causes, a crash the watcher saw before
  an orderly stop, a switch-off within the healthy window, a restart the
  user asked for (`patchd`'s record, the watcher's orderly stop), a power
  cut, a healthy boot, the original or no backup of ours, a missing or
  corrupt state file or patch record, both services patched (a failure
  nothing explains counts for both, a crash for its service, an unpatched
  service's crash explains nothing), a backup that does not match its
  record (refused, the service starts, retried), another build, aapd.conf
  another file, the restore's mode, temporary file, marker, remount lock
  and `.woc-running` link, a remount that leaves `/` writable, a record
  that cannot be saved; the watcher's crashes per run, starts, kills, a
  crash seen once `/var/opt` went read-only, a stop during a restore; the
  system's stopping state; `shutdown_mark.sh` and its crash record; the
  boot step with an unreadable binary and its timing, its exit status, the
  units; the patchers' records; a state of 0.8.1 or earlier starting its
  counts again; a shutdown inside the healthy window with no restart
  record counted as a boot with no record; a restart record from after
  the armed boot or ahead of the clock ignored; a boot the service never
  ran in; two failed boots with and without a healthy one between; a crash
  seen once this boot's shutdown record is written, and an NSM shutdown
  under way; a boot step out of time putting `/` back read-only and
  releasing the lock; a backup whose record names another build-id
  refused; a backup without its record restored only when it is the
  proven build's shipped file; a service systemd started that died before
  the watcher looked counted as having run; on a refused head unit, a
  patch restored whatever the count, aapd.conf left patched restored too,
  and nothing remounted with nothing to restore; and, with `CARPLAY_BIN`,
  `AAPD_BIN` and `AAPD_CONF`, the real
  boot step restoring patched copies and its time. `TEST_ONLY=<part of a
  name>` runs only the tests it names.
