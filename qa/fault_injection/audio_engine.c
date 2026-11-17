#include "audio_engine.h"

#include <stdio.h>
#include <string.h>

#define DEVICE_CAPACITY 256u
#define DEVICE_PERIOD   16u
#define LOW_WATERMARK   64u
#define MAX_OPEN_RETRY  3u

struct audio_engine {
    mock_device  *dev;
    engine_state  state;
    unsigned      open_attempts;
    unsigned      phase;          /* sample counter for the test tone */
    unsigned      underflow_gaps;
    engine_counters counters;
    mock_rng     *rng;
};

static mock_status device_open_with_retry(audio_engine *engine)
{
    while (engine->open_attempts < MAX_OPEN_RETRY) {
        engine->open_attempts++;
        engine->dev = mock_open(DEVICE_CAPACITY, DEVICE_PERIOD,
                                FAULT_HOTPLUG | FAULT_TIMEOUT |
                                    FAULT_UNDERRUN | FAULT_OVERRUN,
                                engine->rng);
        if (engine->dev != NULL) {
            mock_status st = mock_start(engine->dev);
            mock_global_log("engine open success on attempt %u",
                            engine->open_attempts);
            engine->open_attempts = 0;
            engine->state = ENGINE_STREAMING;
            return st;
        }
        engine->counters.open_failures_seen++;
        engine->counters.recovered++;
        engine->counters.recovered_by_kind[MOCK_ERR_OPEN]++;
        mock_global_log("engine open attempt %u failed, retrying",
                        engine->open_attempts);
    }
    mock_global_log("engine open gave up after %u attempts",
                    MAX_OPEN_RETRY);
    return MOCK_ERR_OPEN;
}

audio_engine *engine_create(mock_rng *rng)
{
    audio_engine *engine = aheap_alloc(sizeof(*engine));
    if (engine == NULL)
        return NULL;
    memset(engine, 0, sizeof(*engine));
    engine->rng = rng;
    engine->state = ENGINE_OPEN;
    mock_global_log("engine create");
    if (device_open_with_retry(engine) != MOCK_OK) {
        /* Even a permanently failed open must leave a destroyable object. */
        engine->state = ENGINE_OPEN;
    }
    return engine;
}

void engine_destroy(audio_engine *engine)
{
    if (engine == NULL)
        return;
    if (engine->dev != NULL) {
        if (mock_is_running(engine->dev))
            mock_stop(engine->dev);
        mock_close(engine->dev);
        engine->dev = NULL;
    }
    mock_global_log("engine destroy (recovered=%u open_fail=%u)",
                    engine->counters.recovered,
                    engine->counters.open_failures_seen);
    engine->state = ENGINE_UNINIT;
    aheap_free(engine);
}

static void refill_if_needed(audio_engine *engine)
{
    float frames[DEVICE_CAPACITY];
    unsigned want;
    unsigned i;
    mock_status write_status;
    size_t accepted;

    if (engine->dev == NULL || !mock_is_running(engine->dev))
        return;
    if (mock_level(engine->dev) >= LOW_WATERMARK)
        return;

    want = DEVICE_CAPACITY - mock_level(engine->dev);
    for (i = 0; i < want; i++) {
        engine->phase++;
        /* Deterministic triangle wave; values stay within [-1, 1]. */
        frames[i] = (float)((int)(engine->phase % 64u) - 32) / 32.0f;
    }
    accepted = mock_write(engine->dev, frames, want, &write_status);
    /* Back-pressure short writes are normal flow control, not faults; only a
     * device-reported OVERRUN is counted by the tick fault handler. */
    (void)accepted;
    (void)write_status;
}

static mock_status handle_fault(audio_engine *engine, mock_status fault)
{
    if (fault == MOCK_OK) {
        engine->counters.ignored_ok++;
        return MOCK_OK;
    }

    switch (fault) {
    case MOCK_ERR_HOTPLUG:
        /* Device is gone: tear down and re-open from scratch. */
        mock_global_log("engine handling HOTPLUG: reopening device");
        if (engine->dev != NULL) {
            if (mock_is_running(engine->dev))
                mock_stop(engine->dev);
            mock_close(engine->dev);
            engine->dev = NULL;
        }
        engine->state = ENGINE_REOPENING;
        engine->open_attempts = 0;
        if (device_open_with_retry(engine) != MOCK_OK)
            return MOCK_ERR_OPEN;
        engine->counters.recovered_by_kind[MOCK_ERR_HOTPLUG]++;
        engine->counters.recovered++;
        return MOCK_OK;

    case MOCK_ERR_TIMEOUT:
        /* Skip this period; next tick resumes normally. No state change. */
        engine->counters.recovered_by_kind[MOCK_ERR_TIMEOUT]++;
        engine->counters.recovered++;
        return MOCK_OK;

    case MOCK_ERR_UNDERRUN:
        /* Ring survived: refill aggressively and keep streaming. */
        engine->underflow_gaps++;
        refill_if_needed(engine);
        engine->counters.recovered_by_kind[MOCK_ERR_UNDERRUN]++;
        engine->counters.recovered++;
        return MOCK_OK;

    case MOCK_ERR_OVERRUN:
        /* Ring was flushed by the device; just refill below. */
        refill_if_needed(engine);
        engine->counters.recovered_by_kind[MOCK_ERR_OVERRUN]++;
        engine->counters.recovered++;
        return MOCK_OK;

    default:
        mock_global_log("engine unexpected fault %d", (int)fault);
        return fault;
    }
}

