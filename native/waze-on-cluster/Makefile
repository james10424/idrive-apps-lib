# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Robert Nisipeanu
# waze-on-cluster. See README.md.
#
#   make test                 host build + NAL, RTP, the SPS fields and frame timing logged, the decoder
#                             against a fake MSDK, receive queue and catch-up, backlog, stream health,
#                             keyframe wait, the turn-box setting, the map's saved settings, the map feed
#                             against the shared renderer, the kit's tests, patcher,
#                             the aapd locator on every build in AAPD_BIN and
#                             AAPD_BINS, patchd patch/install activation/DLT/restart/driver profiles, the CarPlay
#                             Wi-Fi band, capture, boot-recovery, journal keeper, logs
#                             page, the logs archive and its firewall rule, and unit tests
#   make test-x86             the C++ host tests built and run with the unit's toolchain
#                             in Docker (x86-64: the SSE2 paths against their scalar twins)
#   make test-firmware ROOTFS=<dir>
#                             DLT capture against the firmware's own dlt-daemon,
#                             libdlt, dlt-receive, dlt-control and /etc/dlt.conf,
#                             in Docker
#   make target               head-unit binary build/target/waze-on-cluster,
#                             built in Docker (Ubuntu 18.04 amd64: gcc 7.5,
#                             glibc 2.27, the unit's libstdc++ ABI), linked
#                             against stubs of the unit's libmfxhw64, libva,
#                             libva-drm and the IAS AVB bridge, every import
#                             checked
#
# The map is drawn by the shared cluster renderer (CR, ../cluster-render),
# whose client and kit are compiled in, and whose tree holds the log, the
# config, the unit's ABI tools and the build image this build uses.

CXX     ?= c++
NODE    ?= node
PYTHON  ?= python3
BUILD   ?= build
JOBS    ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)   # parallel compiles

CR      ?= ../cluster-render
KIT      = $(CR)/kit
# the unit's toolchain ABI: the shared renderer's build image, by the same tag
IMAGE   := cluster-render-build:$(shell cksum < $(CR)/docker/Dockerfile | cut -d' ' -f1)
DOCKER_IMAGE = docker image inspect $(IMAGE) >/dev/null 2>&1 || \
	docker build -q --platform linux/amd64 -t $(IMAGE) $(CR)/docker
CXXFLAGS = -std=c++14 -O2 -g -Wall -Wextra -Werror
CPPFLAGS = -I. -Isrc -I$(KIT) -I$(CR)/include -Iabi/include -I$(CR)/src

T = $(BUILD)/target

