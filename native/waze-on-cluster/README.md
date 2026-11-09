# waze-on-cluster

Shows the CarPlay instrument-cluster map (Waze, Apple Maps or Google Maps),
or Android Auto's (Google Maps), on the instrument cluster of a BMW with an
iDrive 7 head unit (MGU), which the stock software does not do. It runs as root tools on the head unit and is
driven from the HMI by the companion app
[robertn_waze_cluster](../../apps/robertn_waze_cluster).

Both send the cluster map only as H.264 video, with no map data, so this is
a video pipeline:

1. **Ungate the second display.** A reversible patch to
   `/usr/bin/carplay` makes `KI_KOMBI_VARIANT` 7 resolve to a supported
   cluster variant, makes CarPlay start the cluster stream itself, and
   advertises the size the map is shown at ([patch/PATCH.md](patch/PATCH.md)).
   Another to `/usr/bin/aapd` and `/etc/aapd.conf` does the same for Android
   Auto (PATCH.md "Android Auto"). Each is applied only where it is
   supported, and each on its own: one that cannot be applied leaves the
   other as it is.
2. **Capture.** The phone sends the stream as RTP to the stock media-bus sink,
   AVB stream `media_transport.avb_streaming.15`; the render joins it as an
   extra receiver, in-process. It also listens on a local UDP port and a FIFO,
   which other sink patches can point at.
3. **Decode.** The receive threads only queue each packet; one decode
   thread depacketises RTP to H.264 Annex B (`src/rtp_h264.*`) and decodes it
   with the unit's Intel Media SDK / VAAPI on `/dev/dri/renderD128` to the
   picture's own Y, U and V planes, through any lost packets ("Losses").
   The GPU also copies each picture out of its own memory (`gpu_copy`;
   MSDK copies on the CPU without it).
4. **Draw.** Each frame goes to the shared cluster renderer,
   [cluster-render](../cluster-render), as a
   video layer (`src/map_feed.*`): a picture that is the last one again,
   byte for byte, as a repeat of a few bytes (not even packed), and nothing
   while the map is hidden. The renderer owns the RAMSES participant, the
   DCSM slots and one upload budget for all its clients.

## Install and remove

