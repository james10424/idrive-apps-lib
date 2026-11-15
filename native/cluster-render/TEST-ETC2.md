# ETC2 car tests

Whether the cluster draws `etc2rgba` assets (PROTOCOL.md): ETC2 RGBA with
EAC alpha, 1 byte a pixel, made into an immutable `Texture2D`
(`ETextureFormat_ETC2RGBA`) in each scene of its target.

Results:

| Test | Unit | Result |
|---|---|---|
| 1. 252x280 frame, beside the same frame as rgba8 | iDrive 7, G20 HUD (KOMBI), Nov 12 2026 | **Drawn**: both layers `shown`, the ETC2 frame matched the RGBA8 one, no KOMBI restart; `asset 27 etc2rgba 252x280: 1 writes, 68 KB, 0 failed` |
| 2. 2048x1024 atlas, one layer | iDrive 7, G20 HUD (KOMBI), Nov 13 2026 | **Drawn**: `--atlas` showed the whole atlas in the HUD window (both copies, the white frame, the red and green bars), `--pan` drifted over it sharp at 1:1; `asset 31: 2048x1024 etc2rgba texture made, 2097152 bytes`, no refusal, no KOMBI restart |
| 2b. 2048x1024 atlas on the instrument cluster (`panel`, 1440x720 in the middle; the client's targets switched to `panel`) | iDrive 7, G20 cluster (KOMBI), Nov 13 2026 | **Drawn**: whole and panned at 1:1; `asset 37: 2048x1024 etc2rgba texture made, 2097152 bytes`, no refusal, no restart |
| 3. 2048x2048 atlas, one layer (the close-up atlas in 2x2 with a coloured bar under each copy; 4,194,304 bytes, under the 5,529,600-byte message limit) | iDrive 7, G20 HUD and instrument cluster (KOMBI), Nov 13 2026 | **Drawn** on both: whole (scaled) and panned at 1:1 to the bottom copies and their bars; `asset N: 2048x2048 etc2rgba texture made, 4194304 bytes` on each, no refusal, no KOMBI restart |
| either | iDrive 8 | not run |

## Files

- Service: `make target`.
- Client: `tools/etc2_test/etc2_client.js` (Node 8, no deps) with
  `js/cluster_render_client.js`.
- Test images, from a Live2D close-up atlas (`close.rgba` and its
  `atlas.json` with a `close` section; not in the repository):

  ```sh
  python3 tools/etc2_test/etc2_encode.py frame <close.rgba> <atlas.json> <dir>   # test 1: frame.rgba, frame.etc2, frame.json
  python3 tools/etc2_test/etc2_encode.py atlas <close.rgba> <atlas.json> <dir>   # test 2: atlas.etc2, atlas.json
  ```

  Test 1: the close-up's middle pose (frame `zoomSteps + 1`), padded to
  4x4 blocks (249x278 to 252x280): 282,240 bytes RGBA, 70,560 ETC2.
  Test 2: the 1024 px atlas twice side by side, each copy padded to
  1024x1024, a bar along the bottom of each (red left, green right) and a
  white 4 px frame round the whole: 2048x1024, 2,097,152 bytes (RGBA8
  would be 8 MiB, past one frame message). The encoder's output decodes
  byte for byte the same on an Apple GPU (Metal `eac_rgba8`) as with its own
  decoder; `atlas-etc2-decoded.png` shows what the display should show.

## Steps (parked)

1. Build and install the kit (from the repository root; README.md
   "Install"):

   ```sh
   node release/tool_artifact.js native/cluster-render --out /tmp/kit
   <mgu-store>/deploy/hu install --artifact /tmp/kit/cluster-render@<version>.tgz
   ```

   On the unit, `grep -c etc2rgba /var/opt/cluster-render/current/cluster-render-idrive7`
   prints 1 or more when a build with `etc2rgba` is installed.

2. Copy the files (`deploy/hu connect --hold 900` prints the ssh line; `UNIT`
   is its host):

   ```sh
   ssh root@UNIT mkdir -p /tmp/etc2
   scp native/cluster-render/tools/etc2_test/etc2_client.js native/cluster-render/js/cluster_render_client.js \
       <dir>/frame.rgba <dir>/frame.etc2 <dir>/frame.json <dir>/atlas.etc2 <dir>/atlas.json root@UNIT:/tmp/etc2/
   ```

3. Turn off other HUD apps (so only the test draws), the HUD on. In one ssh
   session follow the log:

   ```sh
   tail -f /var/opt/cluster-render/log/cluster-render.log | grep -E 'etc2|asset|hud|went away|ramses'
   ```

   In another, run one test at a time, leave it up while you look, Ctrl+C
   ends it:

   ```sh
   /usr/bin/node /tmp/etc2/etc2_client.js /tmp/etc2                 # test 1: rgba8 left, etc2rgba right
   /usr/bin/node /tmp/etc2/etc2_client.js /tmp/etc2 --atlas         # test 2: the whole 2048x1024, scaled to 560x280
   /usr/bin/node /tmp/etc2/etc2_client.js /tmp/etc2 --atlas --pan   # test 2: a 560x280 window at 1:1, drifting over it
   ```

## What to look for

Test 1: two Mikus side by side, the right the same as the left (ETC2 a bit
blockier in colour).

Test 2, `--atlas`: the whole atlas twice, side by side, in the HUD's window:
the white frame on all four sides, the red bar under the left copy and the
green one under the right. `--pan`: the window drifts over every part of
it, sharp (1:1), reaching the green bar and the right and bottom white edges.
Log: `asset N: 2048x1024 etc2rgba texture made, 2097152 bytes`; client: the
layer `shown`.

| You see | Means |
|---|---|
| As above, the layers `shown` | **Drawn.** |
| Blank, the layer `error - asset N: the renderer refused ...` | RAMSES on the head unit refused the texture (the log has `ramses: ... refused by createTexture2D`) |
| Blank, the layer `shown`, no restart | The cluster took the scene and did not draw the texture: too big, or the format unsupported |
| The left half right, the right half or the far edges garbage or missing | The cluster draws only part of a 2048 px texture: keep atlases at 1024 |
| `REFUSED asset ...: format must be ...` | A build without `etc2rgba` is running: redo step 1 |
| HUD (and cluster) blank, KOMBI restarts | The cluster choked on it. The log has `target hud: the display service went away … after its mapping`. Stop the client |

Write down what the HUD showed, the client's output and the log lines.

## Afterwards

`rm -rf /tmp/etc2` on the unit and turn the HUD apps back on. To leave a development
build, install a release from the Store (`deploy/hu rollback cluster-render
--yes` works only while the deploy tool keeps another version).
