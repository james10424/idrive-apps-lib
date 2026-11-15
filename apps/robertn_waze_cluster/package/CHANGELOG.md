# Changelog

## Unreleased

## 0.6.8

A plainer description in the Store: on supported cars the map turns on by
itself after you install the app and restart the head unit.

## 0.6.7

Keep HUD map over BMW's pop-ups, a checkbox in HUD. Unticked, the default,
the HUD map steps aside while BMW's HUD media and list pop-ups show, such
as the media source list, and comes back after they go; it
never steps aside for route guidance. Ticked, it stays shown over them.
Kept for the next start, and per driver profile with Own settings for this
driver.

## 0.6.6

Show on gets **Off**: the map leaves the cluster
and the HUD at once and BMW's own content comes back, with no CarPlay or
Android Auto reconnect and no restart, for a phone that keeps sending its
map after the route ends. Cluster, HUD or both again shows the map with
the phone's next frame. Kept for the next start, and per driver profile
with Own settings for this driver. While Off, the HUD's size and position
and Hide BMW's turn box are greyed.

## 0.6.5

The Store shows the app's icon while it is not installed.

## 0.6.4

Map settings per driver profile, after eroobeeroo's #41. Show on, the HUD
map's size and its position stay shared by every driver profile; Own
settings for this driver, a checkbox under Show on, gives the profile in
use its own copy, which the head unit switches to with the profile.
Unticked, the profile uses the shared settings again. Greyed while no
driver profile is detected.

## 0.6.3

The same app, built by the release that sets its version. Nothing changes
on the car.

## 0.6.2

The app's tile keeps its name in the Apps menu after the app manager stops
the app: the tile shows the name and icon the app manager gives it.

## 0.6.1

Android Auto's Patch is offered on an `Untested version` too, and patches
it; an install leaves such a version as it is. While Android Auto reports
any other conflict, Zoom out, Zoom in and Hide phone's turn card are greyed
and its Status names the conflict (`Changed by another tool`, `Not
supported on this car`, `Unavailable`), also while it is patched.

Remove patch, for CarPlay and Android Auto, is offered whenever something
of the patch is on disk, also while it waits for a restart (`Patched after
restart`). A patch waiting over the original is removed at once and the
Status reads `Not patched`; one waiting over a running patch leaves a
removal that waits too (`Removed after restart`). Requires
waze-on-cluster 0.9.1, which finds Android Auto's edit sites in any aapd
build, undoes the proven builds without their backups, and refuses a
removal of the tool while an edit of its own is left.

## 0.6.0

The map on the HUD, contributed by eroobeeroo and wesleyredhat. Show on, a
drop-down in General, chooses where the map shows: Cluster (the default),
HUD (experimental) or Cluster and HUD (experimental); with both the HUD uses
up to half the upload bandwidth, only what it needs. With the map not on
the cluster, `Cluster map: Off` and the Hide BMW's turn box checkbox is
greyed. A HUD section: `HUD map: …`, the Size slider (5 %
a step from 30 % to 85 %) and `Position: …` with Move left, Move right, Move
up, Move down and Centre, greyed while the HUD is off, and while the
cluster renderer keeps it off after errors (`HUD map: Disabled after
errors`). The settings take effect at once and are kept for the next
start, in a file of their own that waze-on-cluster 0.8 does not read, so
going back to it keeps working. These rows are in the head unit's display language, as is the rest
of the page. Requires waze-on-cluster 0.9.0.

The page's sections: General first, then HUD, CarPlay, Android Auto.
Restart head unit and its Restart row are the page's last rows, so nothing
above them moves when they show.

CarPlay's section shows `CarPlay Wi-Fi: 5 GHz` or `CarPlay Wi-Fi: 2.4 GHz`
while CarPlay runs over Wi-Fi and waze-on-cluster knows the band. On 2.4
GHz a line under it says `Audio may cut out. On your iPhone, turn Wi-Fi off
and on.` Saved logs include CarPlay's Wi-Fi band, the Wi-Fi interfaces'
counters and the kernel's Wi-Fi connection lines.

## 0.5.7

The map's capture starts at boot after the head unit's media bus is up,
ordered after BMW's own media services and never holding any of them up.
Requires waze-on-cluster 0.8.6.

## 0.5.6

When boot recovery removed a patch because the head unit kept restarting
with it, its Status row reads `Removed — head unit kept restarting`, in the
display language, also with part of the patch left, until its Patch button
applies the patch again. On a head unit that refuses the tool, `Not
supported on this car` comes first. Requires waze-on-cluster 0.8.5.

## 0.5.5

On a HIGH F car where waze-on-cluster 0.8.2 removed the patch at boot, the
update puts it back by itself: the app shows `Status: Patched after
restart`, and the map returns at the next head-unit start or drive.
Requires waze-on-cluster 0.8.4.

## 0.5.4

HIGH F head units are supported again, as every variant is; only a head
unit whose variant cannot be read shows `Not supported on this car`.
Requires waze-on-cluster 0.8.3.

## 0.5.3

Every word on both pages, and the tile's name, in the head unit's display
language: each language the iDrive 7 HMI offers, English where a language
has no translation. A change of the display language shows at once. Values
from the tools (reasons, addresses, numbers) are shown as they are.

## 0.5.2

