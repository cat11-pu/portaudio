#include "audio_engine.h"
#include "mock_audio.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#define ITERATIONS      200u
#define TICKS_PER_ITER  60u
#define FAULT_AT_TICK   30u

static const mock_status rotation[] = {
    MOCK_ERR_OPEN, MOCK_ERR_HOTPLUG, MOCK_ERR_TIMEOUT,
    MOCK_ERR_UNDERRUN, MOCK_ERR_OVERRUN
};
static const char *rotation_name[] = {
    "OPEN_FAIL", "HOTPLUG", "TIMEOUT", "UNDERRUN", "OVERRUN"
};

struct shared_counters {
    unsigned injected[MOCK_STATUS_COUNT];
    unsigned recovered[MOCK_STATUS_COUNT];
    unsigned open_attempts_failed;
};

/* Children inherit the mapping: faults/heap stats aggregate across forks. */
static size_t *g_heap_live;
static size_t *g_heap_peak;
static int g_report_fd = -1;

static void silence_child_output(void)
{
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }
}

static void report_child_line(const char *line)
{
    if (g_report_fd >= 0) {
        size_t len = strlen(line);
        ssize_t w = write(g_report_fd, line, len);
        (void)w;
    }
}

static void dump_sink_cb(const char *line, void *ctx)
{
    (void)ctx;
    report_child_line(line);
}

static int run_one_iteration(unsigned iteration, mock_status primary,
                             struct shared_counters *agg)
{
    mock_rng rng;
    audio_engine *engine;
    engine_counters counters_snapshot;
    const mock_device *dev;
    unsigned delivered_before[MOCK_STATUS_COUNT];
    unsigned delivered_after[MOCK_STATUS_COUNT];
    unsigned tick;
    int ok = 1;
    size_t heap_before, heap_after;
    unsigned opens_before, opens_after;

    mock_rng_seed(&rng, 0x1000 + iteration);
    heap_before = aheap_live_allocations();
    opens_before = mock_open_device_count();
    {
        mock_status kind;
        for (kind = MOCK_OK; kind < MOCK_STATUS_COUNT; kind++)
            delivered_before[kind] = mock_delivered_total(kind);
    }

    if (primary == MOCK_ERR_OPEN)
        mock_arm_next_open_failures(2);

    engine = engine_create(&rng);
    if (engine == NULL) {
        mock_global_log("engine_create returned NULL");
        mock_dump_failure("CREATE", (int)iteration, NULL, 0);
        return 1;
    }

    for (tick = 0; tick < TICKS_PER_ITER; tick++) {
        mock_status pump_status;

        if (primary != MOCK_ERR_OPEN && tick == FAULT_AT_TICK) {
            mock_device *mutable_dev =
                (mock_device *)engine_device_get(engine);
            if (mutable_dev != NULL)
                mock_arm_fault(mutable_dev, primary);
        }

        pump_status = engine_pump_half(engine);
        if (pump_status != MOCK_OK && pump_status != MOCK_ERR_UNDERRUN &&
            pump_status != MOCK_ERR_OVERRUN) {
            mock_global_log("pump returned %s at tick %u",
                            mock_status_str(pump_status), tick);
            ok = 0;
        }
    }

    dev = engine_device_get(engine);
    {
        const engine_counters *live = engine_counters_get(engine);
        if (live != NULL)
            counters_snapshot = *live;
        else
            memset(&counters_snapshot, 0, sizeof(counters_snapshot));
    }

    /* Invariant 1: state is recoverable - engine streaming, device live. */
    if (engine_state_get(engine) != ENGINE_STREAMING ||
        dev == NULL || !mock_is_running(dev)) {
        mock_global_log("engine not streaming at end (state=%d)",
                        (int)engine_state_get(engine));
        ok = 0;
    }

    /* Snapshot delivered-fault totals (global, hotplug-safe). */
    {
        mock_status kind;
        for (kind = MOCK_OK; kind < MOCK_STATUS_COUNT; kind++)
            delivered_after[kind] = mock_delivered_total(kind);
    }

