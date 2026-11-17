#include "mock_audio.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_LOG_CAP 16
#define GLOBAL_LOG_CAP 12
#define AHEAP_GUARD 0xA5A51234u

struct mock_device {
    float   *buf;
    unsigned capacity;
    unsigned period;
    unsigned head;
    unsigned tail;
    unsigned level;
    int      running;
    unsigned tick_no;
    unsigned consumed_total;
    mock_status armed_fault;
    unsigned rng_mask;
    mock_rng *rng;
    unsigned injected[MOCK_STATUS_COUNT];
    char     log[DEV_LOG_CAP][96];
    unsigned log_count;
};

static unsigned global_open_failures;
static unsigned global_open_devices;
static unsigned pending_open_failures;

static char global_log_buf[GLOBAL_LOG_CAP][128];
static unsigned global_log_count;
static void (*dump_sink)(const char *line, void *ctx);
static void *dump_sink_ctx;

static size_t heap_live;
static size_t heap_peak;
static size_t *shared_live_ptr;
static size_t *shared_peak_ptr;
static unsigned global_delivered[MOCK_STATUS_COUNT];

static const char *status_names[MOCK_STATUS_COUNT] = {
    "OK", "OPEN_FAIL", "HOTPLUG", "TIMEOUT", "UNDERRUN", "OVERRUN",
    "BAD_STATE", "NO_MEM"
};

const char *mock_status_str(mock_status status)
{
    if ((unsigned)status >= MOCK_STATUS_COUNT)
        return "UNKNOWN";
    return status_names[(unsigned)status];
}

