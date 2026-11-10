# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Robert Nisipeanu

# cluster-render: the one service that draws on the instrument cluster and
# the HUD for every app (README.md), and kit/, the pixel path it shares with
# producers that prepare frames: the 4:2:0 YUV atlas, dirty-tile planning,
# 4x4 packed fast-motion blocks, SIMD conversion and rounding, upload
# pacing, the frame clock and the map-area geometry.
#
#   make test                 every host test: the kit, slot arbitration, the
#                             RAMSES console reader, the upload governor, the
#                             stack, layers and compositor
#                             on a fake renderer, the receive buffer, scene
#                             files and their edits, the service over its socket
#                             with the C++ client, and the Node client
#   make test-kit             the kit's tests alone (for producers that link it)
#   make test-x86             the C++ tests built and run with the unit's
#                             toolchain in Docker (x86-64: the SSE2 paths)
#   make test-kit-x86         the kit's alone, likewise
#   make target               head-unit binaries in build/target, built in
#                             Docker (Ubuntu 18.04 amd64: gcc 7.5, glibc 2.27,
#                             the unit's libstdc++ ABI): cluster-render, which
#                             starts the service of the unit's generation,
#                             cluster-render-idrive7 against stubs of the
#                             unit's libramses 19 and libvsomeip3 and
#                             cluster-render-idrive8 against a stub of
#                             iDrive 8's libramses 27, every import checked
#   make test-ramses ROOTFS=… every layer kind built by the unit's own libramses,
#                             and a scene file it saved, loaded and edited
#   make test-ramses27 ROOTFS=…  the same on an iDrive 8 unit's libramses 27

CXX     ?= c++
NODE    ?= node
PYTHON  ?= python3
BUILD   ?= build
JOBS    ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)   # parallel compiles

# The unit's toolchain ABI (docker/Dockerfile), tagged with the Dockerfile's
# checksum: built when no image has that tag.
IMAGE   := cluster-render-build:$(shell cksum < docker/Dockerfile | cut -d' ' -f1)
DOCKER_IMAGE = docker image inspect $(IMAGE) >/dev/null 2>&1 || docker build -q --platform linux/amd64 -t $(IMAGE) docker

CXXFLAGS = -std=c++14 -O2 -g -Wall -Wextra -Werror
HOSTINC  = -Ikit -Iinclude -I. -Isrc -isystem dcsm/third_party/vsomeip/interface
CPPFLAGS = $(HOSTINC) -Iabi/include
CPPFLAGS27 = $(HOSTINC) -isystem abi/include/ramses27   # RAMSES 27.0.116's own headers (abi/ABI27.md)