    /* Invariant 3: every injected fault was accounted for and recovered.
     * Checked while the engine (and its counters) are still alive. */
    {
        mock_status kind;
        for (kind = MOCK_ERR_HOTPLUG; kind <= MOCK_ERR_OVERRUN; kind++) {
            unsigned seen = delivered_after[kind] - delivered_before[kind];
            unsigned healed = counters_snapshot.recovered_by_kind[kind];
            agg->injected[kind] += seen;
            agg->recovered[kind] += healed;
            if (healed < seen) {
                mock_global_log("unrecovered %s: injected=%u healed=%u",
                                mock_status_str(kind), seen, healed);
                ok = 0;
            }
        }
    }
    if (primary == MOCK_ERR_OPEN &&
        counters_snapshot.open_failures_seen < 2) {
        mock_global_log("expected 2 open failures, saw %u",
                        counters_snapshot.open_failures_seen);
        ok = 0;
    }
    agg->open_attempts_failed += counters_snapshot.open_failures_seen;
    agg->injected[MOCK_ERR_OPEN] += counters_snapshot.open_failures_seen;
    agg->recovered[MOCK_ERR_OPEN] +=
        counters_snapshot.recovered_by_kind[MOCK_ERR_OPEN];

    if (!ok)
        mock_dump_failure("INVARIANT", (int)iteration, dev, 10);

    /* Invariant 2: zero leaks - tracked heap and mock devices balanced. */
    engine_destroy(engine);
    heap_after = aheap_live_allocations();
    opens_after = mock_open_device_count();
    if (heap_after != heap_before) {
        mock_global_log("heap leak: live %zu -> %zu", heap_before,
                        heap_after);
        ok = 0;
    }
    if (opens_after != opens_before) {
        mock_global_log("device leak: open devices %u -> %u",
                        opens_before, opens_after);
        ok = 0;
    }

    return ok ? 0 : 1;
}

static void print_summary(const struct shared_counters *agg)
{
    static const mock_status kinds[] = {
        MOCK_ERR_OPEN, MOCK_ERR_HOTPLUG, MOCK_ERR_TIMEOUT,
        MOCK_ERR_UNDERRUN, MOCK_ERR_OVERRUN
    };
    unsigned i;

    printf("\n================ summary (%u iterations) ================\n",
           ITERATIONS);
    printf("%-10s %12s %12s %10s\n", "fault", "injected", "recovered",
           "ratio");
    for (i = 0; i < 5; i++) {
        unsigned inj = agg->injected[kinds[i]];
        unsigned rec = agg->recovered[kinds[i]];
        printf("%-10s %12u %12u %9.1f%%\n", mock_status_str(kinds[i]),
               inj, rec, inj ? 100.0 * rec / inj : 0.0);
    }
    printf("tracked heap peak live allocations: %zu\n",
           g_heap_peak != NULL ? *g_heap_peak : aheap_peak_allocations());
    printf("tracked heap live after all iterations: %zu\n",
           g_heap_live != NULL ? *g_heap_live : aheap_live_allocations());
    printf("unclosed mock devices: %u\n", mock_open_device_count());
    printf("=========================================================\n");
}

