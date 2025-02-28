#include "ContainC/queue.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

// Default allocator implementation
static void* default_alloc(size_t size, void* user_data) {
  (void)user_data; // Unused
  return malloc(size);
}

static void default_free(void* ptr, void* user_data) {
  (void)user_data; // Unused
  free(ptr);
}

queue_allocator_t queue_default_allocator = {
  .alloc = default_alloc,
  .free = default_free,
  .user_data = NULL
};

// Thread-local buffer management
static queue_buffer_t* queue_get_thread_buffer(queue_t* queue) {
  queue_buffer_t* buffer = pthread_getspecific(queue->tls_key);
  
  if (!buffer) {
    // Create new buffer for this thread
    buffer = queue->allocator.alloc(sizeof(queue_buffer_t), queue->allocator.user_data);
    if (!buffer) return NULL;
    
    buffer->head = NULL;
    buffer->tail = NULL;
    buffer->count = 0;
    buffer->max_size = queue->buffer_flush_threshold;
    buffer->owner_thread = pthread_self();
    buffer->next = NULL;
    
    pthread_setspecific(queue->tls_key, buffer);
    
    // Add to queue's buffer list
    pthread_mutex_lock(&queue->mutex);
    buffer->next = queue->buffers;
    queue->buffers = buffer;
    pthread_mutex_unlock(&queue->mutex);
  }
  
  return buffer;
}

static void queue_buffer_destructor(void* ptr) {
  // This function is called when a thread exits
  // The buffer will be handled by the queue during shutdown
  (void)ptr; // Buffer is managed in the queue's buffer list
}

// Queue creation
queue_t* queue_create(size_t max_size, queue_allocator_t* allocator) {
  queue_t* queue = NULL;
  
  // Use provided allocator or default
  queue_allocator_t alloc = allocator ? *allocator : queue_default_allocator;
  
  queue = alloc.alloc(sizeof(queue_t), alloc.user_data);
  if (!queue) return NULL;
  
  queue->head = NULL;
  queue->tail = NULL;
  queue->count = 0;
  queue->max_size = max_size;
  queue->consumers_waiting = false;
  queue->allocator = alloc;
  queue->buffers = NULL;
  queue->buffer_flush_threshold = 32; // Default buffer size
  queue->shutdown = false;
  
  if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
    queue->allocator.free(queue, queue->allocator.user_data);
    return NULL;
  }
  
  if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
    pthread_mutex_destroy(&queue->mutex);
    queue->allocator.free(queue, queue->allocator.user_data);
    return NULL;
  }
  
  if (pthread_cond_init(&queue->not_full, NULL) != 0) {
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    queue->allocator.free(queue, queue->allocator.user_data);
    return NULL;
  }
  
  if (pthread_key_create(&queue->tls_key, queue_buffer_destructor) != 0) {
    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    queue->allocator.free(queue, queue->allocator.user_data);
    return NULL;
  }
  
  return queue;
}

// Queue destruction
void queue_destroy(queue_t* queue) {
  if (!queue) return;
  
  // Signal shutdown
  queue_shutdown(queue);
  
  // Flush all thread-local buffers
  queue_buffer_t* buffer = queue->buffers;
  while (buffer) {
    queue_buffer_t* next = buffer->next;
    
    // Free items in buffer
    queue_item_t* item = buffer->head;
    while (item) {
      queue_item_t* next_item = item->next;
      queue->allocator.free(item, queue->allocator.user_data);
      item = next_item;
    }
    
    queue->allocator.free(buffer, queue->allocator.user_data);
    buffer = next;
  }
  
  // Free queue items
  queue_item_t* item = queue->head;
  while (item) {
    queue_item_t* next = item->next;
    queue->allocator.free(item, queue->allocator.user_data);
    item = next;
  }
  
  pthread_key_delete(queue->tls_key);
  pthread_cond_destroy(&queue->not_full);
  pthread_cond_destroy(&queue->not_empty);
  pthread_mutex_destroy(&queue->mutex);
  
  queue->allocator.free(queue, queue->allocator.user_data);
}