mock_status engine_pump_half(audio_engine *engine)
{
    mock_status fault;

    if (engine == NULL)
        return MOCK_ERR_STATE;

    if (engine->dev == NULL) {
        engine->state = ENGINE_REOPENING;
        engine->open_attempts = 0;
        if (device_open_with_retry(engine) != MOCK_OK)
            return MOCK_ERR_OPEN;
    }

    refill_if_needed(engine);
    fault = mock_tick(engine->dev);
    return handle_fault(engine, fault);
}

engine_state engine_state_get(const audio_engine *engine)
{
    return engine == NULL ? ENGINE_UNINIT : engine->state;
}

const engine_counters *engine_counters_get(const audio_engine *engine)
{
    return engine == NULL ? NULL : &engine->counters;
}

unsigned engine_underflow_events(const audio_engine *engine)
{
    return engine == NULL ? 0 : engine->underflow_gaps;
}

const mock_device *engine_device_get(const audio_engine *engine)
{
    return engine == NULL ? NULL : engine->dev;
}

/* ====================================================================== */
/* Counterexample engine: same shape, three deliberately broken variants. */
/* ====================================================================== */

typedef enum { BAD_NONE = 0, BAD_LEAK, BAD_STUCK, BAD_CRASH } bad_flaw;

struct bad_engine {
    mock_device *dev;
    engine_state state;
    unsigned     phase;
    bad_flaw     flaw;
    float       *leaked;       /* leak variant: never freed */
    mock_rng    *rng;
};

audio_engine *bad_engine_create(mock_rng *rng, const char *flaw_name)
{
    struct bad_engine *be = aheap_alloc(sizeof(*be));
    bad_flaw flaw = BAD_NONE;

    if (be == NULL)
        return NULL;
    memset(be, 0, sizeof(*be));
    be->rng = rng;

    if (strcmp(flaw_name, "leak") == 0)
        flaw = BAD_LEAK;
    else if (strcmp(flaw_name, "stuck") == 0)
        flaw = BAD_STUCK;
    else if (strcmp(flaw_name, "crash") == 0)
        flaw = BAD_CRASH;

    be->flaw = flaw;
    be->dev = mock_open(DEVICE_CAPACITY, DEVICE_PERIOD, 0, rng);
    if (be->dev != NULL)
        mock_start(be->dev);
    be->state = be->dev != NULL ? ENGINE_STREAMING : ENGINE_OPEN;
    return (audio_engine *)be;
}

/* The bad engine reuses the public engine API only as a type alias; it
 * overrides pump/destroy locally. The counterexample driver calls these via
 * a small dispatch table below. */

mock_status bad_engine_pump_half(audio_engine *opaque)
{
    struct bad_engine *be = (struct bad_engine *)opaque;
    float fill[DEVICE_CAPACITY];
    mock_status st;
    unsigned i;

    if (be->dev == NULL)
        return MOCK_ERR_STATE;

    if (mock_level(be->dev) < LOW_WATERMARK) {
        unsigned want = DEVICE_CAPACITY - mock_level(be->dev);
        for (i = 0; i < want; i++)
            fill[i] = (float)((int)((be->phase += 1) % 64u) - 32) / 32.0f;
        mock_write(be->dev, fill, want, &st);
    }

    st = mock_tick(be->dev);
    if (st == MOCK_OK)
        return MOCK_OK;

    switch (be->flaw) {
    case BAD_LEAK:
        /* "Handle" every fault by allocating a scratch buffer and leaking
         * it on purpose. */
        be->leaked = aheap_alloc(128);
        mock_global_log("bad(leak): allocated scratch and dropped reference");
        return MOCK_OK;

    case BAD_STUCK:
        /* Hotplug leaves the dead device in place; the engine wedges. */
        be->state = ENGINE_REOPENING;
        mock_global_log("bad(stuck): wedged in REOPENING, device never closed");
        return st;

    case BAD_CRASH:
        mock_global_log("bad(crash): dereferencing NULL on fault");
        fflush(stdout);
        fflush(stderr);
        *(volatile int *)0 = 0; /* deliberate SIGSEGV */
        return st;

    default:
        return st;
    }
}

void bad_engine_destroy(audio_engine *opaque)
{
    struct bad_engine *be = (struct bad_engine *)opaque;
    if (be == NULL)
        return;
    /* leak variant intentionally omits freeing be->leaked */
    if (be->dev != NULL) {
        if (mock_is_running(be->dev))
            mock_stop(be->dev);
        mock_close(be->dev);
    }
    aheap_free(be);
}
