# T2 replay scenarios

These six `p4.trace.v1` fixtures are deterministic Host/Replay contracts. They
contain sanitized synthetic events, expected effects, explicit M+/M- oracles,
and at least one causal, order, or count invariant. `Fault.Inject` is an input
to the fake executor and never emits an effect by itself.

| Fixture | Fault injected | M+ | M- |
|---|---|---|---|
| `cold_boot_first_round.json` | Delay the first `Wake.Detected` by 120 ms. | Standby is seeded, wake begins one session, and the first TTS chunk is queued. | Boot does not request session close. |
| `long_tts.json` | Duplicate the next `TTS.Chunk`. | Three unique chunks are queued, the duplicate is suppressed, and the visual speech phase ends. | A long answer does not close the session. |
| `exit_session.json` | Delay `Session.ExitRequested` by 25 ms; the model then schedules close completion 50 ms later. | Close completes, wake is re-enabled, and standby is restored. | Close is not aborted. |
| `second_wake.json` | Drop the first post-exit `Wake.Detected`; a later retry is delivered. | Exactly two sessions begin with wake armed between them. | The dropped wake produces no synthetic begin effect. |
| `network_recovery.json` | Delay the first failed `Network.ReconnectResult` by 300 ms. | Offline is visible, failed results schedule 1 s then 2 s backoff, and success returns online. | A network fault cannot begin a voice session. |
| `same_emotion_cross_generation.json` | Duplicate a generation-1 `Emotion.Requested`, then commit at `Visual.SafePoint`. | The repeated request is deduplicated in the same generation, while the same emotion commits again in generation 2. | A cross-generation request is never suppressed as a same-generation duplicate. |

## Proof boundary

A green result proves only the modeled event-to-effect semantics, fake-clock
ordering, fault delivery, and declared invariants. It does not prove ESP-IDF or
FreeRTOS scheduling, UDP/AFE audio continuity, PSRAM/DMA behavior, SD or MIPI
timing, physical wake latency, rendered animation quality, or device lifetime.
Those remain Emulator/HIL/Human/V3 evidence. The fixtures deliberately do not
contain raw serial logs, user text, credentials, endpoint data, or hardware
claims.
