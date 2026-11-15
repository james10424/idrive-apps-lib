# idrive-apps

Apps for the iDrive 7 head unit of a BMW, installed from the
[mgu Store](https://github.com/robertnisipeanu/mgu-store).

## Waze on cluster

Shows your phone's navigation map on the instrument cluster: CarPlay (Waze,
Apple Maps or Google Maps) or Android Auto (Google Maps). It can also show it
on the head-up display (experimental).

![The map on the cluster](docs/hud/cluster-map.jpg)

**Install:** in the Store, add the source `robertnisipeanu/idrive-apps` and
install **Waze on cluster**. Restart the head unit when the Store asks, and
in the car's settings set **Instrument cluster > Central display area** to
**Map view**. On a
supported car the map turns on by itself; you don't need to press anything
in the app.

Settings, what to do if the map doesn't show, and how to send logs:
[Waze on cluster](apps/robertn_waze_cluster/README.md).

### On the head-up display

Choose **HUD (experimental)** or **Cluster and HUD (experimental)** in the
app's **Show on**. You can set the map's size and position on the HUD. At
night the map's background is black, so only the roads, route and labels
show. Photos from a 2021 X5 xDrive45e (G05); street names blurred.

![The HUD map while driving](docs/hud/hud-on-the-road.jpg)

*The HUD map while driving. (The panel at the bottom right, battery power
and temperatures, is a separate app.)*

| Waze's grey-blue night background on the HUD | The background made black, which the HUD shows as nothing |
|---|---|
| ![Before](docs/hud/hud-before-black-background.jpg) | ![After](docs/hud/hud-night.jpg) |

## For developers

| Path | |
|---|---|
| [`native/cluster-render`](native/cluster-render) | the cluster renderer: one service that draws every app's layers on the instrument cluster and the HUD, on iDrive 7 and iDrive 8 |
| [`native/waze-on-cluster`](native/waze-on-cluster) | the CarPlay map (Waze, Apple Maps or Google Maps), or Android Auto's, on the instrument cluster, drawn through cluster-render |
| [`apps/robertn_waze_cluster`](apps/robertn_waze_cluster) | the iDrive app for it: where the map shows, the patches' status and buttons, Logs |
| [`release`](release) | builds and publishes the Store catalog |
| `tools` | `oap_pack.js`, `oap_link.js`: pack the app |

Installing the app installs what it requires first: `mgu-deploy` from
mgu-store, then `cluster-render` and `waze-on-cluster` from here. The
`waze-on-cluster` install hook writes the CarPlay patch, and the Android Auto
patch on a car that supports it; one head-unit start activates them. The
Store removes them in the reverse order: the app, then the tool, then the
renderer.

### Build

```sh
make -C native/cluster-render test test-x86 target    # host tests; the unit's toolchain and binary (Docker)
make -C native/waze-on-cluster test test-x86 target
node apps/robertn_waze_cluster/tools/test_control.js
node apps/robertn_waze_cluster/tools/test_page.js
node apps/robertn_waze_cluster/tools/test_i18n.js
node release/test/release_test.js
node release/release.js check                         # reproducible artifacts
```

Each folder's README has the details; [`release/README.md`](release/README.md)
covers versions, the catalog and publishing. Before opening a pull request,
read [`CONTRIBUTING.md`](CONTRIBUTING.md): the release sets versions.

### License

GPL-3.0-or-later ([`LICENSE`](LICENSE)), unless a file's SPDX header says
otherwise. What apps compile in or copy from cluster-render, its `include/`,
`client/`, `kit/`, `js/cluster_render_client.js` and `PROTOCOL.md`, is
MPL-2.0 ([`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt)). Third-party files
keep their own licenses: the RAMSES and vsomeip interfaces in cluster-render
(MPL-2.0), the Intel Media SDK headers in waze-on-cluster (MIT), Project
Nayuki's QR code generator in robertn_waze_cluster (MIT).