# component.json's version, compiled into the binary for its log file.
VERSION := $(shell sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' component.json)

# The capture, decode and map feed (own files), the kit's atlas, packing and
# geometry, and the shared renderer's client.
WOC_SRC = src/main.cpp src/refused.cpp src/video_producer.cpp src/freeze_watch.cpp src/alt_screen.cpp src/map_feed.cpp src/map_feeds.cpp src/atlas_scale.cpp src/control.cpp src/h264_source.cpp \
          src/guide_setting.cpp src/map_state.cpp \
          src/udp_source.cpp src/avb_source.cpp src/framing_probe.cpp src/rtp_h264.cpp src/sps_info.cpp \
          src/decoder_msdk.cpp src/nal_reader.cpp src/stream_health.cpp $(KIT)/map_geometry.cpp $(KIT)/yuv_atlas.cpp \
          $(KIT)/block_pack.cpp
CR_SRC  = $(CR)/client/client.cpp $(CR)/client/protocol.cpp
SRC     = $(WOC_SRC) $(CR_SRC) $(CR)/src/config.cpp $(CR)/src/log.cpp $(CR)/src/head_unit.cpp

.PHONY: all test test-bin test-cpp test-node test-x86 test-x86-in-docker test-firmware target target-in-docker clean

CPP_TESTS = $(BUILD)/host/nal_reader_test $(BUILD)/host/rtp_h264_test $(BUILD)/host/udp_probe_test \
		$(BUILD)/host/map_feed_test $(BUILD)/host/keyframe_wait_test $(BUILD)/host/freeze_watch_test \
		$(BUILD)/host/alt_screen_test $(BUILD)/host/packet_queue_test $(BUILD)/host/backlog_test \
		$(BUILD)/host/stream_health_test $(BUILD)/host/source_crop_test \
		$(BUILD)/host/map_rect_test $(BUILD)/host/guide_setting_test $(BUILD)/host/decoder_test \
		$(BUILD)/host/map_state_test $(BUILD)/host/atlas_scale_test $(BUILD)/host/sps_info_test \
		$(BUILD)/host/frame_timing_test
# The control socket is Linux's (accept4): built and run on Linux only (make test-x86).
LINUX_TESTS = $(if $(filter Linux,$(shell uname -s)),$(BUILD)/host/refused_test)
CPP_TESTS += $(LINUX_TESTS)

all: test

# ---- host tests (no unit libraries) ---------------------------------------
# The test binaries build in parallel; the tests run one at a time.
test:
	$(MAKE) -j$(JOBS) test-bin
	$(MAKE) -j1 test-cpp test-node

test-bin: $(CPP_TESTS)
	$(MAKE) -C $(CR) test-kit-bin BUILD=$(abspath $(BUILD))/kit

# The patchers, patchd and boot recovery under the umask an installer may
# set (077): every file they replace keeps the mode its service needs.
test-node:
	umask 077 && $(NODE) patch/test/locate_test.js
	umask 077 && $(NODE) patch/test/permissions_test.js
	umask 077 && $(NODE) patch/test/file_mode_test.js
	umask 077 && $(NODE) patch/test/mutation_test.js
	umask 077 && $(NODE) patch/test/undo_test.js
	umask 077 && $(NODE) patch/test/patch_test.js
	umask 077 && $(NODE) patch/test/patchd_patch_test.js
	umask 077 && $(NODE) patch/test/patchd_activate_test.js
	umask 077 && $(NODE) patch/test/patchd_hud_test.js
	umask 077 && $(NODE) patch/test/patchd_profile_test.js
	umask 077 && $(NODE) patch/test/patchd_dlt_test.js
	umask 077 && $(NODE) patch/test/patchd_reboot_test.js
	umask 077 && $(NODE) patch/test/patchd_wifi_test.js
	umask 077 && $(NODE) patch/test/wifi_band_test.js
	umask 077 && $(NODE) patch/test/dlt_capture_test.js
	umask 077 && $(NODE) patch/test/recovery_test.js
	umask 077 && $(NODE) patch/test/journal_keep_test.js
	umask 077 && $(NODE) patch/test/logs_test.js
	umask 077 && $(NODE) patch/test/collect_logs_test.js
	umask 077 && $(NODE) patch/test/logs_fw_test.js
	umask 077 && $(NODE) patch/test/units_test.js
	umask 077 && $(NODE) patch/test/x86_flow_test.js
	umask 077 && $(NODE) patch/test/aapd_locate_test.js
	umask 077 && $(NODE) patch/test/aapd_test.js
	umask 077 && $(NODE) patch/test/head_unit_test.js

test-cpp: $(CPP_TESTS)
	$(BUILD)/host/nal_reader_test
	$(BUILD)/host/rtp_h264_test test/data/stream15_start.rtp
	$(BUILD)/host/udp_probe_test
	$(BUILD)/host/map_feed_test
	$(BUILD)/host/keyframe_wait_test
	$(BUILD)/host/freeze_watch_test
	$(BUILD)/host/alt_screen_test
	$(BUILD)/host/packet_queue_test
	$(BUILD)/host/backlog_test
	$(BUILD)/host/stream_health_test
	$(BUILD)/host/source_crop_test
	$(BUILD)/host/map_rect_test
	$(BUILD)/host/guide_setting_test
	$(BUILD)/host/decoder_test
	$(BUILD)/host/map_state_test
	$(BUILD)/host/atlas_scale_test
	$(BUILD)/host/sps_info_test
	$(BUILD)/host/frame_timing_test
	$(if $(LINUX_TESTS),$(LINUX_TESTS),@echo 'refused_test: Linux only (make test-x86)')
	$(MAKE) -C $(CR) test-kit BUILD=$(abspath $(BUILD))/kit

test-x86:
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w" -v "$(abspath $(CR)):/cr" -w /w $(IMAGE) \
		make -s BUILD=$(BUILD)/x86 CR=/cr CXX=g++ JOBS=$(JOBS) test-x86-in-docker

test-x86-in-docker:
	$(MAKE) -j$(JOBS) test-bin
	$(MAKE) -j1 test-cpp

test-firmware:
	@test -n "$(ROOTFS)" || { echo "set ROOTFS (a copy of the unit's root file system)"; exit 2; }
	sh patch/test/firmware/run.sh "$(ROOTFS)"

$(BUILD)/host/source_crop_test: test/source_crop_test.cpp src/source_crop.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/source_crop_test.cpp

# The decoder against a fake MSDK and libva.
DEC_SRC = test/decoder_test.cpp test/fake_msdk.cpp src/decoder_msdk.cpp src/nal_reader.cpp $(KIT)/yuv_atlas.cpp $(CR)/src/log.cpp
$(BUILD)/host/decoder_test: $(DEC_SRC) test/fake_msdk.h src/decoder_msdk.h src/source_crop.h src/nal_reader.h src/bit_reader.h \
		$(KIT)/yuv_atlas.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(KIT) -I$(CR)/src -Iabi/include -o $@ $(DEC_SRC) -lpthread

$(BUILD)/host/map_rect_test: test/map_rect_test.cpp src/map_rect.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/map_rect_test.cpp

$(BUILD)/host/nal_reader_test: test/nal_reader_test.cpp src/nal_reader.cpp src/nal_reader.h src/bit_reader.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/nal_reader_test.cpp src/nal_reader.cpp

$(BUILD)/host/rtp_h264_test: test/rtp_h264_test.cpp src/rtp_h264.cpp src/rtp_h264.h src/gap_settler.h src/nal_reader.cpp src/nal_reader.h \
		src/bit_reader.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/rtp_h264_test.cpp src/rtp_h264.cpp src/nal_reader.cpp

$(BUILD)/host/udp_probe_test: test/udp_probe_test.cpp src/udp_source.cpp src/udp_source.h \
		src/framing_probe.cpp src/framing_probe.h $(CR)/src/log.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(CR)/src -o $@ test/udp_probe_test.cpp src/udp_source.cpp src/framing_probe.cpp $(CR)/src/log.cpp -lpthread

# The map feed against the shared renderer's service on fake display
# backends, over its socket, with the freeze watch driving it: every service
# source but the units' backends, the starter and its main.
CR_CORE = $(filter-out $(addprefix $(CR)/src/,main.cpp graph_ramses19.cpp dcsm2_link.cpp config.cpp log.cpp \
                         unit_idrive7.cpp unit_idrive8.cpp graph_ramses27.cpp launch.cpp generation.cpp refused.cpp head_unit.cpp), \
          $(wildcard $(CR)/src/*.cpp)) $(wildcard $(KIT)/*.cpp)
# Each of its sources compiles once, with its header dependencies.
MF       = $(BUILD)/host/map_feed
MF_FLAGS = $(CXXFLAGS) -Isrc -I$(KIT) -I$(CR)/include -I$(CR) -I$(CR)/src -isystem $(CR)/dcsm/third_party/vsomeip/interface
MF_SRC   = test/map_feed_test.cpp src/map_feed.cpp src/map_feeds.cpp src/atlas_scale.cpp src/freeze_watch.cpp $(filter-out $(CR)/client/protocol.cpp,$(CR_CORE)) $(CR_SRC) \
           $(CR)/src/log.cpp

# objects of sources in this tree under <dir>/, of the shared renderer's under <dir>/cr/
objs = $(foreach s,$(1),$(if $(filter $(CR)/%,$(s)),$(patsubst $(CR)/%.cpp,$(2)/cr/%.o,$(s)),$(patsubst %.cpp,$(2)/%.o,$(s))))

$(MF)/cr/%.o: $(CR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(MF_FLAGS) -MMD -MP -c -o $@ $<

$(MF)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(MF_FLAGS) -MMD -MP -c -o $@ $<

-include $(wildcard $(MF)/*/*.d $(MF)/cr/*/*.d)

$(BUILD)/host/map_feed_test: $(call objs,$(MF_SRC),$(MF))
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(BUILD)/host/sps_info_test: test/sps_info_test.cpp src/sps_info.cpp src/sps_info.h src/bit_reader.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/sps_info_test.cpp src/sps_info.cpp

$(BUILD)/host/frame_timing_test: test/frame_timing_test.cpp src/frame_timing.h $(KIT)/frame_clock.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(KIT) -o $@ test/frame_timing_test.cpp

$(BUILD)/host/keyframe_wait_test: test/keyframe_wait_test.cpp src/keyframe_wait.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/keyframe_wait_test.cpp

$(BUILD)/host/freeze_watch_test: test/freeze_watch_test.cpp src/freeze_watch.cpp src/freeze_watch.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/freeze_watch_test.cpp src/freeze_watch.cpp

$(BUILD)/host/alt_screen_test: test/alt_screen_test.cpp src/alt_screen.cpp src/alt_screen.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/alt_screen_test.cpp src/alt_screen.cpp

$(BUILD)/host/packet_queue_test: test/packet_queue_test.cpp src/packet_queue.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ test/packet_queue_test.cpp -lpthread

$(BUILD)/host/backlog_test: test/backlog_test.cpp src/backlog.h $(KIT)/frame_clock.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(KIT) -o $@ test/backlog_test.cpp

$(BUILD)/host/stream_health_test: test/stream_health_test.cpp src/stream_health.cpp src/stream_health.h $(CR)/src/log.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(CR)/src -o $@ test/stream_health_test.cpp src/stream_health.cpp $(CR)/src/log.cpp -lpthread

$(BUILD)/host/refused_test: test/refused_test.cpp src/refused.cpp src/refused.h src/control.cpp src/control.h \
		$(CR)/src/config.cpp $(CR)/src/log.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(CR)/src -o $@ test/refused_test.cpp src/refused.cpp src/control.cpp $(CR)/src/config.cpp \
		$(CR)/src/log.cpp -lpthread

$(BUILD)/host/guide_setting_test: test/guide_setting_test.cpp src/guide_setting.cpp src/guide_setting.h $(CR)/src/log.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(CR)/src -o $@ test/guide_setting_test.cpp src/guide_setting.cpp $(CR)/src/log.cpp -lpthread

$(BUILD)/host/map_state_test: test/map_state_test.cpp src/map_state.cpp src/map_state.h $(CR)/src/config.cpp $(CR)/src/log.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(CR)/src -o $@ test/map_state_test.cpp src/map_state.cpp $(CR)/src/config.cpp $(CR)/src/log.cpp -lpthread

# ---- target binary (in Docker) --------------------------------------------
target:
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w" -v "$(abspath $(CR)):/cr" -w /w $(IMAGE) \
		make -s -j$(JOBS) BUILD=$(BUILD) CR=/cr target-in-docker

target-in-docker: $(T)/waze-on-cluster

# Link-only stubs exporting the unit libraries' symbols. Each carries the real
# SONAME, so the binary records the right DT_NEEDED.
$(T)/stub/libmfxhw64.so: abi/libmfx.symbols
	mkdir -p $(@D)
	$(PYTHON) $(CR)/tools/ramses_abi.py stub-source $< > $(@D)/mfx_stub.c
	gcc -shared -Wl,-soname,libmfxhw64.so -o $@ $(@D)/mfx_stub.c

$(T)/stub/libva.so.1: abi/libva.symbols
	mkdir -p $(@D)
	$(PYTHON) $(CR)/tools/ramses_abi.py stub-source $< > $(@D)/va_stub.c
	gcc -shared -Wl,-soname,libva.so.1 -o $@ $(@D)/va_stub.c

$(T)/stub/libva-drm.so.1: abi/libva-drm.symbols
	mkdir -p $(@D)
	$(PYTHON) $(CR)/tools/ramses_abi.py stub-source $< > $(@D)/va_drm_stub.c
	gcc -shared -Wl,-soname,libva-drm.so.1 -o $@ $(@D)/va_drm_stub.c

$(T)/stub/libias-media_transport-avb_ufipc_bridge.so.1: abi/libias_avb.symbols
	mkdir -p $(@D)
	$(PYTHON) $(CR)/tools/ramses_abi.py stub-source $< > $(@D)/ias_avb_stub.c
	gcc -shared -Wl,-soname,libias-media_transport-avb_ufipc_bridge.so.1 -o $@ $(@D)/ias_avb_stub.c

STUBS = $(T)/stub/libmfxhw64.so $(T)/stub/libva.so.1 $(T)/stub/libva-drm.so.1 \
        $(T)/stub/libias-media_transport-avb_ufipc_bridge.so.1

# Each source compiles once, in parallel, with its header dependencies.
WOC_FLAGS = $(CXXFLAGS) $(CPPFLAGS) -DWOC_VERSION='"$(VERSION)"' -pthread

$(T)/obj/cr/%.o: $(CR)/%.cpp component.json
	@mkdir -p $(@D)
	g++ $(WOC_FLAGS) -MD -MP -c -o $@ $<

$(T)/obj/%.o: %.cpp component.json
	@mkdir -p $(@D)
	g++ $(WOC_FLAGS) -MD -MP -c -o $@ $<

-include $(wildcard $(T)/obj/*/*.d $(T)/obj/cr/*/*.d)

$(T)/waze-on-cluster: $(call objs,$(SRC),$(T)/obj) $(STUBS)
	g++ $(WOC_FLAGS) -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2 -o $@ $^
	$(PYTHON) $(CR)/dcsm/tools/abi.py check $@ $(CR)/dcsm/abi/libvsomeip3.so.3.symbols   # loader, glibc/libstdc++ limits
	$(PYTHON) tools/woc_abi.py check-imports $@ --prefix MFX va ias_ -- \
		abi/libmfx.symbols abi/libva.symbols abi/libva-drm.symbols abi/libias_avb.symbols

clean:
	rm -rf $(BUILD)

$(BUILD)/host/atlas_scale_test: test/atlas_scale_test.cpp src/atlas_scale.cpp src/atlas_scale.h $(KIT)/yuv_atlas.cpp $(KIT)/yuv_atlas.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Isrc -I$(KIT) -o $@ test/atlas_scale_test.cpp src/atlas_scale.cpp $(KIT)/yuv_atlas.cpp -lpthread
