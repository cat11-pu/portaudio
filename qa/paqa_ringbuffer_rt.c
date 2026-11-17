/*
 * Real-time ring buffer benchmark for 48 kHz stereo float audio blocks.
 *
 * The producer is callback-driven: the audio thread consumes one complete
 * block, which makes space available and wakes the producer through eventfd
 * without a mutex. This measures the path that matters at low latency:
 * batch copy, index publication, wakeup latency, and callback underruns.
 */

#if defined(__linux__)

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <pthread.h>

#include "pa_ringbuffer.h"

#define SAMPLE_RATE             48000
#define CHANNEL_COUNT           2
#define BYTES_PER_FRAME         ((ring_buffer_size_t)(CHANNEL_COUNT * sizeof(float)))
#define RING_BLOCKS             4
#define TARGET_BLOCKS           2
#define MAX_SAMPLES             20000
#define RUN_SECONDS             6.0
#define MIX_TIME_SECONDS        0.00025
#define MAX_DEADLINE_JITTER     0.00005

typedef struct
{
    PaUtilRingBuffer ring;
    unsigned blockFrames;
    unsigned ringCapacityFrames;
    int wakeFd;
    volatile int stop;
    volatile unsigned long long callbacks;
    volatile unsigned long long underruns;
    volatile unsigned long long overruns;
    volatile unsigned long long producerWakeups;
    double queueFrames[MAX_SAMPLES];
    double wakeMs[MAX_SAMPLES];
    double deadlineLateMs[MAX_SAMPLES];
    unsigned sampleCount;
    unsigned wakeCount;
} BenchContext;

static double NowSeconds( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void SleepUntil( double deadline )
{
    struct timespec ts;
    ts.tv_sec = (time_t)deadline;
    ts.tv_nsec = (long)((deadline - (double)ts.tv_sec) * 1e9);
    while( clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL ) == EINTR )
    {
    }
}

static void BusyUntil( double deadline )
{
    while( NowSeconds() < deadline )
    {
        __asm__ __volatile__( "" ::: "memory" );
    }
}

static uint32_t RandomState = 0x6d2b79f5u;

static uint32_t NextRandom( void )
{
    RandomState ^= RandomState << 13;
    RandomState ^= RandomState >> 17;
    RandomState ^= RandomState << 5;
    return RandomState;
}

static void WakeProducer( void *userData )
{
    BenchContext *ctx = (BenchContext *)userData;
    uint64_t one = 1;
    ssize_t ignored = write( ctx->wakeFd, &one, sizeof(one) );
    (void)ignored;
}

static void MakeBlock( float *block, unsigned blockFrames, unsigned long long sequence )
{
    unsigned frame;
    for( frame = 0; frame < blockFrames; ++frame )
    {
        uint64_t value = sequence * blockFrames + frame;
        block[frame * CHANNEL_COUNT] = (float)(value & 0xffffu);
        block[frame * CHANNEL_COUNT + 1] = (float)((value >> 16) & 0xffffu);
    }
}

static void *ProducerThread( void *arg )
{
    BenchContext *ctx = (BenchContext *)arg;
    float *block = (float *)malloc( (size_t)ctx->blockFrames * BYTES_PER_FRAME );
    unsigned long long sequence = 0;

    if( block == NULL )
        return NULL;

    while( !ctx->stop )
    {
        uint64_t wakeCount, blockIndex;
        double wakeStart, mixSeconds;

        if( read( ctx->wakeFd, &wakeCount, sizeof(wakeCount) ) != sizeof(wakeCount) )
        {
            if( errno == EINTR )
                continue;
            break;
        }
        if( ctx->stop )
            break;

        wakeStart = NowSeconds();
        mixSeconds = MIX_TIME_SECONDS;
        if( (sequence % 113u) == 0u )
            mixSeconds += 0.0008;
        BusyUntil( wakeStart + mixSeconds );

        /* eventfd coalesces notifications into a counter. Produce one block
           for every completed callback represented by that counter. */
        for( blockIndex = 0; blockIndex < wakeCount; ++blockIndex )
        {
            ring_buffer_size_t written;
            MakeBlock( block, ctx->blockFrames, sequence );
            written = PaUtil_WriteRingBuffer( &ctx->ring, block, ctx->blockFrames );
            if( written != (ring_buffer_size_t)ctx->blockFrames )
                ctx->overruns++;
            sequence++;
        }

        if( ctx->wakeCount < MAX_SAMPLES )
            ctx->wakeMs[ctx->wakeCount++] = (NowSeconds() - wakeStart) * 1000.0;
        ctx->producerWakeups += wakeCount;
    }

    free( block );
    return NULL;
}

static void *ConsumerThread( void *arg )
{
    BenchContext *ctx = (BenchContext *)arg;
    float *block = (float *)malloc( (size_t)ctx->blockFrames * BYTES_PER_FRAME );
    double period = (double)ctx->blockFrames / SAMPLE_RATE;
    double next = NowSeconds() + period;
    double stopTime = next + RUN_SECONDS;

    if( block == NULL )
        return NULL;

    while( NowSeconds() < stopTime )
    {
        double jitter = ((NextRandom() & 0xffffu) / 65535.0 - 0.5) * (2.0 * MAX_DEADLINE_JITTER);
        double scheduled = next + jitter;
        double actual;
        unsigned queued;
        ring_buffer_size_t readFrames;
        unsigned index = ctx->sampleCount;

        SleepUntil( scheduled );
        actual = NowSeconds();
        queued = (unsigned)PaUtil_GetRingBufferReadAvailable( &ctx->ring );
        readFrames = PaUtil_ReadRingBuffer( &ctx->ring, block, ctx->blockFrames );

        if( index < MAX_SAMPLES )
        {
            ctx->queueFrames[index] = (double)queued;
            ctx->deadlineLateMs[index] = (actual - scheduled) * 1000.0;
            ctx->sampleCount++;
        }
        ctx->callbacks++;

        if( readFrames != (ring_buffer_size_t)ctx->blockFrames )
        {
            ctx->underruns++;
            memset( block, 0, (size_t)ctx->blockFrames * BYTES_PER_FRAME );
        }

        next += period;
        if( next < NowSeconds() - period )
            next = NowSeconds();
    }

    free( block );
    return NULL;
}

