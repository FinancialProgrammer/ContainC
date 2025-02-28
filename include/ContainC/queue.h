#ifndef QUEUE_H
#define QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Forward declarations
typedef struct queue_s queue_t;
typedef struct queue_item_s queue_item_t;
typedef struct queue_allocator_s queue_allocator_t;
typedef struct queue_buffer_s queue_buffer_t;

// Allocator interface
struct queue_allocator_s {
  void* (*alloc)(size_t size, void* user_data);
  void (*free)(void* ptr, void* user_data);
  void* user_data;
};

// Default allocator (uses malloc/free)
extern queue_allocator_t queue_default_allocator;

// Queue item structure
struct queue_item_s {
  void* data;
  queue_item_t* next;
};

// Thread-local buffer for producers
struct queue_buffer_s {
  queue_item_t* head;
  queue_item_t* tail;
  size_t count;
  size_t max_size;
  pthread_t owner_thread;
  queue_buffer_t* next;
};

// Queue structure
struct queue_s {
  queue_item_t* head;
  queue_item_t* tail;
  size_t count;
  
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
  
  size_t max_size;        // 0 means unlimited
  bool consumers_waiting;
  
  queue_allocator_t allocator;
  queue_buffer_t* buffers;
  pthread_key_t tls_key;  // Thread-local storage key
  
  size_t buffer_flush_threshold;
  volatile bool shutdown;
};

// Queue creation and destruction
queue_t* queue_create(size_t max_size, queue_allocator_t* allocator);
void queue_destroy(queue_t* queue);

// Basic operations
bool queue_put(queue_t* queue, void* data);
void* queue_get(queue_t* queue, int64_t timeout_ms); // -1 for infinite wait
bool queue_try_get(queue_t* queue, void** data);

// Thread-local buffer configuration
void queue_set_buffer_size(queue_t* queue, size_t buffer_size);
void queue_flush_buffer(queue_t* queue);

// Utility functions
size_t queue_size(queue_t* queue);
void queue_shutdown(queue_t* queue);

// --------- OPTIMIZED MEMORY ALLOCATORS ---------
/*
// Pool Allocator - efficient for fixed-size allocations
queue_allocator_t queue_create_pool_allocator(size_t item_size, size_t blocks_per_chunk, size_t max_chunks);
void queue_destroy_pool_allocator(queue_allocator_t* allocator);

// Thread-Local Allocator - reduces contention in multi-threaded environments
queue_allocator_t queue_create_tls_allocator(size_t item_size, size_t batch_size, queue_allocator_t* base_allocator);
void queue_destroy_tls_allocator(queue_allocator_t* allocator);

// Aligned Allocator - ensures memory alignment for SIMD operations
queue_allocator_t queue_create_aligned_allocator(size_t alignment, queue_allocator_t* base_allocator);
void queue_destroy_aligned_allocator(queue_allocator_t* allocator);

// Convenience functions for common use cases
queue_allocator_t queue_create_item_pool(size_t estimated_queue_size);
queue_allocator_t queue_create_buffer_pool(void);
*/

#ifdef __cplusplus
}
#endif

#endif // QUEUE_H