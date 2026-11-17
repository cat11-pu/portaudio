#ifndef MOCK_AUDIO_H
#define MOCK_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Deterministic mock of a game audio output device.
 *
 * The device owns a fixed-capacity ring buffer. The "audio thread" is
 * simulated by mock_tick(): one call equals one callback period. Frames are
 * pushed in by the engine with mock_write(). Five real-world failure classes
 * can be injected either as one-shot armed faults or via an RNG-driven mask:
 *
 *   MOCK_ERR_OPEN     - OpenStream fails (transient, retryable)
 *   MOCK_ERR_HOTPLUG  - device vanished while streaming
 *   MOCK_ERR_TIMEOUT  - callback missed its deadline
 *   MOCK_ERR_UNDERRUN - buffer starvation (audible gap)
 *   MOCK_ERR_OVERRUN  - buffer full, frames dropped
 */

typedef enum {
    MOCK_OK = 0,
    MOCK_ERR_OPEN,
    MOCK_ERR_HOTPLUG,
    MOCK_ERR_TIMEOUT,
    MOCK_ERR_UNDERRUN,
    MOCK_ERR_OVERRUN,
    MOCK_ERR_STATE,
    MOCK_ERR_NOMEM
} mock_status;

#define FAULT_OPEN     (1u << 0)
#define FAULT_HOTPLUG  (1u << 1)
#define FAULT_TIMEOUT  (1u << 2)
#define FAULT_UNDERRUN (1u << 3)
#define FAULT_OVERRUN  (1u << 4)
#define FAULT_ALL      (FAULT_OPEN | FAULT_HOTPLUG | FAULT_TIMEOUT | \
                        FAULT_UNDERRUN | FAULT_OVERRUN)

#define MOCK_STATUS_COUNT 8

typedef struct mock_device mock_device;

typedef struct {
    uint64_t state;
} mock_rng;

void     mock_rng_seed(mock_rng *rng, uint64_t seed);
uint32_t mock_rng_next(mock_rng *rng);

/* Arm N transient open failures before the next successful mock_open(). */
void mock_arm_next_open_failures(unsigned fail_times);

/* Create/destroy the device. Returns NULL on an armed open failure. */
mock_device *mock_open(unsigned capacity_frames, unsigned period_frames,
                       unsigned random_fault_mask, mock_rng *rng);
void         mock_close(mock_device *dev);

mock_status mock_start(mock_device *dev);
mock_status mock_stop(mock_device *dev);

/* Advance one callback period. */
mock_status mock_tick(mock_device *dev);

/* Push frames; returns the number accepted. *status is MOCK_ERR_OVERRUN when
 * some frames had to be dropped. */
size_t mock_write(mock_device *dev, const float *frames, size_t count,
                  mock_status *status);

/* Arm a one-shot fault delivered by the next mock_tick(). */
void mock_arm_fault(mock_device *dev, mock_status fault);

/* Cumulative faults actually delivered by ticks (survives hotplug only as
 * far as the same device instance is concerned). */
unsigned mock_delivered_total(mock_status fault);

/* Inspection. */
int           mock_is_running(const mock_device *dev);
unsigned      mock_level(const mock_device *dev);
unsigned      mock_total_consumed(const mock_device *dev);
unsigned      mock_tick_number(const mock_device *dev);
unsigned      mock_injected(const mock_device *dev, mock_status fault);
unsigned      mock_open_failure_count(void);
unsigned      mock_open_device_count(void);
const char   *mock_status_str(mock_status status);

/* Minimal operation sequence, used to print a repro trace on failure. */
void mock_log(mock_device *dev, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void mock_global_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void mock_dump_failure(const char *tag, int iteration,
                       const mock_device *dev, unsigned tail_lines);

/* When set, dump output is redirected here (used by forked test children to
 * stream their minimal operation sequence back to the parent). */
void mock_set_dump_sink(void (*sink)(const char *line, void *ctx),
                        void *ctx);

/* Tracked heap: every engine allocation is visible in-process, so leaks are
 * asserted directly instead of relying solely on an external sanitizer. */
void  *aheap_alloc(size_t size);
void   aheap_free(void *ptr);
size_t aheap_live_allocations(void);
size_t aheap_peak_allocations(void);

/* Stats live in MAP_SHARED memory so forked test iterations aggregate. */
void   aheap_install_shared(size_t *shared_live, size_t *shared_peak);

#endif /* MOCK_AUDIO_H */