static int CompareDouble( const void *a, const void *b )
{
    double left = *(const double *)a;
    double right = *(const double *)b;
    return (left > right) - (left < right);
}

static double Percentile( double *values, unsigned count, double percentile )
{
    unsigned index;
    if( count == 0 )
        return 0.0;
    qsort( values, count, sizeof(double), CompareDouble );
    index = (unsigned)(percentile * (count - 1));
    return values[index];
}

static int RunTier( unsigned blockFrames )
{
    BenchContext ctx;
    pthread_t producer, consumer;
    float *prefill = NULL;
    double queueAvg = 0.0;
    double queueP99;
    double queueMax;
    double wakeP50;
    double wakeP99;
    double lateP99;
    double e2eP99Ms;
    unsigned i;

    memset( &ctx, 0, sizeof(ctx) );
    ctx.blockFrames = blockFrames;
    ctx.ringCapacityFrames = blockFrames * RING_BLOCKS;
    ctx.sampleCount = 0;
    ctx.wakeCount = 0;

    ctx.ring.buffer = NULL;
    if( PaUtil_InitializeRingBuffer( &ctx.ring, BYTES_PER_FRAME,
            ctx.ringCapacityFrames, calloc( ctx.ringCapacityFrames, BYTES_PER_FRAME ) ) != 0 )
    {
        fprintf( stderr, "ring init failed\n" );
        return 1;
    }
    ctx.wakeFd = eventfd( 0, EFD_CLOEXEC );
    if( ctx.wakeFd < 0 )
    {
        fprintf( stderr, "eventfd failed: %s\n", strerror(errno) );
        return 1;
    }

    /* Prime two blocks, then keep two blocks queued. Every completed
       callback wakes the producer for exactly one replacement block. */
    prefill = (float *)calloc( (size_t)blockFrames * TARGET_BLOCKS, BYTES_PER_FRAME );
    if( prefill == NULL || PaUtil_WriteRingBuffer( &ctx.ring, prefill,
                (ring_buffer_size_t)blockFrames * TARGET_BLOCKS ) !=
                (ring_buffer_size_t)blockFrames * TARGET_BLOCKS )
    {
        fprintf( stderr, "prefill failed\n" );
        return 1;
    }
    free( prefill );

    PaUtil_SetRingBufferSpaceAvailableNotifier( &ctx.ring,
            blockFrames,
            WakeProducer, &ctx );

    if( pthread_create( &producer, NULL, ProducerThread, &ctx ) != 0
            || pthread_create( &consumer, NULL, ConsumerThread, &ctx ) != 0 )
    {
        fprintf( stderr, "thread create failed\n" );
        return 1;
    }

    pthread_join( consumer, NULL );
    ctx.stop = 1;
    WakeProducer( &ctx );
    pthread_join( producer, NULL );

    if( ctx.wakeCount > 0 )
    {
        wakeP50 = Percentile( ctx.wakeMs, ctx.wakeCount, 0.50 );
        wakeP99 = Percentile( ctx.wakeMs, ctx.wakeCount, 0.99 );
    }
    else
    {
        wakeP50 = wakeP99 = 0.0;
    }
    if( ctx.sampleCount > 0 )
    {
        for( i = 0; i < ctx.sampleCount; ++i )
            queueAvg += ctx.queueFrames[i];
        queueAvg /= ctx.sampleCount;
        queueP99 = Percentile( ctx.queueFrames, ctx.sampleCount, 0.99 );
        queueMax = Percentile( ctx.queueFrames, ctx.sampleCount, 1.0 );
        lateP99 = Percentile( ctx.deadlineLateMs, ctx.sampleCount, 0.99 );
    }
    else
    {
        queueP99 = queueMax = lateP99 = 0.0;
    }

    e2eP99Ms = ((double)blockFrames + queueP99) / SAMPLE_RATE * 1000.0;

    printf( "%3u %6u %6u %8llu %8llu %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f\n",
            blockFrames,
            ctx.ringCapacityFrames,
            TARGET_BLOCKS,
            ctx.underruns,
            ctx.callbacks,
            queueAvg / SAMPLE_RATE * 1000.0,
            queueP99 / SAMPLE_RATE * 1000.0,
            queueMax / SAMPLE_RATE * 1000.0,
            e2eP99Ms,
            wakeP50,
            wakeP99,
            lateP99 );

    close( ctx.wakeFd );
    free( ctx.ring.buffer );
    return 0;
}

int main( void )
{
    printf( "block capacity target_blocks underruns callbacks queue_avg_ms queue_p99_ms queue_max_ms e2e_p99_ms wake_p50_ms wake_p99_ms deadline_late_p99_ms\n" );
    if( RunTier( 128 ) != 0 )
        return 1;
    if( RunTier( 256 ) != 0 )
        return 1;
    if( RunTier( 512 ) != 0 )
        return 1;
    return 0;
}

#else

#include <stdio.h>

int main( void )
{
    printf( "paqa_ringbuffer_rt: Linux eventfd benchmark skipped on this platform\n" );
    return 0;
}

#endif
