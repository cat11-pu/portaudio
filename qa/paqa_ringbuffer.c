/*
 * Functional tests for the batch SPSC ring buffer and its notifier hooks.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "pa_ringbuffer.h"

#define ELEMENT_COUNT 256u
#define TRANSFER_COUNT 1000000ull

#define CHECK( expression ) do { \
    if( !(expression) ) { \
        fprintf( stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression ); \
        abort(); \
    } \
} while( 0 )

static unsigned NotifyCount;

static void CountNotification( void *userData )
{
    (void)userData;
    NotifyCount++;
}

static int TestWraparound( void )
{
    PaUtilRingBuffer ring;
    uint64_t storage[16];
    uint64_t input[16];
    uint64_t output[16];
    unsigned i;

    CHECK( PaUtil_InitializeRingBuffer( &ring, sizeof(uint64_t), 16, storage ) == 0 );
    for( i = 0; i < 10; ++i )
        input[i] = i + 1;
    CHECK( PaUtil_WriteRingBuffer( &ring, input, 10 ) == 10 );
    CHECK( PaUtil_ReadRingBuffer( &ring, output, 3 ) == 3 );
    for( i = 0; i < 3; ++i )
        CHECK( output[i] == i + 1 );

    for( i = 0; i < 9; ++i )
        input[i] = 100 + i;
    CHECK( PaUtil_WriteRingBuffer( &ring, input, 9 ) == 9 );
    CHECK( PaUtil_ReadRingBuffer( &ring, output, 16 ) == 16 );
    for( i = 0; i < 7; ++i )
        CHECK( output[i] == i + 4 );
    for( i = 0; i < 9; ++i )
        CHECK( output[7 + i] == (uint64_t)(100 + i) );

    CHECK( PaUtil_GetRingBufferReadAvailable( &ring ) == 0 );
    return 0;
}

static int TestNotifiers( void )
{
    PaUtilRingBuffer ring;
    uint64_t storage[16];
    uint64_t data[16];
    uint64_t output[16];
    unsigned i;

    for( i = 0; i < 16; ++i )
        data[i] = i;

    CHECK( PaUtil_InitializeRingBuffer( &ring, sizeof(uint64_t), 16, storage ) == 0 );
    NotifyCount = 0;
    PaUtil_SetRingBufferDataAvailableNotifier( &ring, 4, CountNotification, NULL );
    CHECK( PaUtil_WriteRingBuffer( &ring, data, 3 ) == 3 );
    CHECK( NotifyCount == 0 );
    CHECK( PaUtil_WriteRingBuffer( &ring, data + 3, 2 ) == 2 );
    CHECK( NotifyCount == 1 );
    CHECK( PaUtil_WriteRingBuffer( &ring, data + 5, 5 ) == 5 );
    CHECK( NotifyCount == 1 );

    PaUtil_SetRingBufferDataAvailableNotifier( &ring, 0, NULL, NULL );
    PaUtil_FlushRingBuffer( &ring );
    NotifyCount = 0;
    PaUtil_SetRingBufferSpaceAvailableNotifier( &ring, 4, CountNotification, NULL );
    CHECK( PaUtil_WriteRingBuffer( &ring, data, 16 ) == 16 );
    CHECK( NotifyCount == 0 );
    CHECK( PaUtil_ReadRingBuffer( &ring, output, 3 ) == 3 );
    CHECK( NotifyCount == 0 );
    CHECK( PaUtil_ReadRingBuffer( &ring, output, 1 ) == 1 );
    CHECK( NotifyCount == 1 );
    CHECK( PaUtil_ReadRingBuffer( &ring, output, 1 ) == 1 );
    CHECK( NotifyCount == 2 );

    return 0;
}

typedef struct
{
    PaUtilRingBuffer ring;
    int failed;
} ThreadContext;

static void *WriteThread( void *arg )
{
    ThreadContext *context = (ThreadContext *)arg;
    uint64_t data[64];
    uint64_t sequence = 1;

    while( sequence <= TRANSFER_COUNT )
    {
        ring_buffer_size_t count = 1 + (ring_buffer_size_t)((sequence * 17u) % 64u);
        ring_buffer_size_t done = 0;

        if( count > (ring_buffer_size_t)(TRANSFER_COUNT - sequence + 1) )
            count = (ring_buffer_size_t)(TRANSFER_COUNT - sequence + 1);
        while( done < count )
        {
            ring_buffer_size_t chunk = count - done;
            ring_buffer_size_t written;
            ring_buffer_size_t i;
            if( chunk > 64 )
                chunk = 64;
            for( i = 0; i < chunk; ++i )
                data[i] = sequence + (uint64_t)done + (uint64_t)i;
            written = PaUtil_WriteRingBuffer( &context->ring, data, chunk );
            if( written > 0 )
                done += written;
            else
                { }
        }
        sequence += (uint64_t)count;
    }
    return NULL;
}

static void *ReadThread( void *arg )
{
    ThreadContext *context = (ThreadContext *)arg;
    uint64_t data[64];
    uint64_t expected = 1;

    while( expected <= TRANSFER_COUNT )
    {
        ring_buffer_size_t count = 1 + (ring_buffer_size_t)((expected * 29u) % 64u);
        ring_buffer_size_t done = 0;

        if( count > (ring_buffer_size_t)(TRANSFER_COUNT - expected + 1) )
            count = (ring_buffer_size_t)(TRANSFER_COUNT - expected + 1);
        while( done < count )
        {
            ring_buffer_size_t chunk = count - done;
            ring_buffer_size_t received;
            ring_buffer_size_t i;
            if( chunk > 64 )
                chunk = 64;
            received = PaUtil_ReadRingBuffer( &context->ring, data, chunk );
            for( i = 0; i < received; ++i )
            {
                if( data[i] != expected + (uint64_t)done + (uint64_t)i )
                    context->failed = 1;
            }
            done += received;
            if( received == 0 )
                { }
        }
        expected += (uint64_t)count;
    }
    return NULL;
}

static int TestConcurrentTransport( void )
{
    ThreadContext context;
    pthread_t writer;
    pthread_t reader;
    uint64_t *storage = (uint64_t *)calloc( ELEMENT_COUNT, sizeof(uint64_t) );

    CHECK( storage != NULL );
    memset( &context, 0, sizeof(context) );
    CHECK( PaUtil_InitializeRingBuffer( &context.ring, sizeof(uint64_t),
            ELEMENT_COUNT, storage ) == 0 );
    CHECK( pthread_create( &reader, NULL, ReadThread, &context ) == 0 );
    CHECK( pthread_create( &writer, NULL, WriteThread, &context ) == 0 );
    pthread_join( writer, NULL );
    pthread_join( reader, NULL );
    free( storage );
    return context.failed;
}

int main( void )
{
    if( offsetof(PaUtilRingBuffer, readIndex) - offsetof(PaUtilRingBuffer, writeIndex)
            < PA_RINGBUFFER_CACHE_LINE_SIZE )
    {
        fprintf( stderr, "ring buffer indices share a cache line\n" );
        return 1;
    }
    if( TestWraparound() != 0 || TestNotifiers() != 0 || TestConcurrentTransport() != 0 )
    {
        fprintf( stderr, "ring buffer test failed\n" );
        return 1;
    }
    puts( "ring buffer tests passed" );
    return 0;
}
