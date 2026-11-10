# libramses 27.0.116r1 ABI (iDrive 8)

Headers (`include/ramses27/`), a symbol list and a link stub for an iDrive 8
head unit's `/usr/lib/libramses-shared-lib-wayland-ivi-egl-es-3-0.so.27.0`
(BuildID `a31624ba8f4b678b8aff5486ccb2b262c671c0e0`, SHA-1
`0e2e5f7843a4aae37c7c11a9e408f02db6e9bfce`, firmware 23w50.3), whose DCSM
the cluster consumes. Unlike 19's ([ABI.md](ABI.md)), nothing here is
reconstructed: RAMSES 27.0.116 is open source, and `include/ramses27/` is
its own headers, unchanged, from tag `27.0.116` of
https://github.com/bmwcarit/ramses (commit
`2f04202d0975428cbc54d98e28723a951bb54b3d`;
`client/ramses-client/ramses-client-api/include` and
`framework/ramses-framework-api/include`): the ones the service includes
(`src/graph_ramses27.cpp`, `src/unit_idrive8.cpp`, `test/ramses_check.cpp`)
and theirs, 66 files, BMW Car IT, MPL-2.0. The unit's library is BMW's
build of that version ("r1") with a few exports of its own (below).

Labels as in [ABI.md](ABI.md): **VERIFIED** (seen in the binary or the
unit's configuration, or observed by running the real library),
**INFERRED**, **CAR-ONLY**. "The test unit" is the unit's own root
filesystem, booted off the car, with its vsomeip, policies, HMI and libramses, and a
stand-in cluster (a DCSM consumer and renderer, built on the same
library): no KOMBI.

## Using it

```make
RAMSES27_SONAME = libramses-shared-lib-wayland-ivi-egl-es-3-0.so.27.0
$(BUILD)/stub/$(RAMSES27_SONAME): abi/libramses27.symbols
	mkdir -p $(@D)
	python3 tools/ramses_abi.py stub-source $< > $(@D)/ramses27_stub.c
	gcc -shared -Wl,-soname,$(RAMSES27_SONAME) -o $@ $(@D)/ramses27_stub.c
# compile: g++ -isystem abi/include/ramses27 ...; link ... $(BUILD)/stub/$(RAMSES27_SONAME)
# then: python3 tools/ramses_abi.py check-imports BIN abi/libramses27.symbols
```

- `#include "ramses-client-api/Scene.h"` and the like, namespace `ramses`.
  `-isystem`, so `-Werror` does not hold upstream's own headers to the
  service's warnings.
- **Toolchain:** the service's (gcc 7, glibc 2.27, the unit's libstdc++), as
  for 19. The library links its own `libvsomeip.so.2`, CommonAPI and the
  RamsesService and RemoteContentManager SOME/IP bindings; the service
  links none of them.
- **`check-imports`** fails the build if any `ramses` import is missing from
  the unit's export list.
- **BMW's own:** `RamsesFrameworkConfig::enableSomeIPCommunication(uint32_t)`
  is not in the open-source headers. `src/unit_idrive8.cpp` declares it by
  its exported name.
- **The 19 and 27 libraries cannot share a process:** they export the same
  `ramses::` names, and the dynamic linker binds each to the first library
  that has it. So the service is two programs (`../src/unit.h`).

## Evidence per item