// Add all items from thread-local buffer to the main queue
static void queue_flush_buffer_internal(queue_t* queue, queue_buffer_t* buffer) {
  if (!buffer || buffer->count == 0) return;
  
  // Move items from buffer to queue
  if (!queue->head) {
    queue->head = buffer->head;
    queue->tail = buffer->tail;
  } else {
    queue->tail->next = buffer->head;
    queue->tail = buffer->tail;
  }
  
  queue->count += buffer->count;
  
  // Signal waiting consumers
  if (queue->consumers_waiting) {
    pthread_cond_broadcast(&queue->not_empty);  // Wake ALL waiting consumers
  }
  
  // Reset buffer
  buffer->head = NULL;
  buffer->tail = NULL;
  buffer->count = 0;
}

// Public flush buffer function
void queue_flush_buffer(queue_t* queue) {
  if (!queue) return;
  
  queue_buffer_t* buffer = pthread_getspecific(queue->tls_key);
  if (!buffer || buffer->count == 0) return;
  
  pthread_mutex_lock(&queue->mutex);
  queue_flush_buffer_internal(queue, buffer);
  pthread_mutex_unlock(&queue->mutex);
}

// Add an item to the queue
bool queue_put(queue_t* queue, void* data) {
  if (!queue || queue->shutdown) return false;
  
  // Get thread-local buffer
  queue_buffer_t* buffer = queue_get_thread_buffer(queue);
  if (!buffer) return false;
  
  // Create new item
  queue_item_t* item = queue->allocator.alloc(sizeof(queue_item_t), queue->allocator.user_data);
  if (!item) return false;
  
  item->data = data;
  item->next = NULL;
  
  // Add to thread-local buffer
  if (!buffer->head) {
    buffer->head = item;
    buffer->tail = item;
  } else {
    buffer->tail->next = item;
    buffer->tail = item;
  }
  
  buffer->count++;
  
  // Check if buffer should be flushed
  bool should_flush = buffer->count >= buffer->max_size;
  bool consumers_waiting = false;
  
  // Quick check without locking to see if consumers are waiting
  consumers_waiting = queue->consumers_waiting;
  
  if (should_flush || consumers_waiting) {
    pthread_mutex_lock(&queue->mutex);
    
    // Recheck if consumers are waiting after acquiring lock
    should_flush = should_flush || queue->consumers_waiting;
    
    if (should_flush) {
      queue_flush_buffer_internal(queue, buffer);
    }
    
    pthread_mutex_unlock(&queue->mutex);
  }
  
  return true;
}

// Get an item from the queue with timeout
void* queue_get(queue_t* queue, int64_t timeout_ms) {
  if (!queue) return NULL;
  
  void* result = NULL;
  struct timespec ts;
  
  pthread_mutex_lock(&queue->mutex);
  
  // Wait until there's an item or shutdown
  while (queue->count == 0 && !queue->shutdown) {
    // Signal we're waiting
    queue->consumers_waiting = true;
    
    if (timeout_ms < 0) {
      // Wait indefinitely
      pthread_cond_wait(&queue->not_empty, &queue->mutex);
    } else if (timeout_ms == 0) {
      // Don't wait at all
      break;
    } else {
      // Wait with timeout
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += timeout_ms / 1000;
      ts.tv_nsec += (timeout_ms % 1000) * 1000000;
      
      // Handle nsec overflow
      if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
      }
      
      int rv = pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &ts);
      if (rv == ETIMEDOUT) {
        break;
      }
    }
  }
  
  // Get an item if available
  if (queue->count > 0) {
    queue_item_t* item = queue->head;
    queue->head = item->next;
    
    if (!queue->head) {
      queue->tail = NULL;
    }
    
    result = item->data;
    queue->count--;
    
    queue->allocator.free(item, queue->allocator.user_data);
    
    // Signal producers if queue was full
    if (queue->max_size > 0 && queue->count == queue->max_size - 1) {
      pthread_cond_signal(&queue->not_full);
    }
  }
  
  // Only reset waiting flag if we have no more consumers waiting
  // This requires a more complex tracking system, but for now,
  // we'll set it to false and let other consumers set it back if needed
  queue->consumers_waiting = false;
  
  pthread_mutex_unlock(&queue->mutex);
  return result;
}