# component.json's version, compiled into the binary for its log file.
VERSION := $(shell sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' component.json)

RAMSES_SONAME = libramses-shared-lib-wayland-ivi-egl-es-3-0.so.19.0
RAMSES27_SONAME = libramses-shared-lib-wayland-ivi-egl-es-3-0.so.27.0
T = $(BUILD)/target
H = $(BUILD)/host

KIT_SRC  = kit/dirty_tiles.cpp kit/yuv_atlas.cpp kit/block_pack.cpp
CORE_SRC = src/slots.cpp src/guard.cpp src/governor.cpp src/stack.cpp src/recv_buffer.cpp src/layer.cpp src/video_layer.cpp src/mesh_layer.cpp \
           src/asset.cpp src/compositor.cpp src/scene_file.cpp src/scene_edit.cpp \
           src/server.cpp src/service.cpp src/dcsm2_port.cpp src/dcsm27_port.cpp src/ramses_console.cpp \
           client/protocol.cpp src/log.cpp
CORE_H   = $(wildcard src/*.h kit/*.h include/cluster_render/*.h)
UNIT_SRC = src/main.cpp src/head_unit.cpp src/refused.cpp src/holds.cpp src/unit_idrive7.cpp src/graph_ramses19.cpp src/dcsm2_link.cpp src/config.cpp dcsm/dcsm_client.cpp
UNIT8_SRC = src/main.cpp src/head_unit.cpp src/refused.cpp src/holds.cpp src/unit_idrive8.cpp src/graph_ramses27.cpp src/config.cpp
LAUNCH_SRC = src/main.cpp src/head_unit.cpp src/refused.cpp src/holds.cpp src/launch.cpp src/generation.cpp src/config.cpp client/protocol.cpp src/log.cpp
CLIENT_SRC = client/client.cpp client/protocol.cpp

.PHONY: all test test-cpp test-js test-kit test-kit-bin test-x86 test-x86-in-docker test-kit-x86 image target target-in-docker \
	test-ramses test-ramses27 clean

KIT_TESTS = $(H)/map_upload_test $(H)/yuv_atlas_test $(H)/frame_clock_test $(H)/block_pack_test
CPP_TESTS = $(H)/log_test $(H)/slots_test $(H)/governor_test $(H)/compositor_test $(H)/service_test \
	$(H)/dcsm2_port_test $(H)/ramses_console_test $(H)/dcsm27_port_test $(H)/generation_test $(H)/holds_test \
	$(H)/guard_test $(H)/head_unit_test $(H)/refused_test $(H)/recv_buffer_test $(H)/scene_test

all: test

# The test binaries build in parallel; the tests run one at a time.
test:
	$(MAKE) -j$(JOBS) $(KIT_TESTS) $(CPP_TESTS) $(H)/host_service
	$(MAKE) -j1 test-kit test-cpp test-js

test-cpp: $(CPP_TESTS)
	$(H)/log_test
	$(H)/slots_test
	$(H)/ramses_console_test
	$(H)/governor_test
	$(H)/compositor_test
	$(H)/dcsm2_port_test
	$(H)/dcsm27_port_test
	$(H)/generation_test
	$(H)/holds_test
	$(H)/guard_test
	$(H)/head_unit_test test/head_unit_cases.jsonl
	$(H)/refused_test $(H)/cluster-render
	$(H)/recv_buffer_test
	$(H)/scene_test
	$(H)/service_test

test-js: $(H)/host_service
	$(NODE) js/test/client_test.js $(H)/host_service

test-kit-bin: $(KIT_TESTS)

test-kit: $(KIT_TESTS)
	$(H)/map_upload_test
	$(H)/yuv_atlas_test
	$(H)/frame_clock_test
	$(H)/block_pack_test

test-x86:
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w/cluster-render" -w /w/cluster-render $(IMAGE) \
		make -s BUILD=$(BUILD)/x86 CXX=g++ JOBS=$(JOBS) test-x86-in-docker

test-x86-in-docker:
	$(MAKE) -j$(JOBS) $(KIT_TESTS) $(CPP_TESTS)
	$(MAKE) -j1 test-kit test-cpp

test-kit-x86:
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w/cluster-render" -w /w/cluster-render $(IMAGE) \
		make -s BUILD=$(BUILD)/x86 CXX=g++ test-kit

# ---- host tests -------------------------------------------------------------
# Each source compiles once into $(H)/obj (the service's include path) and
# $(H)/kitobj (the kit's alone: producers build it without the service), with
# its header dependencies; the test binaries link the objects they need.
O  = $(H)/obj
KO = $(H)/kitobj
CORE_OBJ = $(addprefix $(O)/,$(CORE_SRC:.cpp=.o) $(KIT_SRC:.cpp=.o))

$(O)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(HOSTINC) -MMD -MP -c -o $@ $<

$(KO)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Ikit -MMD -MP -c -o $@ $<

-include $(wildcard $(O)/*/*.d $(KO)/*/*.d)

$(H)/log_test: $(O)/test/log_test.o $(O)/src/log.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/slots_test: $(O)/test/slots_test.o $(O)/src/slots.o $(O)/src/guard.o $(O)/src/governor.o $(O)/src/log.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/ramses_console_test: $(O)/test/ramses_console_test.o $(O)/src/ramses_console.o $(O)/src/log.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/governor_test: $(O)/test/governor_test.o $(O)/src/governor.o $(O)/src/log.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/compositor_test: $(O)/test/compositor_test.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/dcsm2_port_test: $(O)/test/dcsm2_port_test.o $(O)/src/dcsm2_port.o $(O)/src/slots.o $(O)/src/guard.o $(O)/src/governor.o \
		$(O)/src/log.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/dcsm27_port_test: $(O)/test/dcsm27_port_test.o $(O)/src/dcsm27_port.o $(O)/src/slots.o $(O)/src/guard.o $(O)/src/governor.o \
		$(O)/src/log.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/generation_test: $(O)/test/generation_test.o $(O)/src/generation.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(H)/head_unit_test: $(O)/test/head_unit_test.o $(O)/src/head_unit.o $(O)/client/protocol.o
	$(CXX) $(CXXFLAGS) -o $@ $^

# The launcher on the host: what it does before it starts a generation's service.
$(H)/cluster-render: $(addprefix $(O)/,$(LAUNCH_SRC:.cpp=.o))
	$(CXX) $(CXXFLAGS) -o $@ $^

$(H)/refused_test: $(O)/test/refused_test.o $(O)/client/protocol.o $(H)/cluster-render
	$(CXX) $(CXXFLAGS) -o $@ $(O)/test/refused_test.o $(O)/client/protocol.o

$(H)/holds_test: $(O)/test/holds_test.o $(O)/src/holds.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(H)/guard_test: $(O)/test/guard_test.o $(O)/src/guard.o $(O)/src/log.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/recv_buffer_test: $(O)/test/recv_buffer_test.o $(O)/src/recv_buffer.o $(O)/client/protocol.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(H)/scene_test: $(O)/test/scene_test.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/service_test: $(O)/test/service_test.o $(CORE_OBJ) $(O)/client/client.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/host_service: $(O)/test/host_service.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/map_upload_test: $(KO)/test/map_upload_test.o $(KO)/kit/map_geometry.o $(KO)/kit/dirty_tiles.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(H)/yuv_atlas_test: $(KO)/test/yuv_atlas_test.o $(KO)/kit/yuv_atlas.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(H)/block_pack_test: $(KO)/test/block_pack_test.o $(KO)/kit/block_pack.o $(KO)/kit/yuv_atlas.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(H)/frame_clock_test: $(KO)/test/frame_clock_test.o
	$(CXX) $(CXXFLAGS) -o $@ $^

# ---- target binary (in Docker) ----------------------------------------------
# The build image, built when missing; prints its tag.
image:
	@$(DOCKER_IMAGE) >/dev/null
	@echo $(IMAGE)

target:
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w/cluster-render" -w /w/cluster-render $(IMAGE) \
		make -s -j$(JOBS) BUILD=$(BUILD) target-in-docker

target-in-docker: $(T)/cluster-render $(T)/cluster-render-idrive7 $(T)/cluster-render-idrive8 \
	$(T)/libcluster_render_client.a

$(T)/stub/$(RAMSES_SONAME): abi/libramses19.symbols
	mkdir -p $(@D)
	$(PYTHON) tools/ramses_abi.py stub-source $< > $(@D)/ramses_stub.c
	gcc -shared -Wl,-soname,$(RAMSES_SONAME) -o $@ $(@D)/ramses_stub.c

$(T)/stub/$(RAMSES27_SONAME): abi/libramses27.symbols
	mkdir -p $(@D)
	$(PYTHON) tools/ramses_abi.py stub-source $< > $(@D)/ramses27_stub.c
	gcc -shared -Wl,-soname,$(RAMSES27_SONAME) -o $@ $(@D)/ramses27_stub.c

$(T)/stub/libvsomeip3.so.3: dcsm/abi/libvsomeip3.so.3.symbols
	mkdir -p $(@D)
	$(PYTHON) dcsm/tools/abi.py stub $< $(@D)/vsomeip_stub.S
	g++ -shared -nostdlib -Wl,-soname,libvsomeip3.so.3 -o $@ $(@D)/vsomeip_stub.S

STUBS = $(T)/stub/$(RAMSES_SONAME) $(T)/stub/libvsomeip3.so.3

# Each binary's sources compile in a folder of their own, with that binary's
# flags and their header dependencies (-MD: the -isystem RAMSES and vsomeip
# headers too).
LAUNCH_FLAGS  = $(CXXFLAGS) $(HOSTINC) -pthread
IDRIVE7_FLAGS = $(CXXFLAGS) $(CPPFLAGS) -DCR_VERSION='"$(VERSION)"' -pthread
IDRIVE8_FLAGS = $(CXXFLAGS) $(CPPFLAGS27) -DCR_VERSION='"$(VERSION)"' -pthread
LD_UNIT       = -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2

$(T)/launch/%.o: %.cpp
	@mkdir -p $(@D)
	g++ $(LAUNCH_FLAGS) -MD -MP -c -o $@ $<

$(T)/idrive7/%.o: %.cpp component.json
	@mkdir -p $(@D)
	g++ $(IDRIVE7_FLAGS) -MD -MP -c -o $@ $<

$(T)/idrive8/%.o: %.cpp component.json
	@mkdir -p $(@D)
	g++ $(IDRIVE8_FLAGS) -MD -MP -c -o $@ $<

-include $(wildcard $(T)/launch/*/*.d $(T)/idrive7/*/*.d $(T)/idrive8/*/*.d)

# Starts the service of the unit's generation (src/unit.h): needs neither
# libramses.
$(T)/cluster-render: $(addprefix $(T)/launch/,$(LAUNCH_SRC:.cpp=.o))
	g++ $(LAUNCH_FLAGS) $(LD_UNIT) -o $@ $^
	$(PYTHON) tools/ramses_abi.py check-imports $@ abi/libramses19.symbols

$(T)/cluster-render-idrive7: $(addprefix $(T)/idrive7/,$(CORE_SRC:.cpp=.o) $(KIT_SRC:.cpp=.o) $(UNIT_SRC:.cpp=.o)) $(STUBS)
	g++ $(IDRIVE7_FLAGS) $(LD_UNIT) -o $@ $^
	$(PYTHON) tools/ramses_abi.py check-imports $@ abi/libramses19.symbols
	$(PYTHON) dcsm/tools/abi.py check $@ dcsm/abi/libvsomeip3.so.3.symbols

$(T)/cluster-render-idrive8: $(addprefix $(T)/idrive8/,$(CORE_SRC:.cpp=.o) $(KIT_SRC:.cpp=.o) $(UNIT8_SRC:.cpp=.o)) \
		$(T)/stub/$(RAMSES27_SONAME)
	g++ $(IDRIVE8_FLAGS) $(LD_UNIT) -o $@ $^
	$(PYTHON) tools/ramses_abi.py check-imports $@ abi/libramses27.symbols

# Every layer kind on the unit's own libramses (TCP, no renderer).
test-ramses:
	@test -n "$(ROOTFS)" || { echo "set ROOTFS to a copy of the unit's root filesystem"; exit 2; }
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w/cluster-render" \
		-v "$(ROOTFS):/rootfs:ro" -w /w/cluster-render $(IMAGE) \
		sh -c 'make -s BUILD=$(BUILD) $(T)/ramses_check && \
		/rootfs/lib/ld-linux-x86-64.so.2 --library-path /rootfs/lib:/rootfs/usr/lib $(T)/ramses_check \
		> $(T)/ramses_check.log 2>&1; rc=$$?; grep -v "Info " $(T)/ramses_check.log; \
		if grep -q "\[ Error \]" $(T)/ramses_check.log; then echo "FAIL libramses logged errors"; exit 1; fi; exit $$rc'

$(T)/ramses_check: test/ramses_check.cpp test/fake_port.h src/graph_ramses19.cpp $(CORE_SRC) $(KIT_SRC) $(CORE_H) \
		$(T)/stub/$(RAMSES_SONAME)
	g++ $(CXXFLAGS) $(CPPFLAGS) -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2 -pthread -o $@ test/ramses_check.cpp \
		src/graph_ramses19.cpp $(CORE_SRC) $(KIT_SRC) $(T)/stub/$(RAMSES_SONAME)

# The same on iDrive 8's libramses 27 (TCP, no renderer).
test-ramses27:
	@test -n "$(ROOTFS)" || { echo "set ROOTFS to a copy of an iDrive 8 unit's root filesystem"; exit 2; }
	$(DOCKER_IMAGE)
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/w/cluster-render" \
		-v "$(ROOTFS):/rootfs:ro" -w /w/cluster-render $(IMAGE) \
		sh -c 'make -s BUILD=$(BUILD) $(T)/ramses_check27 && \
		/rootfs/lib/ld-linux-x86-64.so.2 --library-path /rootfs/lib:/rootfs/usr/lib $(T)/ramses_check27 \
		> $(T)/ramses_check27.log 2>&1; rc=$$?; grep -v "Info " $(T)/ramses_check27.log; \
		if grep -q "\[ Error \]" $(T)/ramses_check27.log; then echo "FAIL libramses logged errors"; exit 1; fi; exit $$rc'

$(T)/ramses_check27: test/ramses_check.cpp test/fake_port.h src/graph_ramses27.cpp $(CORE_SRC) $(KIT_SRC) $(CORE_H) \
		$(T)/stub/$(RAMSES27_SONAME)
	g++ $(CXXFLAGS) $(CPPFLAGS27) -DCRS_RAMSES27 -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2 -pthread -o $@ \
		test/ramses_check.cpp src/graph_ramses27.cpp $(CORE_SRC) $(KIT_SRC) $(T)/stub/$(RAMSES27_SONAME)

# The C++ client for native producers on the unit, as a static library.
$(T)/libcluster_render_client.a: $(CLIENT_SRC) include/cluster_render/client.h include/cluster_render/protocol.h
	mkdir -p $(T)/client
	g++ $(CXXFLAGS) -Iinclude -c client/client.cpp -o $(T)/client/client.o
	g++ $(CXXFLAGS) -Iinclude -c client/protocol.cpp -o $(T)/client/protocol.o
	ar rcs $@ $(T)/client/client.o $(T)/client/protocol.o

clean:
	rm -rf $(BUILD)
