# esp32-p4-robot

This repository contains a source snapshot migrated from the local Xiaozhi P4
engineering workspace on 2026-09-11. Its firmware project retains the internal
build target name `xiaozhi`; the repository name does not change device behavior.

- Firmware source: `main/`, local components: `components/`.
- PC character pack compiler and builders: `tools/emotion_tool_dev/`.
- Character packs: `tools/emotion_assets/`.
- Product schemas: `tools/product_contracts/`.
- Build inputs: `CMakeLists.txt`, `sdkconfig.defaults*`, and `dependencies.lock`.

The v5p3 pack is a historical tested baseline. The v5p4, v5p5, v5p6 and idle flash
assets are retained as separately named source assets/candidates. Inclusion here
does not certify their deployment or runtime stability. Do not mix pack files.

The snapshot is the current local source, not a claim that it matches the running
device or that a release build was validated during migration. Existing README
files retain upstream attribution and historical links.

Firmware images, BIN/ELF, local agent rules, environment credentials, build
directories, serial logs and local crash history are intentionally excluded.
The old repository's complete Git refs/history are backed up locally before
retirement. The new main branch begins with this source snapshot.

CI workflows are not auto-enabled by migration: their secrets and cloud build
inputs must be configured separately before enabling deployment/build automation.