| Item | Evidence | Label |
|---|---|---|
| Every function the service calls exists with these parameter types | link against the stub + `check-imports` against `libramses27.symbols` (the library's `.dynsym` `ramses::` exports, 1829; unlike `libramses19.symbols`, the text API stays in: it is open source in 27): `cluster-render-idrive8` imports 133, all exported | VERIFIED |
| Every symbol resolves on the unit | `cluster-render-idrive8` run with the unit's own loader, glibc and libraries (`LD_BIND_NOW=1`, `ldd -r` style): no undefined symbol; it runs | VERIFIED |
| The calls behave as upstream's API says | `make test-ramses27` on the unit's library: every layer kind, effects compiled by its glslang, whole/partial/packed writes, passes and views, parts and geometry, every animation, a scene published again; nothing logged as an error | VERIFIED |
| Value classes the service puts on the stack are 24 bytes, as upstream's (`sizeof` 24): `RamsesFrameworkConfig`, `RamsesFramework`, `SceneConfig`, `EffectDescription`, `UniformInput`, `AttributeInput`, `DcsmMetadataCreator` | `StatusObject` C2 (0x1f9c00) writes the vptr and `[this+8]`; the C1 constructors of `SceneConfig` (0x1ab430), `EffectDescription` (0x17cac0), `DcsmMetadataCreator` (0x1cdc40) and `RamsesFramework(const RamsesFrameworkConfig&)` (0x1e8c70) then write `[this]` and `[this+0x10]`; `RamsesFrameworkConfig`'s setters read their impl at `[this+0x10]` (0x1e9a20 on); `UniformInput`/`AttributeInput` C1 (0x1c2d50, 0x176870) construct `EffectInput` and write the vptr | VERIFIED (`EffectInput`'s own layout INFERRED; both run in `make test-ramses27` and on the car) |
| `IDcsmProviderEventHandler` and `…Extended` vtables as upstream's: `contentHide` 0x00, `contentShow` 0x08, `stopOfferAccepted` 0x10, `contentSizeChange` 0x18, `contentReadyRequested` 0x20, `contentRelease` 0x28, destructors, `contentStatus` 0x40 | the library calls slot 0x18 on the handler it keeps at impl+0x70 and 0x40 on the extended one at impl+0x78; on the car a provider built on these headers got every event (icwidget); on the test unit the service did | VERIFIED |
| `enableSomeIPCommunication(uint32_t)` returns a status, and takes a communication user (below), not an instance | exported (`_ZN6ramses21RamsesFrameworkConfig25enableSomeIPCommunicationEj`); called with 801 on the car (icwidget) and by the service on the test unit | VERIFIED |
| BMW's other exports: `RamsesFramework::setSomeIPICServerAndClientManager`, `DcsmMetadataCreator::setConfiguratorPriority`, `DcsmMetadataUpdate::has/getConfiguratorPriority` | the symbol list; the service uses none. They change no object size (above) | VERIFIED; vtables unchanged INFERRED |
| Content ids are `432CCCDD`, categories `431CCCDD` (last digit: 1 cluster, 2 HUD, 3 CID); 43202001-43205000 are dynamic widgets, `C_HU_DynWidget<n>` | the library's id-to-name tables, and its own log of our offer: "ContentID C_HU_DynWidget43204001, Category Ca_Widget_IC" | VERIFIED |

## Communication users

`enableSomeIPCommunication(n)` names a RAMSES communication user, whose
RamsesService (0xB0BE) and RemoteContentManager (0xB0A3, DCSM) instances
the library picks (names from the library; instances from runs on the
test unit):

| user | name | RamsesService | DCSM | `connect()` |
|---|---|---|---|---|
| 5 | `HU_SUA` | | | DidUi's: a live HMI role, not used |
| 6 | `HU_VAR` | 6 | 4 | OK |
| 7 | `IC_HMI_IDC` | – | 1 | OK, DCSM only: no scenes reach the cluster |
| 201–210 | `HU_Test1…10` | 11… | – | "Could not connect to daemon" |
| **801** | `MGUTestUser_1` | **7** | **5** | **OK. On the car the cluster takes its offers (VERIFIED: the widget, 2026-10-25; the middle and the HUD, 2026-10-27)** |
| 802 | `MGUTestUser_2` | 8 | 6 | OK; the stand-in cluster on the test unit |

So the one participant both generations use is 801: RAMSES 19's
`MGUTestUserConnectingToAll` on iDrive 7, RAMSES 27's `MGUTestUser_1` on
iDrive 8. `vsomeip_app.json` gives each instance's vsomeip applications
fixed client ids (`RamsesNormalConnId.7` 0x6314, `RamsesResourceConnId.7`
0x6315), unused by the stock software (VERIFIED).

## vsomeip security

`check_credentials` is on (VERIFIED). Offering RamsesService and
RemoteContentManager, any instance, is allowed for uid/gid 5000/5000
(`hmi_5000`, as Hmi8 runs) and 6031/6200 (`nav_didui`, DidUi), in
`/etc/vsomeip/5000_5000/`, `6031_6200/` and `10017_902/vsomeip_security.json`
(VERIFIED); 6040, 6053, 6055, 6058 and 6100 (gid 6200) may only request
them, and root is denied.

- The service runs as `hmi_5000` on both generations, so its systemd unit is
  one. VERIFIED on the test unit: as uid 5000 (with `hmi_5000`'s groups, as
  systemd gives them) it connected as user 801, and the stand-in cluster
  took, showed and stopped its offers.
- On the car only `nav_didui` has run (icwidget). `hmi_5000` there is
  CAR-ONLY.

## Differences from RAMSES 19 the backend handles

`src/graph_ramses27.cpp` builds the same scenes as `graph_ramses19.cpp`:

- **Resources belong to a scene.** `Scene::createEffect` and
  `Scene::createArrayResource(EDataType, n, data)`; there are no client
  resources. An effect is compiled in a scratch scene (to check it and read
  its attributes) and in each scene that draws it.
- **Texture buffers:** `createTexture2DBuffer(ETextureFormat, w, h, mips)`
  (format first; `ETextureFormat` is an enum class) and
  `Texture2DBuffer::updateData(mip, x, y, w, h, data)`.
- **Samplers** take a minifying and a magnifying method:
  `ETextureSamplingMethod_Nearest` or `_Linear`.
- **`Scene::flush(sceneVersionTag_t)`**, no flush mode.
- **Nodes:** `createNode`; `setVisibility(EVisibilityMode)` (hidden is
  `Invisible`: rendering stops, resources stay); `setRotation(x, y, z)` is
  still the left-handed convention of 19's (documented as deprecated,
  still exported).
- **`setDepthWrite(EDepthWrite)`**, `EEffectUniformSemantic` an enum class.
- **Framework:** `RamsesFramework` is a `StatusObject` and makes its client
  (`createClient`) and DCSM provider (`createDcsmProvider`), both before
  `connect()`.

## Not exercised

- The real cluster's renderer drawing the service's scenes: CAR-ONLY.
  icwidget's scenes (text, a grid, a texture) were drawn on the car with the
  same calls; the service's mesh passes, effects and animations ran in the
  test unit's stand-in only.
- `RamsesClient` events are not used.
