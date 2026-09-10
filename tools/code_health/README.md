# Code health gates

Run from the repository root:

```powershell
python tools/code_health/architecture_guard.py
python tools/code_health/mjpeg_strangler_guard.py
python -m unittest discover -s tools/code_health -p "test_*.py"
python tools/code_health/check_p4_image_layout.py build/xiaozhi.bin
```

The guard is a ratchet: existing debt is accepted, growth is rejected. Do not
raise `architecture_baseline.json` merely to make CI green. Extract a cohesive
responsibility or document an intentional migration first.

The MJPEG strangler guard binds the G4 replay oracles to ordered semantics in
the production legacy player. Its normal mode accepts the documented
`dual_path_guarded` phase while reporting the remaining G5 blockers. It
also rejects drift between `mjpeg_runtime_selection.h` and the versioned
manifest, so compiled R0 choices cannot silently disagree with governance
evidence. Manifest v3 records production Host contracts separately from
runtime evidence and reports `failed`, `pending`, and `missing` retirement
blockers distinctly. Use
`--require-g5-ready` only at the deletion gate; never mark evidence `pass`
without the corresponding runtime or soak report.

`tools/build-idf.ps1` runs the P4 image-layout check after every successful
non-dry build. It rejects the near-64 KiB pre-IROM RAM padding shape that has
passed image hashes but repeatedly reset before `FW_MARKER` on the P4 v1.0.

See `docs/architecture-governance.md` for dependency rules and the G0–G5 plan.
