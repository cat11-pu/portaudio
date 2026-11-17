#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include "mock_audio.h"

/*
 * Robust game audio engine under test.
 *
 * Invariants enforced by the engine itself:
 *   - every mock_device opened is eventually closed
 *   - every engine allocation goes through aheap_* and is freed on destroy
 *   - after any fault the engine returns to a defined, streaming-ready state
 *   - public calls are idempotent and safe in every state (no UB, no abort)
 */

typedef enum {
    ENGINE_UNINIT = 0,
    ENGINE_OPEN,
    ENGINE_STREAMING,
    ENGINE_REOPENING
} engine_state;

typedef struct audio_engine audio_engine;

typedef struct {
    unsigned open_failures_seen;
    unsigned recovered;
    unsigned recovered_by_kind[MOCK_STATUS_COUNT];
    unsigned ignored_ok;
} engine_counters;

audio_engine    *engine_create(mock_rng *rng);
void             engine_destroy(audio_engine *engine);

mock_status      engine_pump_half(audio_engine *engine);
engine_state     engine_state_get(const audio_engine *engine);
const engine_counters *engine_counters_get(const audio_engine *engine);
unsigned         engine_underflow_events(const audio_engine *engine);
const mock_device *engine_device_get(const audio_engine *engine);

/* Same API surface as the good engine, but with a deliberately injected flaw
 * selected by flaw: "leak", "stuck" or "crash". Used by the counterexample
 * driver only. */
audio_engine *bad_engine_create(mock_rng *rng, const char *flaw);
mock_status   bad_engine_pump_half(audio_engine *engine);
void          bad_engine_destroy(audio_engine *engine);

#endif /* AUDIO_ENGINE_H */
