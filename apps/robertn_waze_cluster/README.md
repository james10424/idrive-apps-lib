# Waze on cluster

Shows your phone's navigation map on the instrument cluster of a BMW with
iDrive 7. It can also show it on the head-up display (experimental).

- **CarPlay:** Waze, Apple Maps or Google Maps.
- **Android Auto:** Google Maps.

Whether your car and its software are supported can differ for CarPlay and
Android Auto. The app's **Status** for each tells you.

![The map on the cluster](../../docs/hud/cluster-map.jpg)

## Install

1. In the [mgu Store](https://github.com/robertnisipeanu/mgu-store), add the
   source `robertnisipeanu/idrive-apps`.
2. Install **Waze on cluster**. The Store installs everything it needs with
   it.
3. Restart the head unit when the Store asks.
4. In the car's settings, set **Instrument cluster > Central display area**
   to **Map view**.
5. Connect your phone and start navigation.

## What happens automatically

- Installing the app turns the map on for CarPlay and for Android Auto,
  each where your car supports it. You don't need to press anything.
- If a **Status** isn't `Patched` after the restart, see
  [If the map doesn't show](#if-the-map-doesnt-show).
- Updates keep it on.
- The map shows whenever your phone sends it. The app doesn't need to be
  open.

### Do I need to press Patch or Remove patch?

No. Leave them alone unless something is wrong.

- Press **Patch** when that phone type's **Status** says `Not patched`,
  `Update needed` or `Removed — head unit kept restarting`. For any other
  Status, see [If the map doesn't show](#if-the-map-doesnt-show).
- **Remove patch** turns the map off for that phone type. Use it only if
  you want the map off, or to troubleshoot. It stays off, also after
  updates, until you press **Patch**.
- CarPlay and Android Auto are separate. If you only use CarPlay, leave
  Android Auto's buttons alone, and the other way round. Neither one
  affects the other.

![CarPlay's section: Status: Patched](docs/main-carplay.png)

## Settings

The app is in the head unit's display language.

**General**

- **Show on:** where the map shows: **Cluster** (the default), **HUD
  (experimental)**, **Cluster and HUD (experimental)** or **Off**. The
  change is immediate. **Off** shows BMW's own content on both.
- **Own settings for this driver:** ticked, the current driver profile
  keeps its own Show on, HUD size and HUD position.
- **Hide BMW's turn box:** ticked (the default), BMW's turn-by-turn box on
  the cluster is hidden while the map shows.
- **Logs:** opens the Logs page (see below).

**HUD**

- **HUD map:** what the HUD map is doing.
- **Keep HUD map over BMW's pop-ups:** unticked (the default), the map steps
  aside while BMW's media and list pop-ups show on the HUD. Ticked, it
  stays.
- **Size:** how wide the map is on the HUD.
- **Move left / Move right / Move up / Move down** and **Centre:** where the
  map sits on the HUD.

**CarPlay** and **Android Auto**

- **Status:** whether the map is on for that phone type.
- **Patch** and **Remove patch:** see
  [above](#do-i-need-to-press-patch-or-remove-patch).
- **CarPlay Wi-Fi** (CarPlay only): the Wi-Fi band your iPhone uses.
- **Zoom out / Zoom in** (Android Auto only): out shows more map, in shows
  bigger text.
- **Hide phone's turn card** (Android Auto only): hides Google Maps' own
  turn card at the top of the map.

**Restart head unit** shows when a change needs a restart. Press it to
apply the change.

## If the map doesn't show

| If you see | Do this |
|---|---|
| `Status: Not patched` | Press **Patch**. |
| `Status: Update needed` | Press **Patch**. |
| `Status: Patched after restart` or `Removed after restart` | Restart the head unit. If **Restart head unit** shows on the app's page, press it. |
| `Status: Removed — head unit kept restarting` | The patch was removed because the head unit kept restarting. Press **Patch** to try again. If it happens again, [report it](#logs-and-reporting-a-problem). |
| `Status: Untested version` | This software version of your car hasn't been tested. You can press **Patch** to try it. If the head unit keeps restarting, the patch is removed again by itself. |
| `Status: Not supported on this car` | Your car doesn't support the map for this phone type. |
| `Status: Unavailable` | If it stays once the head unit has fully started, [report it](#logs-and-reporting-a-problem). |
| `Status: Failed`, `Partly patched` or `Changed by another tool` | [Report it](#logs-and-reporting-a-problem). |
| `Cluster map: Off` | Check **Show on**: **Off** and **HUD (experimental)** keep the map off the cluster. Check that CarPlay's or Android Auto's Status is `Patched`. |
| `Cluster map: Waiting for the phone` | Connect your phone. |
| `Cluster map: Waiting for the phone's map` | Start navigation on your phone. |
| `Cluster map: Frozen` or `Stream lost` | Reconnect your phone: unplug it, or turn its Wi-Fi off and on. This can happen after you change the cluster's Central display area. |
| The map never shows on the cluster | Set **Instrument cluster > Central display area** to **Map view**. |
| `Cluster map: Error` or `Unavailable` | [Report it](#logs-and-reporting-a-problem). |
| `HUD map: Disabled after errors` | The HUD map was turned off because the instrument cluster restarted right after it showed. It stays off until the next update of Cluster renderer. The cluster map keeps working. |
| CarPlay audio cuts out and `CarPlay Wi-Fi: 2.4 GHz` | On your iPhone, turn Wi-Fi off and on. |

## Logs and reporting a problem

1. Open **Logs** on the app's page.
2. Press **Save logs**.
3. Turn on the car's Wi-Fi hotspot and connect your phone to it.
4. Scan the QR code on the Logs page, or open the **Address** it shows, and
   download the newest file.
5. Open an issue at
   [github.com/robertnisipeanu/idrive-apps/issues](https://github.com/robertnisipeanu/idrive-apps/issues):
   say what you saw, what you expected, your car model, and attach the file.

If you are asked for a capture: press **Start capture**, reconnect your
phone, wait until the problem shows, then press **Save logs**. The capture
stops by itself after 5 minutes.

![The Logs page](docs/logs-page.png)

## Remove

In the Store, remove the **Waze on cluster** app, then the **Waze on
cluster** tool. The original CarPlay and Android Auto come back at the next
head-unit restart.

## For developers

How the app works, its page and sockets, and how to build and test it are
below. The map service and the patches are in
[waze-on-cluster](../../native/waze-on-cluster).

### The cluster's Central display area

Set **Instrument cluster > Central display area** to **Map view**: the Waze
map replaces the stock map while CarPlay's cluster stream shows a map, and
the stock map is back when the stream ends or shows none. Until a navigation
app draws on the cluster (in Waze or Google Maps, once a route starts), the
stock map stays and the page shows `Cluster map: Waiting for the phone's map`.
**Off** works too, with an empty centre instead of the stock map. With
Route preview or Assisted Driving View the cluster's own content takes the
centre. After changing the setting, CarPlay may need reconnecting before
the map returns: the change can freeze the phone's cluster stream, and the
page then shows `Cluster map: Frozen` while the stock map (in Off, an
empty centre) shows.

### Languages

The app is in the head unit's display language, every one the iDrive 7 HMI
offers: English (UK, US), German, Dutch (also Flemish), Danish, Swedish,
Norwegian, Finnish, Polish, Czech, Slovak, Slovenian, Hungarian, Romanian,
French (France, Canada), Italian, Turkish, Spanish (Spain, Latin America),
Portuguese (Portugal, Brazil), Greek, Russian, Ukrainian, Indonesian,
Vietnamese, Japanese, Korean, Simplified Chinese (also Cantonese
simplified), Traditional Chinese (also Cantonese traditional), Arabic,
Hebrew and Thai. A word a language has no translation for is English, as
is every language the HMI reports that the app does not know. A change of
the display language shows at once. Values from the tools (reasons,
addresses, numbers, file names, the cluster map's frame rate) are shown as
they are; CarPlay and Android Auto are names.

A language is one file, `locales/<code>.json`: the HMI's languages it is
for (`CommonTypes.Language`), its RHMI text files (named as the oap-sdk
names those languages) and its words, each keyed by the English (`{0}`,
`{1}` stand for the values):

```json
{ "hmiLanguages": ["DEUTSCH"], "rhmiTexts": ["de_DE"],
  "words": { "Status: {0}": "Status: {0}", "Save logs": "Logs speichern" } }
```

`node tools/build.js` writes them into the app (`src/locales.js`) and into
the RHMI text files, which hold the tile's name and the Logs page's title.
`node tools/test_i18n.js` checks that every language has every word with
the same `{n}`, and every HMI language its text file.

### Where the map shows

**Show on** in General chooses: **Cluster** (the default), **HUD
(experimental)**, **Cluster and HUD (experimental)** or **Off**. Off
takes the map off
the cluster and the HUD at once, without a reconnect or a restart, and both
show BMW's own content again, for a phone that keeps sending its map after
the route ends. Choosing a display again shows the map with the phone's
next frame. The choice is kept for the next start; Remove patch turns the
feature off altogether.
With both, the HUD gets up to half the upload bandwidth, only what it uses;
alone, all of it. On the HUD the frames are scaled on the head unit to the
size shown, the corners are rounded and a night map's background is made
black, which the HUD shows as nothing, so the roads, route and labels float
in view. The map steps aside while the HUD's media and list pop-ups show, never
for route guidance; **Keep HUD map over BMW's pop-ups** keeps it shown.
With the map on the HUD only, the cluster shows BMW's own content. If the
instrument cluster restarts right after the HUD map shows, twice, the HUD
map is disabled; the cluster's map keeps working.

### Driver profiles

Show on, the HUD map's size and its position are shared by every driver
profile. **Own settings for this driver**, under Show on, gives the driver
profile in use its own copy of them, which it then changes on its own; the
head unit switches to them whenever it switches to that profile. Unticked,
the profile goes back to the shared settings. The app reads the profile in use from the head unit
(`SessionManager.currentUserAccountReference`) every 5 s and goes by a
profile once two reads agree; a read with no profile changes nothing. Until
then the checkbox is greyed; on a head unit without that field the shared
settings apply.

### The page

One `LT_State_Wide` list in four sections, each under a header row
(`LT_Label_Header_1Row`, which the controller passes over): General, HUD,
CarPlay, Android Auto; then Restart head unit and the Restart row. CarPlay and Android Auto have the same rows with
the same words; no row repeats the header's name. Every info row reads
`Name: Value`, the value starting with a capital (`Zoom: —` while there
is none), every on/off setting is a checkbox, the choice of where the map
shows a drop-down (`LT_DropDownMenu_1Row_2Col3070`, as the stock apps'
settings) and the HUD map's size a slider (`LT_Slider_LeftJustified_Fill_Steps`,
as the stock seat heating's level). Every row
stays on the page: one that cannot act now is greyed, and while `patchd`
runs any operation (`busy`), every operation in both sections is. A greyed
row pressed sends nothing; a greyed checkbox keeps its label. Four rows
are shown and hidden: CarPlay Wi-Fi and the line under it, CarPlay's last,
and the restart's two, the page's last, so no row above them moves.
`(after restart)` marks a value that waits for the next head-unit start.

Both sections' Status row uses the same words: `Patched`, `Not patched`,
`Patching…`, `Removing patch…`, `Patched after restart`, `Removed after
restart`, `Failed`, `Not supported on this car`, `Untested version`,
`Changed by another tool`, `Partly patched`, `Connecting…`,
`Unavailable`; CarPlay also `Update needed` (a patch from an older app,
which Patch brings up to date), and `Removed — head unit kept
restarting` once boot recovery removed a patch, until Patch applies it
again. On a head unit
whose variant the tool cannot read, both read `Not supported on this car`,
also while a patch waits for the next start to be removed. Every variant is
supported, HIGH F included.

| Section | Row | What it does |
|---|---|---|
| **General** | Show on | a drop-down: `Cluster` (the default), `HUD (experimental)`, `Cluster and HUD (experimental)` or `Off`; with the HUD alone or Off the cluster shows BMW's own content, with Off the HUD too. A choice takes effect at once and is kept for the next start. Greyed until the map service reports it |
| | Own settings for this driver | a checkbox: ticked, the driver profile in use has its own Show on, HUD size and position, a copy of the settings when ticked; unticked (the default), it uses the shared ones. Greyed while no driver profile is detected and until the map service answers |
| | Cluster map | `Cluster map: <state>`, the map the shared render draws for either phone: `Off` unless CarPlay's or Android Auto's patch is in effect (on disk with nothing pending, as the last status outside a change said; a running or failed change keeps it), `Starting…`, `Showing Waze map — 16 frames/s`, `Waiting for the phone`, `Waiting for the phone's map`, `Stream lost`, `Frozen`, `Error`, `Unavailable` (the map service is down, or refuses the head unit) |
| | Hide BMW's turn box | a checkbox: ticked, BMW's turn-by-turn box on the cluster stays hidden while the map shows (the default); unticked, it shows. A press changes it at once and the map service keeps it. Greyed and unticked until the map service reports it and while the cluster renderer keeps the box shown; greyed while the map is not on the cluster |
| | Logs | opens the Logs page |
| **HUD** | HUD map | `HUD map: <state>`, the map on the HUD: `Off`, `Starting…`, `Showing`, `Waiting for the phone`, `Frozen`, `Error`, `Disabled after errors` (the cluster renderer turned it off after the cluster restarted right after it showed, twice), `Unavailable` (the map service is down) |
| | Keep HUD map over BMW's pop-ups | a checkbox: unticked (the default), the HUD map steps aside while BMW's HUD media and list pop-ups show (the media source list) and comes back after they go; ticked, it stays shown over them. Never steps aside for route guidance. Kept for the next start; greyed as the Size slider |
| | Size | a slider, `Size: Standard` (the fitted box, about 57 % of the HUD's width, at the 55 % step) or `Size: 70 %`: 5 % a step, from 30 % to 85 % of the HUD's width, the picture's shape kept; kept for the next start; greyed while the HUD is off or disabled |
| | Position | `Position: Centre`, or how far the HUD map is moved: `Position: 20 px right, 10 px up` |
| | Move left / Move right / Move up / Move down | 10 px a press, held inside the HUD; kept for the next start; greyed as the Size slider |
| | Centre | puts it back in the middle; enabled while it is moved |
| **CarPlay** | Status | `Status: <state>`, in the words above |
| | Patch | applies the patch; enabled while it is off, unknown or out of date, the head unit is supported and no change is pending |
| | Remove patch | reverts the patch, which turns the map off; enabled while something of the patch is on disk, also while it waits for a restart (`Patched after restart`), and no change runs; a patch waiting over the original is removed at once (`Not patched`), one waiting over a running patch leaves a removal that waits (`Removed after restart`) |
| | CarPlay Wi-Fi | `CarPlay Wi-Fi: 5 GHz` or `CarPlay Wi-Fi: 2.4 GHz`, the band of the head unit's Wi-Fi the phone joined; shown while CarPlay runs over Wi-Fi and the band is known (waze-on-cluster README, "CarPlay's Wi-Fi band") |
| | (the line under it) | `Audio may cut out. On your iPhone, turn Wi-Fi off and on.`; shown on 2.4 GHz only, where CarPlay's streams and the cluster's can be more than the link carries |
| **Android Auto** | Status | `Status: <state>`, in the words above; `Failed` also while the last zoom or turn-card change failed |
| | Patch | applies its patch, also to an `Untested version`; enabled while it is off or partly patched, supported and nothing is pending |
| | Remove patch | reverts it; enabled as CarPlay's |
| | Zoom | `Zoom: 180 dpi`, the density on disk, with `(after restart)` while it differs from the one aapd runs until the next head-unit start; `Zoom: —` without the patch |
| | Zoom out / Zoom in | 10 dpi a press (150-400): out shows more map; enabled while patched, nothing is pending, no conflict stands but `Untested version`, and the 10 s between changes has passed |
| | Hide phone's turn card | a checkbox, ticked while the turn card on disk is hidden, or as asked while its change runs: the phone's own turn card cropped off the map; `(after restart)` as for the zoom; enabled as Zoom out and Zoom in |
| (the page's end) | Restart head unit | the Node State Manager's normal head-unit restart; shown while a written change, CarPlay's or Android Auto's, waits for the next head-unit start; enabled while nothing runs and the restart limit (one every 10 minutes) allows it |
| | Restart | shown while it has something to say: `Restart: In progress`, `Restart: Failed, not accepted` (NSM did not take it), `Restart: Failed, the head unit stayed on`, `Restart: Failed, command failed`, each with `— possible in 8 min` while the limit holds, `Restart: Possible in 8 min` |

| | |
|---|---|
| The top of the page: General | ![](docs/main-general.png) |
| Show on: Cluster (the default) | ![](docs/main-show-on-cluster.png) |
| Own settings for this driver, ticked | ![](docs/main-profile-own.png) |
| Show on, open | ![](docs/main-show-on-open.png) |
| Show on: HUD (experimental) | ![](docs/main-show-on-hud.png) |
| Show on: Cluster and HUD (experimental) | ![](docs/main-show-on-both.png) |
| The HUD map's size, the slider turned | ![](docs/main-hud-size.png) |
| Keep HUD map over BMW's pop-ups: unticked (the default) and ticked | ![](docs/main-hud-keep-off.png) ![](docs/main-hud-keep-on.png) |
| HUD disabled after errors | ![](docs/main-hud-disabled.png) |
| CarPlay | ![](docs/main-carplay.png) |
| Android Auto | ![](docs/main-aa-on.png) |
| Android Auto alone patched, the cluster map showing | ![](docs/main-aa-only.png) ![](docs/main-aa-only-map.png) |
| Android Auto not supported | ![](docs/main-aa-unsupported.png) |
| A change waiting for a restart | ![](docs/main-restart-pending-aa.png) ![](docs/main-restart-pending.png) |
| The Logs page | ![](docs/logs-page.png) |

The **Logs** page (`ST_OnlineApps_WazeClusterLogs`, a child of the main
page):

| Row | What it does |
|---|---|
| Start capture / Stop capture | a bounded DLT capture (5 minutes) for diagnostics; start it, then reconnect CarPlay |
| Capture | `Capture: On`, `Off`, `Starting…`, `Stopping…`, `Failed`, `Unavailable` |
| Save logs | saves an archive of the tools' logs, kept journal and status (waze-on-cluster README, "Logs") |
| Saved logs | `Saved logs: None`, the number kept, `Saving…`, `Not enough space`, `Too large to save`, `Save failed`, `Unavailable`, `Connecting…` |
| QR code | the download address, for a phone on the car's Wi-Fi hotspot; blank without one |
| Address | `Address: http://172.16.222.97:8475/<token>/`, `Address: Wi-Fi hotspot off`, or `Address: —` before the logs service answers |
| 5 rows | the saved archives, newest first, with their size |

The app holds the logs socket while the Logs page is visible; the logs
service serves the archives while it is held and for 15 minutes after a
save. The QR code is a PNG in `/dev/shm/robertn_waze_cluster_qr_<uid>/`
(`src/qr.js`, with Project Nayuki's QR code generator, `src/qrcodegen.js`),
replaced when the address changes and removed when the app shuts down.

`Cluster map: Starting…` shows while the cluster renderer starts; an error shows
only when the renderer stays unreachable for 15 s. Before the patch daemon
runs (right after the install), the pending patches are read from
`/dev/shm/waze-on-cluster/activation-pending` (CarPlay) and
`/dev/shm/waze-on-cluster/aa-pending` (Android Auto); a marker with
another boot's id is done, one without a boot id counts.

### The sockets

Unix stream sockets, newline-delimited JSON, group `tm_oap_app`. The app
holds the first two while one of its pages is visible and the third while
the Logs page is, and retries each once a second; the tools keep running
when it closes them.

- **Render** `/dev/shm/waze-on-cluster/ctl.sock`. The app sends
  `{"cmd":"guide","hide":0|1}` (Hide BMW's turn box). The tool sends
  `{"ev":"status","state","target","page","reason","upload_cap_kb_per_s","upload_reason","hide_bmw_guidance","guide_disabled"}`
  (`state` idle, starting, showing, frozen or error; `hide_bmw_guidance` 0
  or 1; `guide_disabled` 1 while the cluster renderer keeps the box shown),
  and `{"ev":"log","line"}`, which the app does not show.
- **Patch daemon** `/dev/shm/waze-on-cluster/patch.sock` (root). The app sends
  `{"cmd":"apply","panel":"high","mode":"avb"}`, `{"cmd":"revert"}`,
  `{"cmd":"aa-apply"}`, `{"cmd":"aa-revert"}`, `{"cmd":"aa-density","value"}`, `{"cmd":"aa-card","value"}`,
  `{"cmd":"reboot"}`, `{"cmd":"capture-start"|"capture-stop"}`. The daemon sends
  `{"ev":"status","state","panel","mode","res","safe","carplay","reason","pending","conflict","busy","restart","aa","wifi"}`
  (`aa`: `{"state","reason","conflict","pending","aapd","dpi","diskCard","runDpi","runCard","density","card","densities","changeAt","error"}`;
  `restart`: `{"state","code","reason","possibleAt"}`; `wifi`: `{"band","iface","freq"}`, `band` `"5"` or
  `"2.4"`, while CarPlay runs over Wi-Fi and its band is known, else `null`)
  (`state` original, patched, unknown, busy or error; `carplay: "deferred"`
  while a change waits for the next start), `{"ev":"capture",…}`,
  `{"ev":"reboot-reply","ok","reason"}`, `{"ev":"refused","cmd","reason"}`
  (a command not taken; it changes nothing on the page) and
  `{"ev":"log","line"}`, which the app does not show. waze-on-cluster's
  PATCH.md, "Activation", has the fields.
- **Logs** `/dev/shm/waze-on-cluster/logs.sock`; a connection starts the
  logs service. The app sends `{"cmd":"save"}`. The service sends
  `{"ev":"state","url","saving","error","saved","archives":[{"name","size","mtime"}]}`
  on connect and on each change (`url` null without the hotspot's address;
  `error` `no-space`, `too-large` or `failed`).

A patch with another sink, panel, size (not `1088x480`) or safe area (not
`504x368+286+72`) shows `Status: Update needed`.

### Build and test

```sh
node tools/test_control.js           # the socket client against fake sockets
node tools/test_page.js              # the page against a fake SDK and HMI
node tools/test_profiles.js          # the driver profiles against fake sockets and SessionManager
node tools/test_i18n.js              # every language: its words, placeholders, RHMI text files
node tools/build.js                  # robertn_waze_cluster@<version>.src.tgz
node tools/build.js --sdk <donor>    # also robertn_waze_cluster@<version>.tgz
```

The app ships as a source package (`mgu-app.json`; `"build": "unit"` in the
catalog). The repository carries no oap-sdk, which is BMW's: the head unit's
deploy tool links the app against the unit's own oap-sdk and takes the
package's entry point from the unit's own packages. To build the OAP tgz
locally, pass a donor bundle with `--sdk` (or `MGU_SDK_DONOR`), for example
the one `mgu-deploy.sh sdk-donor` takes from the unit. The tests need no
donor.

`node tools/fakes.js <render.sock> <patch.sock>` runs both fakes by hand;
Enter starts or stops a fake CarPlay stream.

| Path | |
|---|---|
| `src/robertn_waze_cluster.js` | the app |
| `src/i18n.js`, `src/locales.js` | the words in the display language; the languages, written from `locales/` |
| `locales/<code>.json` | a language's words (Languages) |
| `src/waze_cluster_control.js` | the render and patch socket clients and the status logic |
| `src/logs_control.js` | the logs socket client |
| `src/profile_sync.js` | the driver profile in use and its own map settings |
| `src/qr.js`, `src/qrcodegen.js` | the QR code PNG; Project Nayuki's generator (MIT, `LICENSE-qrcodegen.txt`) |
| `tools/` | build, fakes, tests, icon |
| `package/` | the package tree (manifest, RHMI UI, config, CHANGELOG) |

### License

GPL-3.0-or-later (`LICENSE`). Third-party files keep their own licenses.