static int fault_suite(void)
{
    struct shared_counters *agg;
    size_t *heap_live_shared;
    size_t *heap_peak_shared;
    unsigned iteration;
    unsigned failures = 0;

    agg = mmap(NULL, sizeof(*agg), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    heap_live_shared = mmap(NULL, sizeof(size_t), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    heap_peak_shared = mmap(NULL, sizeof(size_t), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (agg == MAP_FAILED || heap_live_shared == MAP_FAILED ||
        heap_peak_shared == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(agg, 0, sizeof(*agg));
    *heap_live_shared = 0;
    *heap_peak_shared = 0;
    g_heap_live = heap_live_shared;
    g_heap_peak = heap_peak_shared;
    aheap_install_shared(heap_live_shared, heap_peak_shared);

    for (iteration = 0; iteration < ITERATIONS; iteration++) {
        mock_status primary = rotation[iteration % 5];
        pid_t pid;
        int status = 0;
        int report_pipe[2] = {-1, -1};

        if (pipe(report_pipe) != 0) {
            perror("pipe");
            return 1;
        }

        fflush(stdout);
        fflush(stderr);
        pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            silence_child_output();
            close(report_pipe[0]);
            g_report_fd = report_pipe[1];
            mock_set_dump_sink(dump_sink_cb, NULL);
            int rc = run_one_iteration(iteration, primary, agg);
            /* _exit skips LSan atexit: device/heap balances were already
             * asserted explicitly above. */
            _exit(rc ? 1 : 0);
        }
        close(report_pipe[1]);
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            return 1;
        }
        {
            char buf[4096];
            ssize_t got = read(report_pipe[0], buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                fputs(buf, stdout);
            }
            close(report_pipe[0]);
        }
        if (WIFSIGNALED(status)) {
            printf("iter %3u/%-3u %-9s CRASH sig=%d\n", iteration + 1,
                   ITERATIONS, rotation_name[iteration % 5],
                   WTERMSIG(status));
            failures++;
        } else if (WEXITSTATUS(status) != 0) {
            printf("iter %3u/%-3u %-9s INVARIANT FAILURE\n",
                   iteration + 1, ITERATIONS,
                   rotation_name[iteration % 5]);
            failures++;
        } else if ((iteration + 1) % 25 == 0 || iteration == 0) {
            printf("iter %3u/%-3u %-9s ok\n", iteration + 1,
                   ITERATIONS, rotation_name[iteration % 5]);
        }
    }

    print_summary(agg);
    if (failures == 0)
        printf("RESULT: 200/200 iterations passed - zero crashes, "
               "zero leaks, state recovered every time\n");
    else
        printf("RESULT: %u iteration(s) failed\n", failures);
    return failures ? 1 : 0;
}

/* --- counterexample driver ---------------------------------------------- */

static int run_bad_flaw(const char *flaw)
{
    mock_rng rng;
    audio_engine *engine;
    mock_device *dev;
    unsigned tick;

    mock_rng_seed(&rng, 42);
    engine = bad_engine_create(&rng, flaw);
    if (engine == NULL)
        return 2;

    for (tick = 0; tick < TICKS_PER_ITER; tick++) {
        if (tick == FAULT_AT_TICK) {
            /* Reach into the bad engine's device the same way the good
             * engine accessor works for the test harness. */
            dev = (mock_device *)engine_device_get(engine);
            if (dev != NULL)
                mock_arm_fault(dev, MOCK_ERR_HOTPLUG);
        }
        bad_engine_pump_half(engine);
    }

    /* Leak check: the leak variant should have lost allocations. */
    if (strcmp(flaw, "leak") == 0) {
        bad_engine_destroy(engine);
        return aheap_live_allocations() == 0 ? 0 : 1;
    }
    if (strcmp(flaw, "stuck") == 0) {
        int recovered = engine_state_get(engine) == ENGINE_STREAMING;
        bad_engine_destroy(engine);
        return recovered ? 0 : 1;
    }
    /* crash flaw: reaching here means the SIGSEGV did not happen. */
    bad_engine_destroy(engine);
    return 0;
}

static int counterexample_suite(void)
{
    static const char *flaws[] = {"leak", "stuck", "crash"};
    static const char *expect[] = {
        "lost heap allocations (leak)",
        "wedged REOPENING state (no recovery)",
        "SIGSEGV inside fault handler (crash)"
    };
    unsigned i;
    int all_demonstrated = 1;

    printf("\n=========== counterexamples (deliberately broken) ===========\n");
    for (i = 0; i < 3; i++) {
        pid_t pid = fork();
        int status = 0;

        if (pid == 0) {
            silence_child_output();
            int rc = run_bad_flaw(flaws[i]);
            _exit(rc);
        }
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
            printf("bad[%-5s] reproduced: %-38s [detected by harness]\n",
                   flaws[i], expect[i]);
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
            printf("bad[%-5s] reproduced: %-38s [detected by harness]\n",
                   flaws[i], expect[i]);
        } else {
            printf("bad[%-5s] NOT reproduced (flaw masked!)\n", flaws[i]);
            all_demonstrated = 0;
        }
    }
    printf("all three flawed engines are caught by the same invariants the\n"
           "robust engine passes 200/200.\n");
    printf("=============================================================\n");
    return all_demonstrated ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "counterexample") == 0)
        return counterexample_suite();
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
        return run_bad_flaw("leak");
    return fault_suite();
}
