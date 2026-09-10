# P4 validation gate

This folder is the host-side validation foundation described by
`.cursor/rules/p4-validation-architecture.mdc`. It is deliberately outside
`main/**`: running it does not build, flash, reset, or access the serial port.

Run the default fast gate from the repository root with the same Python that
started the runner:

```powershell
C:\Users\0000\.espressif\python_env\idf5.4_py3.14_env\Scripts\python.exe `
  tools\validation\gate\run_gate.py --scope fast
```

The command executes all selected checks even after a failure, returns `0` only
when every required check passes, and writes a stable JSON report to
`tools/validation/gate/reports/latest-fast.json`.

## Current proof boundary

- `contracts/` validates small, sanitized, versioned Event/Effect/Trace
  fixtures and their M+/M- oracles.
- `replay/` provides a deterministic FakeClock/FakeExecutor system model and
  six fault-injected session scenarios. Its golden effects are an executable
  product contract, not a copy of the ESP runtime.
- `gate/` runs the Trace contract, T2 scenarios, the existing FaceStateReducer
  and emotion policy host tests, the real architecture ratchet, and the
  code-health checker's own tests.
- A fast PASS proves the modeled T0/T1/T2 contracts and host logic only. It does not prove ESP
  ABI, FreeRTOS scheduling, PSRAM/DMA behavior, audio/display hardware, cold
  boot, or product feel.
- P4 image-layout checking remains a release/build gate because it requires a
  concrete firmware BIN; it is intentionally not part of the default fast
  scope.

## Adding a check

Add a manifest entry with a stable `id`, `tier`, `scope`, timeout, and command.
Checks must be deterministic, non-interactive, and must not hide a required
failure as `SKIP`. Put hardware-dependent validation in T4/T5 instead of
teaching the host gate to claim evidence it cannot produce.