// Try to get an item without blocking
bool queue_try_get(queue_t* queue, void** data) {
  if (!queue || !data) return false;
  *data = NULL;
  
  pthread_mutex_lock(&queue->mutex);
  
  if (queue->count > 0) {
    queue_item_t* item = queue->head;
    queue->head = item->next;
    
    if (!queue->head) {
      queue->tail = NULL;
    }
    
    *data = item->data;
    queue->count--;
    
    queue->allocator.free(item, queue->allocator.user_data);
    
    // Signal producers if queue was full
    if (queue->max_size > 0 && queue->count == queue->max_size - 1) {
      pthread_cond_signal(&queue->not_full);
    }
    
    pthread_mutex_unlock(&queue->mutex);
    return true;
  }
  
  pthread_mutex_unlock(&queue->mutex);
  return false;
}

// Configure thread-local buffer size
void queue_set_buffer_size(queue_t* queue, size_t buffer_size) {
  if (!queue) return;
  
  pthread_mutex_lock(&queue->mutex);
  queue->buffer_flush_threshold = buffer_size;
  
  // Update existing buffers
  queue_buffer_t* buffer = queue->buffers;
  while (buffer) {
    buffer->max_size = buffer_size;
    buffer = buffer->next;
  }
  
  pthread_mutex_unlock(&queue->mutex);
}

// Get current queue size
size_t queue_size(queue_t* queue) {
  if (!queue) return 0;
  
  pthread_mutex_lock(&queue->mutex);
  size_t count = queue->count;
  pthread_mutex_unlock(&queue->mutex);
  
  return count;
}