void mock_rng_seed(mock_rng *rng, uint64_t seed)
{
    rng->state = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

uint32_t mock_rng_next(mock_rng *rng)
{
    uint64_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng->state = x;
    return (uint32_t)(x >> 32);
}

void mock_arm_next_open_failures(unsigned fail_times)
{
    pending_open_failures += fail_times;
    mock_global_log("arm %u open failure(s)", fail_times);
}

unsigned mock_open_failure_count(void)
{
    return global_open_failures;
}

unsigned mock_open_device_count(void)
{
    return global_open_devices;
}

void mock_log(mock_device *dev, const char *fmt, ...)
{
    va_list ap;
    char line[96];
    unsigned idx;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (dev == NULL)
        return;
    idx = dev->log_count % DEV_LOG_CAP;
    snprintf(dev->log[idx], sizeof(dev->log[idx]), "%s", line);
    dev->log_count++;
}

void mock_global_log(const char *fmt, ...)
{
    va_list ap;
    unsigned idx = global_log_count % GLOBAL_LOG_CAP;
    va_start(ap, fmt);
    vsnprintf(global_log_buf[idx], sizeof(global_log_buf[idx]), fmt, ap);
    va_end(ap);
    global_log_count++;
}

void mock_set_dump_sink(void (*sink)(const char *line, void *ctx),
                        void *ctx)
{
    dump_sink = sink;
    dump_sink_ctx = ctx;
}

static void emit_dump(const char *line)
{
    if (dump_sink != NULL)
        dump_sink(line, dump_sink_ctx);
    else
        fputs(line, stderr);
}

void mock_dump_failure(const char *tag, int iteration,
                       const mock_device *dev, unsigned tail_lines)
{
    unsigned i;
    unsigned start;
    char line[160];

    snprintf(line, sizeof(line),
             "\n[FAIL %s iter=%d] minimal operation sequence "
             "(oldest -> newest):\n", tag, iteration);
    emit_dump(line);

    start = global_log_count > GLOBAL_LOG_CAP
                ? global_log_count - GLOBAL_LOG_CAP : 0;
    for (i = start; i < global_log_count; i++) {
        snprintf(line, sizeof(line), "  g: %s\n",
                 global_log_buf[i % GLOBAL_LOG_CAP]);
        emit_dump(line);
    }

    if (dev != NULL) {
        unsigned cap = dev->log_count > DEV_LOG_CAP ? DEV_LOG_CAP
                                                    : dev->log_count;
        start = dev->log_count > cap ? dev->log_count - cap : 0;
        if (tail_lines && tail_lines < cap)
            start = dev->log_count - tail_lines;
        for (i = start; i < dev->log_count; i++) {
            snprintf(line, sizeof(line), "  d: %s\n",
                     dev->log[i % DEV_LOG_CAP]);
            emit_dump(line);
        }
    }
}

mock_device *mock_open(unsigned capacity_frames, unsigned period_frames,
                       unsigned random_fault_mask, mock_rng *rng)
{
    mock_device *dev;

    if (pending_open_failures) {
        pending_open_failures--;
        global_open_failures++;
        global_delivered[MOCK_ERR_OPEN]++;
        mock_global_log("open -> MOCK_ERR_OPEN (transient)");
        return NULL;
    }

    dev = calloc(1, sizeof(*dev));
    if (dev == NULL)
        return NULL;
    dev->buf = malloc(sizeof(float) * capacity_frames);
    if (dev->buf == NULL) {
        free(dev);
        return NULL;
    }
    dev->capacity = capacity_frames;
    dev->period = period_frames;
    dev->armed_fault = MOCK_OK;
    dev->rng_mask = random_fault_mask;
    dev->rng = rng;
    mock_global_log("open -> OK cap=%u period=%u", capacity_frames,
                    period_frames);
    mock_log(dev, "open(cap=%u, period=%u)", capacity_frames, period_frames);
    global_open_devices++;
    return dev;
}

void mock_close(mock_device *dev)
{
    if (dev == NULL)
        return;
    mock_global_log("close");
    mock_log(dev, "close(level=%u, ticks=%u)", dev->level, dev->tick_no);
    if (global_open_devices)
        global_open_devices--;
    free(dev->buf);
    free(dev);
}

mock_status mock_start(mock_device *dev)
{
    if (dev == NULL)
        return MOCK_ERR_STATE;
    if (dev->running)
        return MOCK_ERR_STATE;
    dev->running = 1;
    mock_log(dev, "start");
    return MOCK_OK;
}

mock_status mock_stop(mock_device *dev)
{
    if (dev == NULL)
        return MOCK_ERR_STATE;
    if (!dev->running)
        return MOCK_ERR_STATE;
    dev->running = 0;
    mock_log(dev, "stop(level=%u)", dev->level);
    return MOCK_OK;
}

static mock_status pick_random_fault(mock_device *dev)
{
    static const mock_status candidates[] = {
        MOCK_ERR_HOTPLUG, MOCK_ERR_TIMEOUT, MOCK_ERR_UNDERRUN,
        MOCK_ERR_OVERRUN
    };
    static const unsigned bits[] = {
        FAULT_HOTPLUG, FAULT_TIMEOUT, FAULT_UNDERRUN, FAULT_OVERRUN
    };
    unsigned i;

    if (!dev->rng_mask || dev->rng == NULL)
        return MOCK_OK;
    for (i = 0; i < 4; i++) {
        if ((dev->rng_mask & bits[i]) &&
            (mock_rng_next(dev->rng) % 100u) < 6u)
            return candidates[i];
        }
    return MOCK_OK;
}

mock_status mock_tick(mock_device *dev)
{
    mock_status fault;
    unsigned consume;

    if (dev == NULL)
        return MOCK_ERR_STATE;

    dev->tick_no++;

    /* One-shot armed fault has priority over random ones. */
    fault = dev->armed_fault;
    dev->armed_fault = MOCK_OK;
    if (fault == MOCK_OK)
        fault = pick_random_fault(dev);

    switch (fault) {
    case MOCK_ERR_HOTPLUG:
        dev->injected[fault]++;
        global_delivered[fault]++;
        dev->running = 0;
        mock_log(dev, "tick#%u -> HOTPLUG (device gone)", dev->tick_no);
        return MOCK_ERR_HOTPLUG;

    case MOCK_ERR_TIMEOUT:
        dev->injected[fault]++;
        global_delivered[fault]++;
        mock_log(dev, "tick#%u -> TIMEOUT (deadline missed)",
                 dev->tick_no);
        return MOCK_ERR_TIMEOUT;

    case MOCK_ERR_UNDERRUN:
        /* Simulate a dry read: the device consumes nothing and reports a
         * gap. Buffer stays intact so the engine can refill it. */
        dev->injected[fault]++;
        global_delivered[fault]++;
        mock_log(dev, "tick#%u -> UNDERRUN (level=%u)", dev->tick_no,
                 dev->level);
        return MOCK_ERR_UNDERRUN;

    case MOCK_ERR_OVERRUN:
        /* Device-side overrun: it flushes the ring to recover itself. */
        dev->injected[fault]++;
        global_delivered[fault]++;
        dev->head = 0;
        dev->tail = 0;
        dev->level = 0;
        mock_log(dev, "tick#%u -> OVERRUN (ring flushed)", dev->tick_no);
        return MOCK_ERR_OVERRUN;

    default:
        break;
    }

    if (!dev->running)
        return MOCK_ERR_STATE;

    consume = dev->level < dev->period ? dev->level : dev->period;
    dev->head = (dev->head + consume) % dev->capacity;
    dev->level -= consume;
    dev->consumed_total += consume;
    if (consume < dev->period) {
        dev->injected[MOCK_ERR_UNDERRUN]++;
        global_delivered[MOCK_ERR_UNDERRUN]++;
        mock_log(dev, "tick#%u -> natural UNDERRUN consumed=%u/%u",
                 dev->tick_no, consume, dev->period);
        return MOCK_ERR_UNDERRUN;
    }
    return MOCK_OK;
}

size_t mock_write(mock_device *dev, const float *frames, size_t count,
                  mock_status *status)
{
    size_t accepted = 0;
    size_t i;

    if (status != NULL)
        *status = MOCK_OK;
    if (dev == NULL || !dev->running)
        return 0;

    for (i = 0; i < count; i++) {
        if (dev->level >= dev->capacity) {
            dev->injected[MOCK_ERR_OVERRUN]++;
            global_delivered[MOCK_ERR_OVERRUN]++;
            if (status != NULL)
                *status = MOCK_ERR_OVERRUN;
            mock_log(dev, "write -> OVERRUN after %zu frames (tick#%u)",
                     accepted, dev->tick_no);
            break;
        }
        dev->buf[dev->tail] = frames[i];
        dev->tail = (dev->tail + 1u) % dev->capacity;
        dev->level++;
        accepted++;
    }
    return accepted;
}

void mock_arm_fault(mock_device *dev, mock_status fault)
{
    if (dev == NULL)
        return;
    dev->armed_fault = fault;
    mock_log(dev, "arm-fault %s -> next tick", mock_status_str(fault));
}

unsigned mock_delivered_total(mock_status fault)
{
    if ((unsigned)fault >= MOCK_STATUS_COUNT)
        return 0;
    return global_delivered[fault];
}

int mock_is_running(const mock_device *dev)
{
    return dev != NULL && dev->running;
}

unsigned mock_level(const mock_device *dev)
{
    return dev->level;
}

unsigned mock_total_consumed(const mock_device *dev)
{
    return dev->consumed_total;
}

unsigned mock_tick_number(const mock_device *dev)
{
    return dev->tick_no;
}

unsigned mock_injected(const mock_device *dev, mock_status fault)
{
    if (dev == NULL || (unsigned)fault >= MOCK_STATUS_COUNT)
        return 0;
    return dev->injected[(unsigned)fault];
}

/* --- tracked heap -------------------------------------------------------- */

typedef struct {
    size_t size;
    unsigned guard;
} aheap_header;

void *aheap_alloc(size_t size)
{
    aheap_header *hdr;
    unsigned char *raw;

    if (size == 0)
        size = 1;
    raw = malloc(sizeof(aheap_header) + size);
    if (raw == NULL)
        return NULL;
    hdr = (aheap_header *)raw;
    hdr->size = size;
    hdr->guard = AHEAP_GUARD;
    heap_live++;
    if (heap_live > heap_peak)
        heap_peak = heap_live;
    if (shared_live_ptr != NULL) {
        *shared_live_ptr = heap_live;
        if (heap_peak > *shared_peak_ptr)
            *shared_peak_ptr = heap_peak;
    }
    return raw + sizeof(aheap_header);
}

void aheap_free(void *ptr)
{
    unsigned char *raw;
    aheap_header *hdr;

    if (ptr == NULL)
        return;
    raw = (unsigned char *)ptr - sizeof(aheap_header);
    hdr = (aheap_header *)raw;
    if (hdr->guard != AHEAP_GUARD) {
        mock_global_log("FATAL aheap corruption detected");
        abort();
    }
    hdr->guard = 0;
    if (heap_live == 0) {
        mock_global_log("FATAL aheap double-free detected");
        abort();
    }
    heap_live--;
    if (shared_live_ptr != NULL)
        *shared_live_ptr = heap_live;
    free(raw);
}

size_t aheap_live_allocations(void)
{
    return heap_live;
}

size_t aheap_peak_allocations(void)
{
    return shared_peak_ptr != NULL ? *shared_peak_ptr : heap_peak;
}

void aheap_install_shared(size_t *shared_live, size_t *shared_peak)
{
    shared_live_ptr = shared_live;
    shared_peak_ptr = shared_peak;
    heap_live = *shared_live;
    heap_peak = *shared_peak;
}