On a head unit that refuses the tool (HIGH F, or a variant it cannot
read), CarPlay and Android Auto both read `Status: Not supported on this
car`, also while a patch from before is on disk or an apply of it waits
for the next start, which boot recovery turns into a restore; Restart head
unit stays offered while one is needed. Neither Patch button, Zoom out,
Zoom in nor the turn card checkbox is offered there; Remove patch stays.
`Cluster map: Unavailable` while the tool reports itself unsupported, and
the Hide BMW's turn box row is greyed. Requires waze-on-cluster 0.8.2.

## 0.5.1

The main page in three sections under header rows. CarPlay and Android Auto
have the same rows with the same words: `Status: …`, Patch and Remove
patch; Android Auto adds Zoom (`Zoom: 170 dpi`), Zoom out, Zoom in and the
Hide phone's turn card checkbox. General: `Cluster map: …` (the map the
shared render draws for either phone), the Hide BMW's turn box checkbox,
Logs, and Restart head unit with its `Restart: …` row. Info rows read
`Name: Value`; on/off settings are `Hide …` checkboxes; `(after restart)`
marks a value that waits for the next head-unit start. Both Status rows
use one set of words (`Patched`, `Not patched`, `Patching…`, `Patched after
restart`, `Failed`, `Not supported on this car`, …). Rows stay in place and
are greyed when they cannot act; while any change runs, every change in
both sections is greyed. Restart head unit, the last row, shows only while
a change waits for a restart; its row says when it failed, why, and when
the 10-minute limit allows the next. The log lines and the setup line are
off the main page; Start capture is on the Logs page. Restart head unit
asks the Node State Manager for a normal restart. Requires
waze-on-cluster 0.8.1.

## 0.5.0

Android Auto next to CarPlay: a line with its state (patched and its
density, off, pending, not supported on this car, untested aapd build),
Enable Android Auto, Disable Android Auto, Zoom out and Zoom in for the
density, and Hide turn card / Show turn card. Restart head unit also
activates a pending Android Auto change. Requires waze-on-cluster 0.8.0.

## 0.4.4

Hide BMW turn box: a checkbox on the main page. Ticked (the default), BMW's
own CarPlay turn-by-turn box on the cluster stays hidden while the Waze map
shows; unticked, it shows. The choice takes effect at once and is kept
across restarts and updates. While the cluster renderer keeps the box shown
(its mapping guard), the checkbox is greyed and labelled unavailable.
Requires waze-on-cluster 0.7.13.

## 0.4.3

A Logs page: Save logs saves an archive of the tools' logs, the journal
lines they keep and their status, and a phone on the car's Wi-Fi hotspot
downloads the saved archives from the address the page shows, as text and as
a QR code. Requires waze-on-cluster 0.7.11.

## 0.4.1

While CarPlay's stream is live but shows no map, the status says why, from
waze-on-cluster 0.7.2 on: "Stream lost — reconnect CarPlay", or "Stream
lost — restarting CarPlay" where the tool restarts CarPlay itself.

## 0.4.0

The page says to set Instrument cluster > Central display area to Map view:
the Waze map replaces the stock map while CarPlay streams it, and the stock
map is back when the stream ends or freezes. A frozen stream shows "Map
frozen — reconnect CarPlay". Requires waze-on-cluster 0.6.0.

## 0.3.1

The page says to set Instrument cluster > Central display area to Off: with
Map view, Route preview or Assisted Driving View there, the cluster shows its
own content in the centre instead of the map.

## 0.3.0

Installing the app from the Store installs the Waze tool and the cluster
renderer too. The tool writes the CarPlay patch as it installs, without
restarting CarPlay; until the next head-unit start the app shows "Patch
pending — reboot to activate". Enable map appears only when the patch is
off, unknown or out of date, Disable map only when it is on. When the install
leaves CarPlay unpatched (an unknown CarPlay build, or another tool starting
CarPlay), the app says why and offers Enable map. The Store can remove the
app and then the Waze tool, which restores CarPlay.

## 0.2.11

When a patch change waits for the next head-unit start, the app says so and
offers a Restart head unit button. A boot recovery service restores the
original CarPlay binary after repeated failed starts with the patch.

## 0.2.10

The map appears on the cluster by itself when CarPlay streams it and goes
away when the stream stops; there is no Show or Stop button. Disable map
turns the feature off.

## 0.2.9

Enable map sets the area where the phone places its guidance, so Waze's
next-turn banner stays visible between the gauges. Units patched with a
full-frame area are prompted to update.

## 0.2.8

The map is drawn in the cluster's native map area (1088x480), under the
gauges. Units patched at another size are prompted to Enable map again.

## 0.2.7

Enable map applies a single patch configuration, including the size the
phone renders the map at. Units on another panel setting are prompted to
update.

## 0.2.6

Enable map uses a single capture path; the capture mode row is removed.
Capture logs has its own button.

## 0.2.5

The status line reads "Patch state unknown — enable the map" until the patch
daemon reports, so the page opens cleanly.

## 0.2.4

After Enable map, the cluster stream starts with the CarPlay session. A patch
waiting for the next head-unit start reads "Patch pending — …".

## 0.2.3

The diagnostic capture window is up to 5 minutes.

## 0.2.2

Capture mode offers a UDP stream sink.

## 0.2.1

Capture logs: a bounded DLT capture, with its status and an early stop.

## 0.2.0

A capture mode choice for Enable map.

## 0.1.0

First release: a page to apply and revert the CarPlay patch, show and stop
the map on the cluster, and read both tools' logs.
