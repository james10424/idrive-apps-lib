<!-- SPDX-License-Identifier: MPL-2.0
     Copyright (C) 2026 Robert Nisipeanu -->

# cluster-render protocol, version 1

A client connects to the service's stream socket
(`/dev/shm/cluster-render/render.sock`, group `tm_oap_app`). Messages go
both ways, each:

```
u32 length (little-endian: the bytes after it) | u8 type | body
```

| Type | Body |
|---|---|
| `J` (0x4A) | a flat JSON object: string and number values (true/false count as 1/0) |
| `F` (0x46) | `u32 ref, u64 frame id, u16 rect count, rect count x {u16 x, y, w, h}, pixels` (little-endian) |
| `M` (0x4D) | a mesh part's geometry ([below](#mesh-geometry-m)) |

A message is at most 5,529,664 bytes (a whole 1920x720 RGBA image). The C++
client is `include/cluster_render/client.h`, the Node 8 client
`js/cluster_render_client.js`; both reconnect by themselves and make their
assets and layers again, with their pixels, uniforms, parts and geometry,
binds and animations.

## Client to service

Every JSON message has `op`. A client names its layers, scenes and assets
with its own `ref` (a positive number, unique within its connection across
all three); they go when the connection does, the layers before the assets. A field the
service does not know is ignored, so a client may send one an older service
lacks.