// Shutdown the queue
void queue_shutdown(queue_t* queue) {
  if (!queue) return;
  
  pthread_mutex_lock(&queue->mutex);
  queue->shutdown = true;
  
  // Wake up all waiting consumers
  pthread_cond_broadcast(&queue->not_empty);
  pthread_cond_broadcast(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
}

/* To be Implemented 


// Memory pool allocator implementation
typedef struct pool_allocator_data_s {
  size_t block_size;     // Size of each memory block
  size_t blocks_per_chunk; // Number of blocks per chunk
  size_t item_size;      // Size of each queue item

  void* free_list;       // Linked list of free blocks
  void** chunks;         // Array of allocated chunks
  size_t chunk_count;    // Number of chunks allocated
  size_t max_chunks;     // Maximum number of chunks allowed

  pthread_mutex_t mutex; // Mutex for thread safety
} pool_allocator_data_t;

// Pool allocator free block definition
typedef struct pool_block_s {
  struct pool_block_s* next;
} pool_block_t;

// Pool allocator initialization
static pool_allocator_data_t* pool_allocator_create(size_t item_size, size_t blocks_per_chunk, size_t max_chunks) {
  pool_allocator_data_t* pool = malloc(sizeof(pool_allocator_data_t));
  if (!pool) return NULL;

  // Calculate block size (must be at least sizeof(pool_block_t))
  pool->item_size = item_size;
  pool->block_size = item_size < sizeof(pool_block_t) ? sizeof(pool_block_t) : item_size;
  pool->blocks_per_chunk = blocks_per_chunk;
  pool->free_list = NULL;
  
  pool->chunks = malloc(sizeof(void*) * 8); // Initial capacity for 8 chunks
  if (!pool->chunks) {
      free(pool);
      return NULL;
  }
  
  pool->chunk_count = 0;
  pool->max_chunks = max_chunks;
  
  if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
      free(pool->chunks);
      free(pool);
      return NULL;
  }
  
  // Allocate initial chunk
  size_t chunk_size = pool->block_size * pool->blocks_per_chunk;
  void* chunk = malloc(chunk_size);
  if (!chunk) {
      pthread_mutex_destroy(&pool->mutex);
      free(pool->chunks);
      free(pool);
      return NULL;
  }
  
  // Initialize free list with blocks from the chunk
  pool_block_t* current = NULL;
  for (size_t i = 0; i < pool->blocks_per_chunk; i++) {
      pool_block_t* block = (pool_block_t*)((char*)chunk + (i * pool->block_size));
      block->next = current;
      current = block;
  }
  
  pool->free_list = current;
  pool->chunks[0] = chunk;
  pool->chunk_count = 1;
  
  return pool;
}

// Pool allocator destruction
static void pool_allocator_destroy(pool_allocator_data_t* pool) {
  if (!pool) return;
  
  pthread_mutex_lock(&pool->mutex);
  
  // Free all allocated chunks
  for (size_t i = 0; i < pool->chunk_count; i++) {
      free(pool->chunks[i]);
  }
  
  free(pool->chunks);
  pthread_mutex_unlock(&pool->mutex);
  pthread_mutex_destroy(&pool->mutex);
  free(pool);
}

// Pool allocator memory allocation
static void* pool_alloc(size_t size, void* user_data) {
  pool_allocator_data_t* pool = (pool_allocator_data_t*)user_data;
  if (!pool || size > pool->item_size) return NULL;
  
  pthread_mutex_lock(&pool->mutex);
  
  // Check if free list is empty
  if (!pool->free_list) {
      // Check if we've reached the maximum chunk limit
      if (pool->max_chunks > 0 && pool->chunk_count >= pool->max_chunks) {
          pthread_mutex_unlock(&pool->mutex);
          return NULL;
      }
      
      // Allocate a new chunk
      size_t chunk_size = pool->block_size * pool->blocks_per_chunk;
      void* chunk = malloc(chunk_size);
      if (!chunk) {
          pthread_mutex_unlock(&pool->mutex);
          return NULL;
      }
      
      // Add blocks to free list
      pool_block_t* current = NULL;
      for (size_t i = 0; i < pool->blocks_per_chunk; i++) {
          pool_block_t* block = (pool_block_t*)((char*)chunk + (i * pool->block_size));
          block->next = current;
          current = block;
      }
      
      pool->free_list = current;
      
      // Check if we need to resize the chunks array
      if (pool->chunk_count >= 8 && pool->chunk_count % 8 == 0) {
          size_t new_capacity = pool->chunk_count * 2;
          void** new_chunks = realloc(pool->chunks, sizeof(void*) * new_capacity);
          if (!new_chunks) {
              // Free the newly allocated chunk if we can't resize the chunks array
              free(chunk);
              pthread_mutex_unlock(&pool->mutex);
              return NULL;
          }
          pool->chunks = new_chunks;
      }
      
      // Add chunk to the chunks array
      pool->chunks[pool->chunk_count++] = chunk;
  }
  
  // Get a block from the free list
  pool_block_t* block = (pool_block_t*)pool->free_list;
  pool->free_list = block->next;
  
  pthread_mutex_unlock(&pool->mutex);
  
  // Clear the memory
  memset(block, 0, pool->block_size);
  return block;
}

// Pool allocator memory free
static void pool_free(void* ptr, void* user_data) {
  if (!ptr) return;
  
  pool_allocator_data_t* pool = (pool_allocator_data_t*)user_data;
  if (!pool) return;
  
  pthread_mutex_lock(&pool->mutex);
  
  // Add the block back to the free list
  pool_block_t* block = (pool_block_t*)ptr;
  block->next = (pool_block_t*)pool->free_list;
  pool->free_list = block;
  
  pthread_mutex_unlock(&pool->mutex);
}

// Create a pool allocator for queue items
queue_allocator_t queue_create_pool_allocator(size_t item_size, size_t blocks_per_chunk, size_t max_chunks) {
  pool_allocator_data_t* pool = pool_allocator_create(item_size, blocks_per_chunk, max_chunks);
  
  queue_allocator_t allocator = {
      .alloc = pool_alloc,
      .free = pool_free,
      .user_data = pool
  };
  
  return allocator;
}

// Free the pool allocator
void queue_destroy_pool_allocator(queue_allocator_t* allocator) {
  if (!allocator) return;
  pool_allocator_data_t* pool = (pool_allocator_data_t*)allocator->user_data;
  pool_allocator_destroy(pool);
  
  // Reset the allocator to avoid using freed memory
  allocator->alloc = NULL;
  allocator->free = NULL;
  allocator->user_data = NULL;
}

// --------------------- Thread-Local Allocator ---------------------
// This allocator uses thread-local storage to reduce contention

typedef struct tls_allocator_data_s {
  pthread_key_t key;     // Thread-local storage key
  size_t item_size;      // Size of each item to allocate
  size_t batch_size;     // Number of items to allocate at once
  queue_allocator_t base_allocator; // Underlying allocator for bulk allocations
} tls_allocator_data_t;

typedef struct tls_free_list_s {
  void* free_list;       // Thread-local free list
  void** chunks;         // Array of allocated chunks
  size_t chunk_count;    // Number of chunks allocated
  size_t capacity;       // Capacity of chunks array
} tls_free_list_t;

// Thread-local storage destructor
static void tls_free_list_destructor(void* data) {
  tls_free_list_t* free_list = (tls_free_list_t*)data;
  if (!free_list) return;
  
  // Free all allocated chunks through the base allocator
  // Note: This actually doesn't release memory immediately, but during queue_destroy
  free(free_list->chunks);
  free(free_list);
}

// TLS allocator initialization
static tls_allocator_data_t* tls_allocator_create(size_t item_size, size_t batch_size, queue_allocator_t* base_allocator) {
  tls_allocator_data_t* data = malloc(sizeof(tls_allocator_data_t));
  if (!data) return NULL;
  
  data->item_size = item_size;
  data->batch_size = batch_size;
  
  // Use provided base allocator or default
  data->base_allocator = base_allocator ? *base_allocator : queue_default_allocator;
  
  if (pthread_key_create(&data->key, tls_free_list_destructor) != 0) {
      free(data);
      return NULL;
  }
  
  return data;
}

// TLS allocator destruction
static void tls_allocator_destroy(tls_allocator_data_t* data) {
  if (!data) return;
  pthread_key_delete(data->key);
  free(data);
}

// TLS allocator memory allocation
static void* tls_alloc(size_t size, void* user_data) {
  tls_allocator_data_t* data = (tls_allocator_data_t*)user_data;
  if (!data || size > data->item_size) return NULL;
  
  // Get or create thread-local free list
  tls_free_list_t* free_list = pthread_getspecific(data->key);
  if (!free_list) {
      free_list = malloc(sizeof(tls_free_list_t));
      if (!free_list) return NULL;
      
      free_list->free_list = NULL;
      free_list->chunks = malloc(sizeof(void*) * 8); // Initial capacity
      if (!free_list->chunks) {
          free(free_list);
          return NULL;
      }
      
      free_list->chunk_count = 0;
      free_list->capacity = 8;
      
      pthread_setspecific(data->key, free_list);
  }
  
  // Check if free list is empty
  if (!free_list->free_list) {
      // Allocate a new batch
      size_t batch_size = data->item_size * data->batch_size;
      void* batch = data->base_allocator.alloc(batch_size, data->base_allocator.user_data);
      if (!batch) return NULL;
      
      // Add to chunks array
      if (free_list->chunk_count >= free_list->capacity) {
          size_t new_capacity = free_list->capacity * 2;
          void** new_chunks = realloc(free_list->chunks, sizeof(void*) * new_capacity);
          if (!new_chunks) {
              data->base_allocator.free(batch, data->base_allocator.user_data);
              return NULL;
          }
          free_list->chunks = new_chunks;
          free_list->capacity = new_capacity;
      }
      
      free_list->chunks[free_list->chunk_count++] = batch;
      
      // Initialize free list with blocks from the batch
      pool_block_t* current = NULL;
      for (size_t i = 0; i < data->batch_size - 1; i++) { // -1 because we'll return the last block
          pool_block_t* block = (pool_block_t*)((char*)batch + (i * data->item_size));
          block->next = current;
          current = block;
      }
      
      free_list->free_list = current;
      
      // Return the last block
      return (char*)batch + ((data->batch_size - 1) * data->item_size);
  }
  
  // Get a block from the free list
  pool_block_t* block = (pool_block_t*)free_list->free_list;
  free_list->free_list = block->next;
  
  // Clear the memory
  memset(block, 0, data->item_size);
  return block;
}

// TLS allocator memory free
static void tls_free(void* ptr, void* user_data) {
  if (!ptr) return;
  
  tls_allocator_data_t* data = (tls_allocator_data_t*)user_data;
  if (!data) return;
  
  // Get thread-local free list
  tls_free_list_t* free_list = pthread_getspecific(data->key);
  if (!free_list) {
      // If no free list exists, this might be a pointer from another thread
      // Just use the base allocator to free it
      data->base_allocator.free(ptr, data->base_allocator.user_data);
      return;
  }
  
  // Add the block back to the free list
  pool_block_t* block = (pool_block_t*)ptr;
  block->next = (pool_block_t*)free_list->free_list;
  free_list->free_list = block;
}

// Create a thread-local allocator
queue_allocator_t queue_create_tls_allocator(size_t item_size, size_t batch_size, queue_allocator_t* base_allocator) {
  tls_allocator_data_t* data = tls_allocator_create(item_size, batch_size, base_allocator);
  
  queue_allocator_t allocator = {
      .alloc = tls_alloc,
      .free = tls_free,
      .user_data = data
  };
  
  return allocator;
}

// Free the thread-local allocator
void queue_destroy_tls_allocator(queue_allocator_t* allocator) {
  if (!allocator) return;
  tls_allocator_data_t* data = (tls_allocator_data_t*)allocator->user_data;
  tls_allocator_destroy(data);
  
  // Reset the allocator to avoid using freed memory
  allocator->alloc = NULL;
  allocator->free = NULL;
  allocator->user_data = NULL;
}

// --------------------- Aligned Allocator ---------------------
// This allocator ensures memory is aligned for better performance with SIMD operations

typedef struct aligned_allocator_data_s {
  size_t alignment;     // Alignment requirement (power of 2)
  queue_allocator_t base_allocator; // Underlying allocator
} aligned_allocator_data_t;

// Aligned allocator memory allocation
static void* _aligned_alloc(size_t size, void* user_data) {
  aligned_allocator_data_t* data = (aligned_allocator_data_t*)user_data;
  if (!data) return NULL;
  
  // Calculate required size with alignment padding and pointer storage
  size_t total_size = size + data->alignment + sizeof(void*);
  
  // Allocate memory
  void* base_ptr = data->base_allocator.alloc(total_size, data->base_allocator.user_data);
  if (!base_ptr) return NULL;
  
  // Calculate aligned address
  void* aligned_ptr = (void*)(((uintptr_t)base_ptr + sizeof(void*) + data->alignment - 1) & ~(data->alignment - 1));
  
  // Store original pointer for freeing
  *((void**)aligned_ptr - 1) = base_ptr;
  
  return aligned_ptr;
}

// Aligned allocator memory free
static void aligned_free(void* ptr, void* user_data) {
  if (!ptr) return;
  
  aligned_allocator_data_t* data = (aligned_allocator_data_t*)user_data;
  if (!data) return;
  
  // Get original pointer
  void* base_ptr = *((void**)ptr - 1);
  
  // Free using base allocator
  data->base_allocator.free(base_ptr, data->base_allocator.user_data);
}

// Create an aligned allocator
queue_allocator_t queue_create_aligned_allocator(size_t alignment, queue_allocator_t* base_allocator) {
  // Ensure alignment is a power of 2
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
      alignment = 16; // Default to 16-byte alignment if invalid
  }
  
  aligned_allocator_data_t* data = malloc(sizeof(aligned_allocator_data_t));
  if (!data) {
      queue_allocator_t null_allocator = {NULL, NULL, NULL};
      return null_allocator;
  }
  
  data->alignment = alignment;
  data->base_allocator = base_allocator ? *base_allocator : queue_default_allocator;
  
  queue_allocator_t allocator = {
      .alloc = _aligned_alloc,
      .free = aligned_free,
      .user_data = data
  };
  
  return allocator;
}

// Free the aligned allocator
void queue_destroy_aligned_allocator(queue_allocator_t* allocator) {
  if (!allocator) return;
  aligned_allocator_data_t* data = (aligned_allocator_data_t*)allocator->user_data;
  free(data);
  
  // Reset the allocator to avoid using freed memory
  allocator->alloc = NULL;
  allocator->free = NULL;
  allocator->user_data = NULL;
}

// --------------------- Helper Functions ---------------------

// Function to create a memory pool optimized for queue items
queue_allocator_t queue_create_item_pool(size_t estimated_queue_size) {
  // Calculate optimal pool size based on estimated usage
  size_t blocks_per_chunk = estimated_queue_size > 1000 ? 1024 : 256;
  
  // Create a pool allocator sized for queue_item_t
  return queue_create_pool_allocator(
      sizeof(queue_item_t),
      blocks_per_chunk,
      0  // No maximum limit on chunks
  );
}

// Function to create a memory pool optimized for queue buffers
queue_allocator_t queue_create_buffer_pool(void) {
  // Buffers are larger but less frequently allocated
  return queue_create_pool_allocator(
      sizeof(queue_buffer_t),
      64,  // 64 buffers per chunk (usually one per thread)
      16   // Maximum 16 chunks (limits extreme cases)
  );
}
*/