Install the app `robertn_waze_cluster` from the catalog of
[idrive-apps](https://github.com/robertnisipeanu/idrive-apps)
through the [mgu Store](https://github.com/robertnisipeanu/mgu-store). The
app requires this tool (0.9.3); this tool requires `cluster-render` 0.4.0 and
`mgu-deploy` 0.7.11, the deploy tool version that runs activate and deactivate
hooks and puts the tool's `recmgr` and `crashNames` names, `aapd` among
them, on the lists. Requirements resolve across catalog sources.

**Install.** The deploy tool runs `patchd.js activate` as root. On a proven
carplay build, or one CarPlay's Patch button patched before, it writes the patch
CarPlay's Patch button writes, keeps the running binary linked, and never stops,
kills or restarts CarPlay. The Store shows a restart pending; the app shows
`Status: Patched after restart`. One head-unit start runs the patched
CarPlay and starts this tool's services. The hook leaves CarPlay as it is,
and the app says why and offers CarPlay's Patch button, when CarPlay's Remove patch button was used, when
the patcher cannot find or recognise one of its edit sites in the binary or
something else modified it, when the build is outside the proven list and
CarPlay's Patch button has not patched it, or when `carplay.service` starts another program.
An update brings an older patch of ours up to date. The first install on a
HIGH F head unit also applies the patch again where 0.8.2's boot recovery
removed it, but not where boot recovery removed it after failed head-unit
starts (PATCH.md "Supported head units").

The same hook then writes the Android Auto patch, on a proven aapd build or
one Android Auto's Patch button patched before, only on a car coded
`KI_KOMBI_VARIANT` 7, and never stops or restarts aapd. On any other car,
or an aapd build whose edit sites the patcher does not find, it leaves aapd
as it is and the app's Android Auto status reads `Not supported on this
car`; on another build whose sites it finds, `Untested version`, and
Android Auto's Patch button patches it. Whatever happens to Android Auto,
the install of the CarPlay patch goes ahead.

**Remove.** In the Store, remove the app, then the tool (refused while the
app is installed). `patchd.js deactivate` reverts only our byte regions,
keeps other tools' edits and drops our backup, and restores aapd and
aapd.conf the same way; the deploy tool then removes
this tool's services and files. The renderer and the deploy tool stay. The
next head-unit start runs the original CarPlay and aapd. Details: PATCH.md
"Install-time activation".

## Using it

On the app's page, CarPlay's **Patch** button asks the root patch daemon
`patchd` to apply the patch and its **Remove patch** button reverts it, which
turns the feature off:
without the patch the phone sends no cluster stream. A change takes effect
when CarPlay next starts. `patchd` restarts CarPlay at once (SIGKILL, then
`systemctl start`) only where both of these keep that from restarting the
head unit (PATCH.md "Activation"); otherwise the app shows **Restart head
unit**, a normal head-unit restart through the Node State Manager:

- recmgr loaded `carplay` in both restart lists at this boot. The deploy
  tool writes it into both sections at install and takes it out at removal;
  recmgr reads the list at boot.
- systemd loaded the tool's drop-in
  `/etc/systemd/system/carplay.service.d/50-waze-on-cluster.conf`
  (`SuccessExitStatus=SIGKILL`), which the deploy tool installs and removes
  with the tool. Without it the SIGKILL fails `carplay.service`, whose
  `OnFailure=mediaddpserver_kill.service` sends `mediaddpserver` SIGQUIT;
  `mediaddpserver` dumps core, and recmgr and crashhandler, on neither of
  whose whitelists it is, restart the head unit.

With the drop-in loaded, any SIGKILL of CarPlay is a clean exit, also one
`patchd` did not send (the out-of-memory killer, a shell). `carplay.service`
does not start CarPlay again, so the recovery service does, where recmgr
loaded the entry: 5 s, 15 s and 60 s after it, at most three starts per
head-unit start together with those after a crash. Any other clean exit
stays as it is.

On a fresh install the patched CarPlay first runs at the next head-unit
start, which also loads the recmgr entry.

**When the patch keeps the head unit from starting.** A CarPlay crash (a
core dump, or a watchdog timeout) fails `carplay.service`, and the head
unit restarts as above; neither the recmgr entry nor the drop-in prevents
it. Another process the patch makes fail restarts it through recmgr. A boot
step that runs before CarPlay records each start of the patched CarPlay,
and the recovery service marks it healthy once CarPlay has run 60 s. The
next boot step counts a start that was not healthy as failed only on
evidence: a crash of CarPlay, or a restart recmgr or crashhandler
recorded, whatever process it names. A switch-off after the healthy window,
a restart asked for or a start CarPlay never ran in counts nothing; a start
that ended with no record (a power cut, or a shutdown inside the window
with no restart record) is unknown, and two of those in a row count one,
the accepted cost of catching a loop whose records are lost. It restores
the original binary before CarPlay starts after the first failed start for
a build outside the proven list, and for a proven one after the second
with no healthy start between, in a second or two, deciding from small
state files only. CarPlay comes back without the map, its Status row reads
`Removed — head unit kept restarting`, and its Patch button applies the
patch again.
Where recmgr loaded the entry and the head unit stays up, the recovery
service also restores at the threshold and starts CarPlay again, at most
three times per head-unit start, 5 s, 15 s and 60 s after a crash.
Details: PATCH.md "Boot recovery".

To break a loop by hand: keep the phone from connecting (unplug it, or turn
off its Wi-Fi and Bluetooth) so CarPlay has no session to crash in, then over
SSH run `sh /var/opt/waze-on-cluster/current/woc-patch.sh revert` and
restart the head unit.

**Android Auto.** Android Auto's **Patch** and **Remove patch** buttons
apply and revert its patch, **Zoom out** and **Zoom in** change the
density the phone is told (150-400 dpi, 180 unless chosen; a lower one shows
more map with smaller text), and **Hide phone's turn card** crops Google Maps'
turn card off the top of the map, or puts it back. Each takes effect when
aapd next starts:
`patchd` restarts aapd at once (SIGKILL, then `systemctl start`) where
recmgr loaded `aapd` in both restart lists at this boot, otherwise at the
next head-unit start (**Restart head unit**). `aapd.service` has no
`OnFailure=`, so no drop-in is needed. A crash or a SIGKILL of aapd that
`patchd` did not send is followed by `systemctl start aapd.service` 5 s, 15
s and 60 s later, at most three times per head-unit start, where recmgr
loaded the entry, and aapd has its own boot recovery: the second failed
start of the patched aapd with no healthy start between restores the
original aapd and aapd.conf before aapd starts.

The phone sends a 1280x720 picture with its map in the top 1280x564, the
map area's shape; the decoder crops that part (`src_crop` in
`waze-on-cluster.conf`) and scales it into the 1088x480 map area. With the
turn card hidden the crop also cuts the card's rows, which grow with the
density, so `patchd` passes that crop to the render
(`/var/opt/waze-on-cluster/aa-crop.env`), and a turn-card change, or a
density change while the card is hidden, takes effect at the next head-unit
start.

Set **Instrument cluster > Central display area** to **Map view**: the
cluster shows the stock map, the Waze map replaces it while CarPlay's cluster
stream is live and shows a map, and the stock map is back when the stream
ends or shows no map. **Off** works
too, with an empty centre instead of the stock map. With Route preview or
Assisted Driving View the cluster's own content takes the centre. After
changing the setting, CarPlay may need reconnecting before the map returns:
the change can freeze the phone's cluster stream (below).

**BMW's turn box.** While the map shows, BMW's own CarPlay turn-by-turn box
on the cluster stays hidden: the map's layer leaves cluster-render's guide
hold on (cluster-render README "Guide hold"). The app's **Hide BMW's turn
box** checkbox turns that off and on at once, with no restart: the app sends
`{"cmd":"guide","hide":0|1}` on the control socket, the service sets the
layer's `guide_hold` and keeps the choice in
`/var/opt/waze-on-cluster/state/waze-on-cluster.state.conf` (`state_file`,
written whole and renamed into place), which outlives restarts and updates
and replaces `hide_bmw_guidance` (1) of the settings. The service makes the
folder, owned by `hmi_5000`, at each start. A value other than 0 or 1, or a
state file that does not read, hides the box, with a warning in the log.
The status event carries the choice as `hide_bmw_guidance`, and
`guide_disabled` 1 while cluster-render's mapping guard keeps the hold off
(cluster-render README "Mapping guard"): the box then shows whatever the
choice. With a
cluster-render before 0.3.7, which has no `guide_hold`, the switch changes
nothing.

Then connect the iPhone to CarPlay. Waze needs its server-side flag 1006 for
the alternate display. The phone sends SPS, PPS and an IDR only at the start
of a stream, so the render service runs from boot, and decodes from the
stream's first SPS whether or not the map is shown. The app need not be open.
It starts once BMW's media bus is up, after `ufipc_daemon.service` and
`avbstreamhandler.service` (ordering only), and joins the bus at once,
retrying every second while the bridge is not there.

The map comes up at the stream's first non-blank picture in the native
central-map slot `0x100` (scene `0x010004f0`, 1088x480), under the gauges,
and stays while the stream is live: while carplay holds the phone's
connection for the cluster stream, TCP port 6031 owned by carplay's uid
13002 in `/proc/net/tcp6`, however few frames come (how often a static map
sends a frame depends on the phone: one about every ten seconds, another
about 30 a second).
When that connection ends (the phone unplugged, the session or the stream
ended) the map is taken down at the next check, once a second, and the slot
returns to the cluster's own content; the next stream maps it again. No
frame for `idle_ms` (60 s) takes it down too. Without that connection
(carplay took another port) or if `/proc/net` cannot be read, a frame within
`stall_ms` (6 s) counts as live. That the phone keeps the connection through
a static map and closes it when the stream ends is still to be confirmed on
the car.

CarPlay starts the cluster stream as soon as the phone connects (PATCH.md
Edit 3), before any navigation app draws on the cluster, and the phone sends
a blank picture until one does: on the car about 100 blank frames in the
first 2 s, then nothing; in the field the map came once a route started in
Waze or Google Maps, and stayed after it ended. So the stock map stays until
the first non-blank picture, and the app shows `Cluster map: Waiting for
the phone's map`. A picture that stays blank for `blank_ms` (10 s) after a map gives
the slot back to the stock map; the app then shows the same
line, or `Cluster map: Frozen` when the blank frames keep coming
fast enough to be a frozen stream (see "A frozen stream" below). The next
non-blank picture takes the slot again. A map always draws something, so a
blank spell shorter than that keeps the map up, and the map never comes and
goes faster than once every `blank_ms`.

While the map is up, a session the cluster ends (the stock map taking the
slot back, a refusal, no answer) is started again after 2, 5, 10, 20, then
every 30 s. The app shows `Cluster map: Starting…` while the renderer starts, and a
map error only when the renderer stays unreachable for 15 s.

A frozen stream, frames still coming (on the car 15-21 a second) but all the
same blank picture, is frozen after 10 s (`freeze_ms`, at least
`freeze_min_fps` 3 a second): the stock map shows (in Off, an empty centre)
and the app shows `Cluster map: Frozen`, not `Cluster map: Waiting
for the phone's map`. The map is down while the stream is frozen
or its picture has stayed blank for `blank_ms`, whichever comes first; with
both at 10 s they come together. The map is back with the next non-blank
picture. A stream that has shown no picture yet is frozen only after three
times as long (30 s), frames still coming: a phone starting its stream
sends the same blank picture for a couple of seconds and pauses. A stream
is CarPlay's cluster stream while it is live: its frames are those decoded
while it is live and those decoded at most 2 s before it became live (the
frame that made it live, or a first frame decoded before the connection
was read, which is read once a second). An older frame, such as the last
stream's still on its way after it ended, does not count. Blank: all but
one in a thousand of the sampled luma values within 17 levels. A static map
is never frozen, however many frames the phone sends: some phones send one
about every ten seconds, others keep their full rate.

A render that missed the stream's start decodes nothing until a new stream:
reconnect the phone.

**Losses.** The phone sends a keyframe only at the start of its stream, so
the render decodes through a lost packet:

- The AVB bridge's callback and the UDP reader only copy each packet into a
  queue of 4096 packets or 6 MB (about 4 s of the stream) and return; one
  decode thread depacketises and decodes them in order. Past the queue's
  headroom the oldest packets are dropped, and counted. A slot keeps a
  buffer of at most 2 KB once its packet is out, so large datagrams leave
  no memory behind. Every thread runs at the service's nice 0. The FIFO
  reader, a byte stream that holds its writer back, decodes on its own
  thread.
- Every frame is decoded, since each builds on the one before, but only the
  newest gets a picture. While a later frame's first packet (a new RTP
  timestamp) is already queued, the frames decoded meanwhile skip the
  picture: no scaling, packing, level rounding or hand-off. A decode slower
  than the phone's frame rate thus skips pictures instead of falling
  behind; the wait in the queue stays within about one picture's output
  and a few frames' decoding. A decoder slower than the stream even
  without the pictures is logged when every packet has waited over 200 ms
  for 5 s (`waze: the decoder is behind the stream: …`), and again when a
  packet waits under 100 ms (`waze: the decoder caught up with the stream
  after 37 s behind it (packets waited at most 2400 ms)`).
- A sequence gap drops only the NAL unit it cut (an FU-A); every slice after
  it goes to the decoder, which conceals what is missing. The frames are
  shown as decoded: the map can show smudges until the phone codes those
  areas again, and MSDK's per-frame `Corrupted` flags are counted. A decoder
  that has never started still waits for the stream's first SPS. Only
  30 access units in a row that fail to decode give the sequence up; the
  decoder then waits for a keyframe as at the start, until the next
  stream.
- A gap followed by an SPS is a new stream (the phone reconnected or
  CarPlay asked for a new one), logged as `waze: a new stream from avb
  (sequence 40000, expected 17)`, not a loss. Any other gap is a loss,
  logged with the jump, its source and whether the receive queue dropped
  it (`waze: lost 5 packets of the stream from avb (sequence 1240, expected
  1235); decoding through it`), and so are the first corrupted frame after
  it and the first clean one begun after it (`waze: the picture is clean
  again 150 ms after the loss at 12:39:50 (3 corrupted frames)`), at most
  10 such lines a minute.
- Decoding through a loss, the map shows and the status has no reason
  for it; `probe.txt`'s `stream:` line says `lost part of the stream at
  12:39:50; decoding through it` until that clean frame. The status's
  `reason`, which the app shows, is `Stream lost`
  while the stream arrives with nothing decoded for 2 s (a render started
  mid-stream, or a loss the decoder does not decode through) and while the
  decoder has given up its sequence. The detail, with when that began, is
  in the log (`waze: stream lost: the stream arrives with nothing decoded
  since 12:40:50`) and in `probe.txt`. A status line whose only change is
  the stream's reason is logged at most once every 10 s.

**Where the map shows** (the HUD contributed by
[eroobeeroo](https://github.com/eroobeeroo), the choice of both or one after
[wesleyredhat](https://github.com/wesleyredhat)'s #53). `cluster` (on by
default) and `hud` (off by default) choose (`src/map_feeds.*`); the app's
**Show on** sets them to the cluster, the HUD or both, and each change takes
effect at once:

| | |
|---|---|
| Cluster only (the default) | the map on the cluster as always |
| HUD only | no map scene on the cluster, so the KOMBI shows its own content and the guide hold is not asked for; the HUD takes each frame itself, with the whole upload budget, at most `hud_fps` a second |
| Both | the cluster first; the HUD gets a copy, and up to half the budget, only what it spends (cluster-render README.md "Upload budget") |
| Neither (the configuration's alone; the app does not offer it) | no feed and no connection to the renderer; every frame is still decoded, since the phone sends keyframes only when its stream starts, but none gets a picture: no VPP, readback, rounding, copy or hand-off. Turned on again, the map shows with the next frame, without waiting for a keyframe |

cluster-render's `hud` target is slot 0x323 (its README.md "The HUD").

- **Off costs nothing.** With the HUD off there is no HUD connection to the
  renderer, no layer, no worker thread and no copy, so nothing HUD-related
  is asked of the KOMBI. On a head unit that refuses the tool, nothing of
  either runs.
- **The cluster first.** A new picture goes to the cluster's feed without a
  copy. The HUD's feed gets a copy of the newest when its next frame is
  due, at most `hud_fps` (10) a second, and a worker thread at a lower
  priority scales it to the size it is shown at (`src/atlas_scale.*`, area
  averaging, `hud_scale_down`); the main loop only copies and hands over,
  so the cluster's frames never wait for the HUD's. A picture the same as
  the last (a still map) goes to both as a repeat: the cluster's last frame
  and the HUD's last scaled one again, a few bytes each, with no copy and
  no scaling, and both layers stay shown. The copy goes no further than
  the cluster's map: one decode, one capture. cluster-render writes the
  HUD's frames, in changed tiles and never packed (a frame goes while the
  budget has any allowance left, into debt, and waits while it is in debt),
  from the HUD's share: up to half the
  budget, what it does not spend going back to the cluster at once.
- **Size** (`hud_width_pct`): Standard (0, the fitted box, about 57 % of the
  HUD's width, 453x200), or 30 % to 85 % (680x300, the widest: wider is cut
  off at the HUD's edges, and a larger saved size is held to it), the
  picture's shape kept; the app's **Size** slider steps it by 5 %.
  A new size is a new layer at that size.
- **Position** (`hud_offset_x`, `hud_offset_y`): pixels of the HUD's scene
  moved from the centre (+x right, +y down), held so the map stays whole
  inside the HUD (the status then reports the offset as far as it goes); the
  app's **Move left**, **Move right**, **Move up** and **Move down** move it
  10 px a press and **Centre** puts it back. A move only changes the layer's
  place, so it shows at once.
- **Made for a HUD.** At the standard size a frame is 135,600 B instead of
  783,360. On a HUD dark pixels are see-through: the corners are rounded
  (`hud_round_corners`, a twelfth of its height) and fade to black, and a
  night map's background is made black (`hud_black_background`): the
  frame's most common shade, followed frame to frame, and the shades just
  above it, so the HUD shows the roads, route and labels without Waze's
  grey-blue field; a day map (light background) is left as it is. Green is
  never made black, and does not count towards the background.
- **It gives way.** The HUD map steps aside while the HUD's media and list
  pop-ups show (cluster-render `hud_yield_slots`), and never takes the HUD
  from BMW's own content.
- **Disabled after errors.** cluster-render's mapping guard turns the HUD
  off after two cluster restarts within 10 s of its mapping; the status
  says `hud_state` `disabled`, the app shows `HUD map: Disabled after
  errors`, and its frames are no longer copied or scaled. The cluster's map
  is not affected. Choosing Cluster and then the HUD again does not undo it; as
  root, `cluster-render guard-reset` does.
- Not with `target = panel`, which shares the HUD's scene id.
- The settings (`cluster`, `hud`, `hud_width_pct`, `hud_offset_x`,
  `hud_offset_y`) are saved by `patchd` in
  `/var/opt/waze-on-cluster/map.state.conf` (`map_state_file`), which this
  tool reads over its configuration at each start, and the app changes the
  running render at once. They stay out of `waze-on-cluster.local.conf`,
  which waze-on-cluster 0.8 refuses to start with: `patchd` moves any it
  finds there into the state file at its start and at an install. Control:
  `{"cmd":"hud","on":"1"}`, `{"cmd":"hud","hud_width_pct":"70"}`,
  `{"cmd":"hud","hud_x":"40","hud_y":"-20"}` (strings; an axis left out
  stays).
- Per driver profile: the settings are shared by every driver profile until
  the app's **Own settings for this driver** gives one its own copy. The app
  tells `patchd` the profile in use (`{"cmd":"profile","id"}`) and the
  checkbox (`{"cmd":"profile-own","id","on"}`); `patchd` keeps the shared
  settings and each profile's own in `/var/opt/waze-on-cluster/profiles.json`
  and writes the ones in effect into `map.state.conf`, as for any change,
  and the app moves the running render to them. `profiles.json` is written
  first; a write that fails is tried again, and at its start `patchd` writes
  `map.state.conf` again from `profiles.json` when a write between the two
  was cut short. `profile-own` is taken only for the profile last told.
  `map.state.conf` keeps its format, so an older version reads the settings
  in effect; changes made under an older version are replaced by
  `profiles.json`'s at the next start while a profile has its own settings.
  A missing, corrupt or unreadable `profiles.json` means no profile has its
  own settings (logged).

From a shell, `waze-on-cluster -c <conf> run map|panel` (`sail` is an alias
of `map`) sets the cluster target until the service restarts and prints the
status.

## Status and diagnostics

Once a second the render writes, in `/run/waze-on-cluster/` (mode 0644):
`status.json`, the status the app shows (`state` idle, starting, showing,
frozen or error, or unsupported on a head unit that refuses the tool; `page`; `reason`; the shared upload cap as `upload_cap_kb_per_s` and
why as `upload_reason`, also in the app's status event); `probe.txt`, the
sources, the depacketiser (`rtp: packets nals aus aus_by_marker seq_gaps lost
dropped bad`), the receive queue (`queue: N dropped, at most M of 4096 packets waiting, K KB
held`), its backlog (`backlog: the oldest packet waiting N ms, the longest
wait M ms; K frames decoded without a picture to catch up`, with `; the
decoder is behind the stream` while it is), the stream (`stream: whole` or
the detail above), the decoder and the order it outputs in, the last SPS
(`sps:`, as the log's SPS line), the frame timing since the start (`frames
ended by the marker bit ...`, as the log's minute line), the late-join
verdict, the noise filter's tile differences and the renderer's own status. `patchd` answers
`{"cmd":"logs"}` with CarPlay, AVB and DLT marker lines and its own log.

Every line the render logs also goes to
`/var/opt/waze-on-cluster/log/waze-on-cluster.log` (`log_file`) with its UTC
time, so it outlives the journal's few minutes on the unit, restarts and
reboots; past `log_file_kb` (512) the file becomes `waze-on-cluster.log.1`.
The service makes the directory, owned by `hmi_5000`, at each start; the
deploy tool removes it with the rest of `/var/opt/waze-on-cluster`. The log
has the status when its state, reason or target changes, each loss and a
decoder behind the stream (above), and once a minute while frames come, how
many were decoded, how many of those without a picture, changed and flagged
corrupted, whether the last picture was blank, the losses and the packets
they cost, the packets the receive queue dropped, and how long the decoded
packets waited in it:
`waze: 1840 frames decoded in the last minute, 12 without a picture to
catch up, 1 changed, 0 corrupted; the last picture is not blank; 0 losses
(0 packets), 0 dropped by the receive queue; packets waited 2.4 ms on
average, 61 ms at most`. A second line says how the minute's RTP frames
ended, by their marker bit or by the next frame's timestamp, how long each
waited from its last packet's arrival until it went to the decoder, and how
many frames the decoder took in after each frame before that frame came out
(0 when each comes out as it is decoded; one it takes and never outputs adds
to frames already waiting in it, never to later ones):
`waze: frames ended by the marker bit 0, by the next timestamp 1840; handed
to the decoder 31.6 ms on average after their last packet, 66.0 ms at most;
the decoder held 0.0 frames on average, 0 at most`.

Each sequence's SPS is logged once, with its source:
`waze: SPS from avb: High (profile_idc 100) level 4.2, 1920x900,
pic_order_cnt_type 0, bitstream_restriction max_num_reorder_frames 0
max_dec_frame_buffering 4` (`bitstream_restriction none` without it, `none
(no VUI)` without a VUI). The decoder logs the first B slice
(`decoder: B slices in the stream: output in display order from the next
SPS`) and each sequence that starts in another output order than the last
(`decoder: output in display order from this sequence`, or `decoding
order`).

For a report, use the app's **Logs** page (below), or run as root on the
unit `sh /var/opt/waze-on-cluster/current/collect-logs.sh [-o <folder>]`:
it writes `woc-logs-<UTC date and time>.tar.gz` into the folder (`/tmp` by
default) and prints its path. The archive holds both tools' log files
(cluster-render's too), the journal lines the journal service keeps, the
files in `/run/cluster-render/`, the render's `status.json` and
`probe.txt`, boot recovery's counter and shutdown record, this boot's
journal of the render, `patchd`, the boot step, the recovery, capture,
journal and logs services, cluster-render and carplay, the list of boots,
`ls -l` of both tools' folders, their `*.local.conf`, the date and the
uptime, and `wifi.txt`: CarPlay's Wi-Fi band (below), each P2P interface's
`/proc/net/dev` counters and the kernel's last 60 802.1X and DHCP lines for
the P2P interfaces (from the journal, else `dmesg`).

**CarPlay's Wi-Fi band.** The head unit runs two wireless-CarPlay P2P
groups, one on 5 GHz and one on 2.4 GHz, bridged on `wlan_bridge`
(`p2p-wlan0-N` with Broadcom's driver, `p2p-wfd0-N` with NXP's; which name
is which band is not fixed). On 2.4 GHz, CarPlay's streams and the cluster
stream together can be more than the link carries, and audio cuts out.
`patchd` (`patch/wifi_band.js`) reads `/proc/net/tcp` and `tcp6` every 15
s. While carplay holds a connection to the phone, it compares each P2P
interface's received bytes in `/proc/net/dev` over 5 s at the start, then
every 2 minutes: the phone's is the one receiving at least 1 KiB/s and 4
times any other. Its band is the frequency of that interface's group, which
`patchd` reads from wpa_supplicant over D-Bus with `busctl` (root only, by
the bus policy; never starting wpa_supplicant): the interface's `P2PDevice`
`Group`, then the group's `Frequency`. CarPlay over USB, too little traffic
to tell, or a frequency that cannot be read or is in no band leave the band
unknown; it is never taken from the interface's name or order. A band once
known stays for that CarPlay session through a still screen, while its
interface still receives the most, at least 128 bytes/s. `patchd` logs the
band when it is first known and when it changes (`CarPlay Wi-Fi: 2.4 GHz
(p2p-wlan0-1, 2462 MHz, 1180 KB/s)`), unknown after a band, and a frequency
it could not read; it sends it to the app in its status (`wifi`) and writes
it, with each P2P interface's rates, to
`/dev/shm/waze-on-cluster/wifi-band.json` (0644).

Sockets: `/dev/shm/waze-on-cluster/ctl.sock` (the render's status and log,
and the turn-box switch, group `tm_oap_app`; a connection also starts the
service),
`/dev/shm/waze-on-cluster/patch.sock` (`patchd`, `root:tm_oap_app`, 0660)
and `/dev/shm/waze-on-cluster/logs.sock` (the logs service, group
`tm_oap_app`; a connection starts it). The app's README lists the
protocols.

## Logs

**The journal service** (`waze-on-cluster-journal.service`,
`patch/journal_keep.js`) keeps the tools' part of the journal on
`/var/opt`: the unit's journal is volatile and holds about four minutes on
the car, where the navigation writes some 16,000 lines in that time. Its
timer starts it 90 s after boot, so it takes nothing from the head unit's
start; no unit waits for it, and it is ordered against no head-unit unit.
It reads what this boot logged so far, then follows this boot's lines of the
waze-on-cluster services, cluster-render, carplay, recmgr and crashhandler,
systemd's lines about them and their sockets, and the kernel's, and appends
them every 2 s to
`/var/opt/waze-on-cluster/journal/journal.log`, opening the file only for
the write. Each line is at most 1 KiB: the time (UTC, µs), the unit, the
program and its pid, the message. Past 384 KiB the file becomes
`journal.log.1` and the older ones move up to `journal.log.9`: at most 10
files and 3.75 MiB. The tools' lines (waze-on-cluster, cluster-render) and
the others each have 128 KiB a minute, with a minute's worth in reserve, so
a flood of one kind never pushes out the other; what comes faster is
dropped and counted (`journal-keep: dropped N other lines (M KiB) over the
rate limit`). That keeps at least the last 10 minutes whatever the units
log: the rotated files always hold more than 3447 KiB, and 10 minutes let
through at most 2816 KiB. What this boot logged before the start is written
whole, within the files' caps (its newest lines when it is more), not
limited. On the car these units wrote 189 lines in four
minutes with the map up (about 7 KiB a minute; carplay logs to DLT), the
tools' own log files at most 67 KiB in any 10 minutes over two days, and the
kernel about 1,650 lines (140 KiB) in a boot's first minute, so the limit is
met only by a fault flooding the journal, and the files hold hours. A start
writes what this boot logged after the newest line the files hold, all of it
when this boot has no such line, then follows from the last entry's journal
cursor, as does a journalctl started again; nothing depends on the clock.

**Save logs** on the app's Logs page asks the logs service
(`waze-on-cluster-logs.service`, `patch/logs.js`) for an archive: with at
least 64 MiB free on `/var/opt` it runs `collect-logs.sh -o
/var/opt/waze-on-cluster/saved`. An archive over 16 MiB is deleted, then the
oldest ones until at most 5 are left, together at most 20 MiB. One save
runs at a time, at most one every 10 s.

**Download.** The logs service serves the saved archives to a phone or a
laptop on the car's Wi-Fi hotspot, at `http://<wlan_bridge's
address>:8475/<token>/` (on the car `172.16.222.97`), which the Logs page
shows as text and as a QR code. The page lists the archives, each a
download. The token is 128 random bits, new at each start of the service;
another path, a wrong token or another `Host` is refused, and 10 wrong
tokens or 120 requests in a minute lock a client address out for the rest
of it, which may download two archives at a time. It serves only GET and
HEAD of the archives, no folder, link or other file.

The logs service starts when the app opens the Logs page (its socket) and
stops by itself 10 s after the page is closed, or 15 minutes after the last
Save logs if that is later. It listens only on `wlan_bridge`'s address while
it has one, checked every 5 s. It starts `waze-on-cluster-logs-fw.service`
(root, `logs_fw.js`) first and stops it with itself (`BindsTo`): that makes
`saved/`, removes any rule tagged `waze-on-cluster-logs` from the firewall's
`ip filter input` chain and keeps one at its top, `iifname "wlan_bridge"
tcp dport 8475 accept`, checked every 30 s and put back after a ruleset
load, and removes it when it stops, however it stopped (`ExecStopPost`).
From its second check on, a logs service neither running nor starting (its
start limit refused it) ends it too, the rule removed. A unit without that
chain gets none.

Every OAP app is in `tm_oap_app` and can use the logs socket, as it can
`patch.sock`; the token keeps out the other devices on the hotspot.

## Map size and upload

**Size.** The map covers the cluster's central map area, the stock map's
1088x480 rect on the 1920x720 panel (138x61 mm). The patch advertises exactly
that, so the phone renders those pixels, the decoder passes them through
unscaled and the texture maps onto the area one to one. In the whole-panel
slot `0x203` the map is centred at (416, 120) with everything around it
transparent. `tex_size` must equal the size the patch advertises.
`src_crop` takes part of a picture of one size (a phone that draws its map
in part of a larger picture) and scales it to `tex_size`; a picture of any
other size is used whole. `map_rect` draws the map stretched into a rect of
the current target instead of its map area.

**Texture.** The planes are packed into one R8 atlas, Y in rows 0–479 and U
and V side by side in rows 480–719 (783,360 B), which the renderer's shader
converts as full-range BT.601. The renderer keeps one persistent buffer and
writes only changed 64x64 tiles in place, one flush per update, so the
cluster never shows a half-written frame. (A new texture per frame held the
map to 3–4 frames a second on the car.)

**Noise filter** (`dirty_threshold` 8, `dirty_settle_ms` 1000). The phone
keeps coding residuals on a still map, mostly within 3 levels. A tile is
written only when it differs from what the cluster holds by more than the
threshold, or after `dirty_settle_ms`; a parked map costs about 1.6 updates a
second.

**Fast motion** (`pan_blocks` 1). An update whose exact write costs more than
261,120 B and more than the budget allows goes as 4x4 colour blocks at a
third of the bytes, slightly softer (38.8 dB mean on a recorded Waze capture;
street names and icons readable). The next update the budget covers, or a
quarter second later, brings the exact picture back. `waze-on-cluster
bench-pack <I420 file>` times the packing on the unit.

**Rate.** `target_fps` (30) is a ceiling on an exact grid; only the newest
frame waits, so the map skips ahead and never shows a backlog, and the
decoder skips the pictures of frames it is behind on ("Losses"). Real Waze
sent up to about 16 frames a second on the car.

**Upload budget.** The budget belongs to cluster-render and is shared by all
its clients: a 5500 KB/s ceiling, stepping down by 500 KB/s to 2000 KB/s on
sustained congestion and recovering slowly. Set it with `upload_kb_per_s` in
`/var/opt/cluster-render/cluster-render.local.conf`. This tool's own
`upload_kb_per_s` and `upload_burst_ms` keys are accepted and ignored with a
warning naming that file. On the car 8000 KB/s made the cluster flash black;
6000 KB/s stayed smooth with packed motion and no failed writes, but the
cluster once removed a shown map after a minute averaging 5774 KB/s, so the
ceiling sits below that. Real Waze
transitions peaked at about 3.2 MB/s. Every head-unit participant shares
the link to the cluster, so an upload that outruns it can drop other contents
too. Details: the cluster-render README.

Settings are in `waze-on-cluster.conf` (documented inline). Overrides go in
`/var/opt/waze-on-cluster/waze-on-cluster.local.conf`, which updates keep;
the app's map settings in `/var/opt/waze-on-cluster/map.state.conf`.

## The patch

`patch/carplay_patch.js` (Node 8, no dependencies) makes four edits after a
verified full backup. `patch/carplay_locate.js` finds each edit's site in the
build by its code and data and checks it before any write, so the patch
applies to any carplay build in which every site is found exactly once and
passes its cross-checks; a build where one does not is left alone, with the
reason. The install patches only a proven build; another build is a conflict
until CarPlay's Patch button (or `apply --unproven`) patches it once. `carplay_patch.js
probe` (`woc-patch.sh probe`) shows where the sites are in a binary and
whether it is patchable, without writing. A proven build also has to match
its shipped SHA-256 before the first edit:

| carplay | build-id | sha256 |
|---|---|---|
| 22.28.3 | `5de62156575d737391ea42254e778a97dbbb51f8` | `ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50` |
| 21.27.4 | `389c1d0536f8cc9283c156410f2f2634a9f952c0` | `fb5d6b179ad4f62a968697d21e84dbb80adc1ad8e618a6f02c6a9a0467aca1ac` |

21.27.4 was confirmed on a G07 X7 through CarPlay's Patch button
([#95](https://github.com/robertnisipeanu/idrive-apps/issues/95)). The
sites in carplay 22.28.3:

| Edit | What it does | Where (carplay 22.28.3) |
|---|---|---|
| variant | jump-table slot for `KI_KOMBI_VARIANT` 7 → IC_HIGH or IC_MID | 4 B at `0x2ba4f0` |
| sink | second-stream sink URL: stock `avb://`, or `udp://…`, `file://…` | ≤ 59 B at `0x2a20b0` |
| selfstart | `onSuggestUi`'s `setStreamStatus(AVAILABLE)` call → `requestStream()` | rel32 at `0x18176c` |
| resolution | IC_HIGH geometry → 1088x480 px at 138x61 mm, safe area 504x368+286+72 | four 8 B immediates at `0x20ca32`–`0x20ca7c` |

`patchd` and the install hook apply `--panel high` with the stock sink.
`woc-patch.sh` is the shell path, for experiments and for undoing the
patches without the app: `status`, `probe`, `verify`, `revert [--force]`,
`aa-status`, `aa-revert [--force]` (aapd and aapd.conf), and `apply
high|mid` with `--avb` (the app's
sink), no flag (`udp://127.0.0.1:20557`), `--capture` (FIFO) or `--sink
<url>`, plus `--res WxH|stock`, `--safe WxH+X+Y|full` and `--unproven`. It
remounts `/` read-write only for the write. **Never `systemctl stop` or
`restart` carplay**: CarPlay dumps core on SIGTERM, which power-cycles the
head unit, and so does any other failed CarPlay end; with the drop-in
loaded, a SIGKILL is a clean exit.
`revert` restores any binary of the backup's build
(`/usr/bin/carplay.woc-orig`), and refuses another build (a firmware update)
unless given `--force`.
Every write of carplay, aapd or aapd.conf (apply, revert, boot recovery's
restore) gives the file its original's owner and mode, recorded with the
backup, with at least 0755 for carplay and aapd and 0644 for aapd.conf, set
explicitly on the new file and checked before it replaces the old one,
whatever the umask it runs under. carplay and aapd run as their own users:
a carplay left `0700` fails to start (`status=203/EXEC`) and the head unit
restarts in a loop. An apply or revert that finds nothing to change, and
boot recovery before the service starts, give a file the service's user
cannot run or read its original's mode and owner.

## The Android Auto patch

`patch/aapd_patch.js` (Node 8, no dependencies) edits `/usr/bin/aapd` and
`/etc/aapd.conf` after a verified full backup of each, only on a car coded
`KI_KOMBI_VARIANT` 7, read from `/dev/shm/rawcodingdata` the way aapd reads
it. `patch/aapd_locate.js` finds each edit's site in the build by its code
(PATCH.md "Android Auto"), and a build where a site is not found is refused
with the reason. The install patches only a proven build (aapd 22.39.3,
sha256 `6880f54c…`, by build-id); another build is a conflict until Android
Auto's Patch button (or `apply --unproven`) patches it once. aapd.conf is edited
when it has the 12.3" entries and numbers the geometry sets, and refused
otherwise. Its edits, at the proven build's offsets:

| Edit | What it does | Where |
|---|---|---|
| gate | `secondVideoEnabled()`: `KombiVariant <= 1` → `<= 2` | 1 B at `0x998c3` |
| selfstart call | `onNewNavigationStatus()`'s `setNaviStreamStatus` call → `requestStream()` | rel32 at `0x1d43ec` |
| selfstart guard | `requestStream()`'s `je` on a field never set → `jmp` | 6 B at `0x1d3f5d` |
| screen | KombiVariant 2 reads the 12.3" profile, as 0 does | 16 B at `0x15b440` |
| geometry | aapd.conf's 12.3" entry: a 1280x720 stream with the UI in its top 1280x564 (or whole with the turn card hidden), the chosen density, the safe area 592x432 at 336,84 | numbers in `KOMBI_DISPLAY` |

`aapd_patch.js status | apply [--dpi n] [--card shown|hidden] [--unproven] | revert [--force] [--drop-backup]`
with `--target`, `--conf` and `--coding` for copies. `status` names the
build-id, the version and whether the build is proven. `revert` needs no
locator: each file's backup is trusted by its recorded SHA-256 and written
over a file of its own build. **Never `systemctl stop` or `restart` aapd.**

## Safety

- Every head-unit variant runs, HIGH F included (0.8.2 refused it).
  Nothing of ours runs only where the variant cannot be read: a kernel
  command line with no `hwvariant=`, or two that disagree (PATCH.md
  "Supported head units"). There no patch is written, a patch already on
  disk is restored at the next boot before carplay or aapd starts, and
  this tool's capture and the cluster renderer stay idle; the app shows
  `Status: Not supported on this car` in both sections.
- The patch is the only change to a BMW binary. It is reversible and fully
  backed up, checks every edit site before writing, refuses a build where a
  site is not found or not recognised, is written by atomic rename, and
  takes effect at the next CarPlay start. Its first apply to a proven build
  (PATCH.md) also requires the whole file to be the shipped one; on any
  other build it needs `--unproven` (CarPlay's Patch button passes it, the install does
  not), and once its backup exists only the four sites are checked.
  Removal restores the backup's bytes in our edits' regions, or the whole
  backup over a binary of its build the patcher no longer recognises.
- The Android Auto patch is written only where every aapd site is found
  and aapd.conf has what the geometry sets, on a car coded
  `KI_KOMBI_VARIANT` 7, with enough free space on `/` for the backups and a
  temporary copy. Its first apply to a proven build also requires the whole
  file to be the shipped one; another build needs `--unproven` (Android
  Auto's Patch button passes it, the install does not). Removal restores both
  files.
- Every revert works from our verified backup; on carplay 22.28.3, aapd
  22.39.3 and its aapd.conf also without one, when
  an edit of ours reads in the file, from its own shipped content, kept only
  when it is the shipped file whole. A file with nothing of ours in it is
  left as it is. Each file is written with its original's mode and owner whatever
  the umask. A removal of the tool fails while an edit of ours still reads in
  carplay or aapd, so the tool stays with its recovery until it is undone.
- `patchd` runs as root. It accepts only `apply` (panel `high|mid`, mode
  `avb|udp|fifo`), `revert`, `aa-apply`, `aa-revert`, `aa-density` and
  `aa-card` (one of the fixed values, at most one change every 10 s), `reboot`, `status`,
  `logs` and the capture commands; no value reaches a shell or a path. It
  checks `/` is read-only again after every write. It applies no patch and
  restarts nothing on its own. `reboot` is honoured only while a change is
  pending, one at a time, and at most once every 10 minutes.
- Boot recovery (`recovery.js`, root) writes back only our backup of the
  original, verified against the SHA-256 recorded when it was taken, over
  the backup's own build, and only for a binary its patch record shows as
  ours or cannot vouch for. Its one action on a service is `systemctl start` of carplay or
  aapd after a crash or a SIGKILL `patchd` did not send, bounded, where
  recmgr loaded the service's entry this boot; it never stops,
  kills or restarts anything. Its shutdown record is a drop-in for
  `var-opt-ro.service`, removed with the tool.
- No debug arming: the services are normal units whitelisted in recmgr.
- The journal and logs services run as dynamic users (`DynamicUser`, a
  read-only file system but their folder) in `systemd-journal`, the group
  that reads the journal and owns `journal/` and `saved/` (mode 2770). Root
  runs only in node, which the crash handler's list names (the journal
  folder's creation, a `+` command, which systemd gives no limits), and in
  the firewall unit. None of the units is ordered against a head-unit unit,
  and no head-unit unit or boot target waits for the services: a timer
  starts the journal service, a socket the logs service, the logs service
  the firewall unit; `timers.target` and `sockets.target` wait only for the
  timer's and the socket's own start. They restart after a failure (10 s
  apart, at most 10 starts in 10 minutes). The journal and logs services run
  at nice 19 with idle I/O and 64 CPU shares, within 20% and 50% of a CPU
  and 96 and 160 MiB; the firewall unit at nice 10, within 50% of a CPU and
  64 MiB. All three dump no core for the crash handler (`LimitCORE=1`, which
  their children such as `journalctl` and `nft` inherit).
- Nothing is mapped on the cluster before the stream's first non-blank
  picture, once its picture has stayed blank for `blank_ms`, or after the
  frames stop. The map never alters the gauges' data.
- The render runs as `hmi_5000` with the groups `video` (the DRM render
  node), `mgu_ufipc` (the AVB bridge's shared memory; the receiver fails
  without it) and `tm_oap_app` (the renderer's socket).

## Diagnostic capture

CarPlay's projection-config markers are on DLT only during a phone
connection. Press **Start capture** on the app's Logs page, then reconnect
the phone; the window lasts up to 5 minutes and **Stop capture** ends it
early. `patchd` keeps the markers until it restarts. The capture service is
a root service, so it starts only after a reboot following its install.

`waze-on-cluster-capture.service` sends BMW's DLT control request (service
`0xf13`, as `dlt-control -u -z 1`) over `/run/dlt-files/dlt-ctrl.sock`, waits
for the acknowledgement and checks the TCP 3490 listener through procfs. A
default log level below INFO (the log-and-trace manager sets OFF whenever it
turns tracing off) is set to INFO for the window (service `0x11`, as
`dlt-control -u -d 4`); otherwise most applications send nothing. It never
edits DLT configuration or restarts CarPlay. It refuses unless a DLT client is
registered, the listener is closed and the newest mode record in
`/run/logntrace/` says unambiguously whether tracing was OFF or BUFFER: the
daemon's (`<lifecycle>_dltd.idlt`) or the log-and-trace manager's
(`<lifecycle>_LTM.idlt`, after a resume often the only one), which must agree
with the daemon's newest. It records ownership under `/run/woc-dlt-capture`
before changing anything, and restores the recorded mode and default log level
at the deadline, on stop and in `ExecStopPost` (also after SIGKILL). A failed
restore shows as `restore_failed` in the app, is retried every 5 s and blocks
a new capture. DLT is shared ECU-wide: restoring closes its TCP clients, and no
other tracing tool should run during a capture.

## Verified, and open

Verified on a car with real CarPlay Waze: the alternate display, the AVB
capture (RTP payload type 96, single NAL units and FU-A), the decode, the map
in slot `0x100` under the gauges with normal colours, readable text and
correct transitions. A 10-packet slice of a car capture is the depacketiser's
golden test (`test/data/stream15_start.rtp`).

Open: how the unit's hardware MSDK conceals a lost slice or picture, and
whether it flags those frames `Corrupted` (and propagates the flag to the
frames that reference them); that the phone's SPS is exactly the advertised 1088x480; how packed
motion reads during a long fast pan while driving; whether the alternate
display survives iOS and Waze updates (flag 1006 is server-side). Waze sends
no guidance metadata, so this shows map pixels, not data-driven directions.

## Building and testing

```sh
make test                         # host tests: NAL, RTP, keyframe wait, the turn-box setting, the map
                                  # feed against cluster-render, the renderer's kit, patch/patchd/recovery/DLT,
                                  # the journal and logs services
make test-x86                     # the C++ tests with the unit's toolchain in Docker (SSE2 paths)
make target                       # build/target/waze-on-cluster in Docker, imports checked
make test-firmware ROOTFS=<dir>   # DLT capture against the unit's own dlt-daemon in Docker
```

`CR` names cluster-render's tree, `../cluster-render` by default. Its tree
also gives the log, the config, the unit's ABI tools and the build image.
The target builds in Ubuntu 18.04 (gcc 7.5, glibc 2.27, the unit's libstdc++ ABI) against
link-only stubs of the unit's libmfx, libva, libva-drm and IAS AVB bridge
(`abi/*.symbols`), with every import checked.

The patch tests run against a copy of the unit's `/usr/bin/carplay`
(carplay 22.28.3, sha256
`ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50`) named by
`CARPLAY_BIN`; without it they are skipped (`SKIP`). The locator's test runs
on synthetic builds without it. `aapd_test.js` runs against copies of the
unit's `/usr/bin/aapd` (aapd 22.39.3) and `/etc/aapd.conf` named by
`AAPD_BIN` and `AAPD_CONF`, and `aapd_locate_test.js` against `AAPD_BIN`
and the other aapd builds in `AAPD_BINS` (paths separated by `:`); both
are skipped without them. The repository
carries no BMW binary. `test-firmware` runs the unit's own dlt-daemon, libdlt and
dlt-receive from `ROOTFS`, an extracted root file system of the unit.

## License

GPL-3.0-or-later (`LICENSE`). `abi/include/mfx/` is a subset of the Intel
Media SDK headers under the MIT license (`abi/include/mfx/LICENSE`).
Third-party files keep their own licenses.