| op | Fields |
|---|---|
| `hello` | `app` (a name for the logs), `proto` 1 |
| `layer` | `ref`, `target`, `kind` (`video`, `image`, `effect`, `mesh`), the layer fields, and the kind's fields |
| `asset` | `ref`, `target`, `format` (`rgba8` default, `r8`), `w`, `h` (1-2048, within one frame message), `sampling` (`nearest` default, `bilinear`), `wrap` (`clamp` default, `repeat`) |
| `set` | `ref` and any layer field; for a mesh layer also its camera and projection fields (not `projection` itself) |
| `uniform` | `ref`, `name`, `v`, optional `type`, `part` ([below](#uniforms)); effect layers and mesh parts |
| `bind` | `ref` (an effect or mesh layer), `part` (a mesh layer's), `sampler` (the shader's `sampler2D`), `asset` (an asset's ref on the same target); binds again when sent again |
| `part` | `ref` (a mesh layer), `part` (1 or more), `vs`, `fs`, `depth` (1 default: tested less-or-equal and written; 0), `cull` (`back` default, `front`, `none`), `blend` (`alpha` default, `add`, `none`) |
| `transform` | `ref`, `part`, any of `tx`, `ty`, `tz`, `rx`, `ry`, `rz` (degrees, right-handed), `sx`, `sy`, `sz` |
| `animate` | `ref`, `part`, `anim`, `property`, `name`, `keys`, `loop`, `interp` ([below](#animations)) |
| `stop` | `ref`, `anim` |
| `scene` | `ref`, `target`, `file`, `res`, `sha256`, `contend` ([below](#scene-files)); sent again with its ref, another file in place |
| `edit` | `ref` (a scene), `lines` ([below](#scene-edits)) |
| `drop` | `ref`: a layer, a scene, or an asset no layer samples (else refused: `asset in use`) |
| `repeat` | `ref` (a video layer): a frame whose atlas is the layer's last frame's, byte for byte, in place of that frame; to a service whose hello has `repeat` 1 |
| `status` | — (answered by a `status` event) |

A client has at most 64 layers and all clients together 96 (past either, a
layer is refused with `too many layers`); a client has at most 16 assets,
all clients together 32 (past either: `too many assets`).

Layer fields (all optional):

| Field | Default | Meaning |
|---|---|---|
| `class` | `content` | `ambient`, `content`, `overlay` or `alert`: the stack's order, bottom to top |
| `z` | 0 | order within the class (-1000 to 1000); the newer layer is on top on a tie |
| `x`, `y`, `w`, `h` | the whole target | where the layer is drawn, in the target's pixels from its top-left |
| `visible` | 1 | 0 hides it; a target no visible layer wants is released after its linger |
| `exclusive` | 0 | 1 hides every layer below it while it is drawn |
| `prewarm` | 0 | 1 asks for the slot before the layer's first content |
| `contend` | 0 | 1 retries the slot after the stock HMI takes it (2, 5, 10, 20, then every 30 s), and on a target that takes over from the stock HMI (`map`), takes it from the stock content; 0 waits until it is free |
| `guide_hold` | 1 | on a target guide holds follow (`map`, iDrive 7): 0 keeps them released while this layer wants the target, so the stock HMI's guidance box shows (README.md "Guide hold") |
| `yield` | 1 | on a target with yield slots (`hud`): 0 keeps the target shown over the stock HMI's pop-ups while this layer wants it (README.md "Giving way") |
| `opacity` | 1 | 0-1, for video and image layers; drawn times the target's fade in and out |
| `rotate` | 0 | video, image and effect layers: degrees clockwise about the pivot |
| `sx`, `sy` | 1 | video, image and effect layers: scale about the pivot |
| `px`, `py` | the rect's centre | the pivot, in pixels from the rect's top-left; once set, kept until set again |

Kind fields:

| Kind | Field | Default | |
|---|---|---|---|
| video | `vw`, `vh` | 1088, 480 | the frame size; width a multiple of 4, height even, up to 1920x720 |
| | `fps` | 30 | updates a second at most (0: no clock) |
| | `level_bits` | 2 | low bits of each level rounded away (0-3) |
| | `dirty_threshold` | 8 | tiles differing by at most this many levels are not written (0-64) |
| | `settle_ms` | 1000 | such a tile is written after this long anyway (0: never) |
| | `pan_blocks` | 1 | fast motion as packed 4x4 blocks |
| | `stall_ms` | 6000 | no frame for this long: the layer is stalled (not drawn) |
| image | `iw`, `ih` | the rect | the image size, up to 1920x720 |
| | `fps` | 30 | writes a second at most |
| | `sampling` | `nearest` | or `bilinear` (a rect of another size than the image) |
| effect | `fs` | — | the fragment shader, GLSL ES 1.00, with `varying vec2 v_uv` (0,0 at the top-left) |
| | `vs` | the service's | a vertex shader with `attribute vec2 a_position` and `uniform mat4 u_mvp` |
| | `time_fps` | 0 | `u_time` (seconds since the layer was made) updated this often |
| | `time` | 0 | 1: `u_time` from the renderer's own clock, seconds from 0 to 3600 and again, nothing sent per frame (declare it `highp`); excludes `time_fps` |
| mesh | `projection` | `perspective` | or `ortho`; set when the layer is made |
| | `fov` | 30 | perspective: the vertical field of view, degrees (1-179) |
| | `ortho_h` | 2 | ortho: the height seen, in scene units |
| | `near`, `far` | 0.1, 100 | the clip planes |
| | `cx`, `cy`, `cz` | 0, 0, 0 | the camera's position; it looks down its -z |
| | `crx`, `cry`, `crz` | 0, 0, 0 | the camera's rotation, degrees, right-handed |

A mesh layer is drawn in a render pass of its own over its rect, at its
place in the stack, with the rect's aspect; a rect reaching past the target
is cut, not squeezed. It is ready once a part has geometry. Its passes clear
depth, never colour: the layers below it show.

Frames (`F`):
- **video**: no rects, and the whole 4:2:0 atlas: Y (w x h), then U and V
  side by side (w/2 each) in h/2 rows; `w * h * 3/2` bytes. Each frame
  replaces the layer's frame not yet written. A `repeat` counts as a frame
  (it keeps the layer from stalling), with nothing to write; before the
  layer's first frame it does nothing.
- **image**: RGBA rows, straight alpha. With rects, each rect's rows packed
  one rect after another; with none, the whole image.
- **asset**: the same, RGBA or one byte a pixel (`r8`).

An asset is not drawn and never makes its target wanted. The service keeps
its pixels, writes them whole into each new scene of its target and in place
as they change, before the layers, within the shared upload budget. A layer
that samples an asset is stalled until the asset has pixels, and waits while
the asset's buffer does not hold them whole.

### Shaders

GLSL ES 1.00. The renderer fills these uniforms wherever a shader declares
them, so in every shader they are reserved for it and must be mat4:
`u_mvp`, `u_mv`, `u_m`, `u_v`, `u_p` (model-view-projection, model-view,
model, view, projection) and `u_normal` (the inverse transpose of `u_mv`).
A part's vertex shader reads `attribute vec3 a_position` and any of
`vec3 a_normal`, `vec2 a_texcoord`, `vec4 a_color`; its geometry must carry
every attribute it reads.

### Uniforms

Without `type`, `v` is 1 to 4 numbers separated by spaces: a float, vec2,
vec3 or vec4. With `type` (`float`, `vec2`, `vec3`, `vec4`, `mat4`), `v` is
up to 1024 finite numbers, whole elements of the type: one value, or an
array uniform of that many elements (a mat4 is 16 numbers in GLSL's column
order). `part` names a mesh layer's part; an effect layer has none. A quad
or part holds at most 32 uniforms and 4096 numbers. A name the shader does
not have is dropped.

### Mesh geometry (`M`)

```
u32 ref | u32 part | u32 vertices | u32 indices | u8 mask |
f32 positions (xyz per vertex) |
[f32 normals (xyz)]    mask 1
[f32 texcoords (uv)]   mask 2
[f32 colors (rgba)]    mask 4
u16 indices (triangles)
```

All little-endian. 1 to 65535 vertices; 3 to 196605 indices, whole
triangles, each less than the vertex count; every value finite; the body at
most 4 MiB and exactly this long. Anything else is refused with an `error`
event (`op` `mesh`). The geometry becomes immutable resources in the
renderer: a part's geometry may be replaced, at most once a second, and its
bytes spend the shared upload budget.

### Animations

The renderer plays an animation by itself; nothing is sent per frame. `anim`
is the client's number for it (1 or more), unique within the layer: sending
it again replaces it, `stop` ends it and its property goes back to what the
client set. A layer plays at most 16.

| Field | |
|---|---|
| `part` | a mesh layer's part; absent: the layer's quad, or a mesh layer's camera |
| `property` | `rotate`, `translate`, `scale` or `uniform` |
| `name` | the uniform's name (`property` `uniform`) |
| `keys` | `t:v[,v,v,v] t:v ...`: `t` in ms from the start, ascending; every key the same number of values; at most 256 keys |
| `loop` | 1: from the first key again after the last; 0 (default): holds the last |
| `interp` | `linear` (default) or `step` |

Values a key takes:

| On | `rotate` | `translate` | `scale` | `uniform` |
|---|---|---|---|---|
| an image or effect layer | 1: degrees clockwise about the pivot | 3: pixels right, down, out from its place | 3 | 1, 3 or 4: float, vec3, vec4 |
| a mesh part | 3: degrees about x, y, z, right-handed | 3 | 3 | 1, 3 or 4 |
| a mesh layer's camera | 3 | 3 | — | — |

Animations are kept, and start again when a scene is published again.

### Scene files

On a target with `<target>_scene_files` 1 (cluster-render.conf; 0, off, on
every target as shipped; the RAMSES 19 backend), a client may have the
service load a RAMSES scene file as the target's whole scene in place of
the layers: content authored beforehand, saved with its resource files
(`RamsesClient::saveSceneToFile`), drawn by its own cameras and passes, and
changed live by edits ([below](#scene-edits)).

| Field | |
|---|---|
| `file` | the scene file, an absolute path |
| `res` | its resource files, absolute paths separated by spaces (at most 16; none: empty) |
| `sha256` | the SHA-256 of `file`, then of each file of `res` in its order: hex, separated by spaces |
| `contend` | as a layer's |

The service runs as `hmi_5000` and any client may name a file, and a
malformed file can take the RAMSES client down with the service, so it takes
only files that a root install put in place and nobody else can change:
each file's real path (symlinks and `..` resolved) must be under
`/var/opt/`, a regular file, and it and every folder below `/var/opt/` on
the way owned by root and writable by no one else; and each file's SHA-256
the one given (the files at most 256 MB together: they are hashed then, in
the render loop). Anything else is refused with an `error` event (`op`
`scene`). The file is loaded when the target's scene is published; it keeps
the scene id it was saved with, which must be the target's
(`<target>_scene_id`; a panel that has fallen back has
`panel_fallback_scene_id`, README.md "Scene ids"), else the scene is in
`error` with the reason.

A target has at most one scene (another is refused: `the target has a
scene file already`). While it is there and has not failed to load, the
target is wanted, as by a layer that wants it, and every layer on it is
`waiting` with `reason` `scene <ref>` (the client's own scene) or
`scene of <app>`, as below an exclusive layer. The scene's `layer` events:
`waiting` (the target not shown yet: its state and reason, the `target`
event naming who holds the slot), `shown` once the display shows the target
with this scene (where the service sees the renderer's subscriptions, once
the renderer has it), `error` when the file did not load. A client sends
its values again on `shown`. `drop`, or the client gone, ends the scene:
with nothing else wanting the target, it stays shown through
`<target>_linger_ms` and the target is released; a target the layers want
is published again with them at once. A scene coming to a target, or going
from one the layers want, takes the target out of its slot and asks for it
again with its new scene.

`scene` sent again with the scene's ref (its target unchanged) loads the
new file in place: the scene is unpublished and published again with the
same id while the target keeps its slot, and its kept edits go. A `layer`
event follows even when the state is the same.

### Scene edits

`lines`: lines separated by newlines (`\r\n` too; empty ones skipped), each
a kind, a space and its value. `RE` is a POSIX extended regular expression
found anywhere in an object's name (as `grep -E`, no back-references), at
most 256 characters; numbers are separated by commas.

| Line | Sets |
|---|---|
| `--hide RE`, `--show RE` | the visibility of each node matching and of every node under it |
| `--move RE=x,y,z`, `--scale RE=x,y,z` | a node's translation, its scaling |
| `--rotate RE=x,y,z` | a node's rotation: degrees, in the scene's own convention (nothing converted) |
| `--viewport RE=x,y,w,h` | a camera's viewport: pixels, from the bottom-left as RAMSES has it |
| `--pass-off RE`, `--pass-on RE` | a render pass disabled, enabled |
| `--uniform RE:NAME=v[,v...]` | the uniform `NAME` of each matching mesh's appearance: as many whole elements as given and it has (float, vec2-4, mat2-4; int and ivec2-4 rounded); one bound to a data object is not set (`--set` it) |
| `--set NAME=v[,v...]` | the data object called `NAME` (the name, not a pattern): float, int, vec2-4 |

A line replaces the kept line with the same key, its kind (`--show` counts
as `--hide`, `--pass-on` as `--pass-off`) and what comes before its value
(up to the last `=`), and goes to the end. The kept lines, at most 1024,
are applied in that order to each scene the file is loaded into, so a
scene published again shows what the lines so far made of it. New lines
are applied once a tick, in the target's one flush (not while the target
is parked; none when they change nothing). A pattern is resolved once per scene and kind of object, and
cached by its text.

A malformed line is refused at once, the other lines of the message taken:
an `error` event, `op` `edit`, `line` the line, `reason` naming it and
why. A line that changes nothing (no object matches, no mesh has the
uniform, not a regular expression) is reported the same way, once per
loaded scene.

## Service to client

Every JSON message has `ev`.

| ev | Fields | When |
|---|---|---|
| `hello` | `proto`, `backend` (`id7`, or `id8` on iDrive 8), `targets` (`map:1088x480,panel:1920x720,hud:800x480`; on iDrive 8 `map:874x749,hud:353x203,guide:702x213,widget:2240x896,popup:2240x896`), `repeat` (1: takes `repeat`) | after the client's hello |
| `layer` | `ref`, `state`, `reason` | the layer's state changed |
| `target` | `target`, `state`, `holder`, `reason`, `retry_in_ms`, `rendering`, `guide_disabled`, `disabled` | a target's state changed |
| `budget` | `cap_kb_per_s`, `max_kb_per_s`, `floor_kb_per_s`, `reason` | the upload cap changed |
| `error` | `op`, `ref`, `reason`; `line` for an edit line | a message was refused, or an edit line changed nothing |
| `status` | `text` | after a status request; among its lines, one for each slot the KOMBI's latest reports name: `slot 0x203 status 2: 0x1000400 class 0 map 1, ...` (each row's content, class and map status) |

Layer states (a scene's too, [above](#scene-files)): `shown`; `waiting`
(its target is not shown yet or is taken, an asset it samples is not
written yet, or a scene file is on its target; `reason` says which);
`suspended` (below an exclusive layer); `stalled` (no content yet: no frame,
no part with geometry, an asset it samples without pixels; or a video's
frames stopped); `hidden`; `error` (the renderer refused it). Target states:
`idle`, `acquiring`, `parked` (mapped, behind the content the slot shows:
`holder` `stock` or `foreign` once the slot table names it, and `reason`
`parked behind stock`, `parked behind foreign`, or `parked` until then; on
iDrive 8: offered, until the cluster shows it), `shown`, `lingering`,
`waiting` (with `holder` `stock` or `foreign` when
someone else holds the slot, and `retry_in_ms` when on a timer),
`unavailable`. `rendering`, for a shown or lingering target: `verified`
(the display's renderer is subscribed to its scene), `checking` (not
subscribed yet; after 1.5 s shown the service asks for the slot again),
`unobservable` (the service cannot see subscriptions); `none` otherwise.
`guide_disabled`: 1 while a guide hold following the target (`map`) is
disabled by the mapping guard or by the display refusing its slot, so the
stock HMI's guidance box shows whatever the layers' `guide_hold`; else 0.
`disabled`: 1 while the target's own mapping is disabled by the mapping
guard (the HUD's, README.md "Mapping guard"): it is never asked for, its
layers stay waiting; else 0.

On a head unit that refuses the service (README.md "Supported head
units") no `hello` comes: a `status` request gets `text` `not supported on
this car (head unit: ...)`, and every other message an `error` event with
that `reason`.

A client that does not read its events (over 1 MiB waiting) is
disconnected.
