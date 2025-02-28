#include "ContainC/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <getopt.h>

// Include headers for comparison queues
#include "rigtorp/MPMCQueue.h"
#include "concurrentqueue/moodycamel/concurrentqueue.h"

using namespace std;

typedef struct {
  moodycamel::ConcurrentQueue<void*>* queue;
  size_t num_items;
  size_t num_producers;
  size_t num_consumers;
  atomic_size_t items_produced;
  atomic_size_t items_consumed;
  atomic_bool running;
  pthread_barrier_t barrier;
  size_t buffer_size;
  double duration_seconds;
} benchmark_context_t;

// Timing utility functions
static double get_time_seconds() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// Producer thread function
void* producer_thread(void* arg) {
  benchmark_context_t* ctx = (benchmark_context_t*)arg;
  size_t items_per_producer = ctx->num_items / ctx->num_producers;
  size_t produced = 0;
  
  // Wait for all threads to be ready
  pthread_barrier_wait(&ctx->barrier);
  
  while (atomic_load(&ctx->running) && produced < items_per_producer) {
    // Create dummy data (just use the counter as data)
    void* data = (void*)(uintptr_t)(produced + 1);
    bool success = false;
    
    ctx->queue->enqueue(data);
    success = true;

    if (success) {
      produced++;
      atomic_fetch_add(&ctx->items_produced, 1);
    }
  }

  return NULL;
}

// Consumer thread function
void* consumer_thread(void* arg) {
  benchmark_context_t* ctx = (benchmark_context_t*)arg;
  
  // Wait for all threads to be ready
  pthread_barrier_wait(&ctx->barrier);
  
  while (atomic_load(&ctx->running) || 
         atomic_load(&ctx->items_consumed) < atomic_load(&ctx->items_produced)) {
    void* data = NULL;
    bool success = false;

    success = ctx->queue->try_dequeue(data);

    if (success) {
      atomic_fetch_add(&ctx->items_consumed, 1);
    } else if (!atomic_load(&ctx->running) && 
               atomic_load(&ctx->items_consumed) >= atomic_load(&ctx->items_produced)) {
      // All items consumed and no longer running
      break;
    } else {
      // Small sleep to avoid busy-waiting when empty
      usleep(10);
    }
  }
  
  return NULL;
}

// Run benchmark for a specific queue implementation
void run_benchmark(benchmark_context_t* ctx, const char* name) {
  pthread_t* producers = NULL;
  pthread_t* consumers = NULL;
  double start_time, end_time;
  double throughput = 0.0;
  size_t final_consumed = 0;
  
  printf("Benchmarking %s:\n", name);
  printf("  Producers: %zu, Consumers: %zu, Items: %zu, Buffer Size: %zu\n", 
         ctx->num_producers, ctx->num_consumers, ctx->num_items, ctx->buffer_size);
  
  // Initialize atomic variables
  atomic_store(&ctx->items_produced, 0);
  atomic_store(&ctx->items_consumed, 0);
  atomic_store(&ctx->running, true);
  
  // Initialize barrier
  pthread_barrier_init(&ctx->barrier, NULL, ctx->num_producers + ctx->num_consumers + 1);
  
  // Create producer and consumer threads
  producers = (pthread_t*)malloc(ctx->num_producers * sizeof(pthread_t));
  consumers = (pthread_t*)malloc(ctx->num_consumers * sizeof(pthread_t));
  
  if (!producers || !consumers) {
    fprintf(stderr, "Failed to allocate thread arrays\n");
    free(producers);
    free(consumers);
    return;
  }
  
  // Start consumers
  for (size_t i = 0; i < ctx->num_consumers; i++) {
    if (pthread_create(&consumers[i], NULL, consumer_thread, ctx) != 0) {
      fprintf(stderr, "Failed to create consumer thread\n");
      atomic_store(&ctx->running, false);
      
      // Wait for existing threads to finish
      for (size_t j = 0; j < i; j++) {
        pthread_join(consumers[j], NULL);
      }
      
      free(producers);
      free(consumers);
      return;
    }
  }
  
  // Start producers
  for (size_t i = 0; i < ctx->num_producers; i++) {
    if (pthread_create(&producers[i], NULL, producer_thread, ctx) != 0) {
      fprintf(stderr, "Failed to create producer thread\n");
      atomic_store(&ctx->running, false);
      
      // Wait for existing threads to finish
      for (size_t j = 0; j < i; j++) {
        pthread_join(producers[j], NULL);
      }
      
      for (size_t j = 0; j < ctx->num_consumers; j++) {
        pthread_join(consumers[j], NULL);
      }
      
      free(producers);
      free(consumers);
      return;
    }
  }
  
  // Wait for all threads to be ready
  pthread_barrier_wait(&ctx->barrier);
  
  // Start timing
  start_time = get_time_seconds();
  
  // Let the benchmark run for the specified duration
  usleep((useconds_t)(ctx->duration_seconds * 1000000));
  
  // Stop the benchmark
  end_time = get_time_seconds();
  atomic_store(&ctx->running, false);
  
  // Wait for all threads to finish
  for (size_t i = 0; i < ctx->num_producers; i++) {
    pthread_join(producers[i], NULL);
  }
  
  for (size_t i = 0; i < ctx->num_consumers; i++) {
    pthread_join(consumers[i], NULL);
  }
  
  // Calculate results
  final_consumed = atomic_load(&ctx->items_consumed);
  double elapsed = end_time - start_time;
  throughput = final_consumed / elapsed;
  
  // Report results
  printf("  Results:\n");
  printf("    Items produced: %zu\n", atomic_load(&ctx->items_produced));
  printf("    Items consumed: %zu\n", final_consumed);
  printf("    Time elapsed: %.4f seconds\n", elapsed);
  printf("    Throughput: %.2f items/second\n", throughput);
  printf("    Latency: %.2f ns/item\n", (elapsed * 1e9) / final_consumed);
  printf("\n");
  
  // Clean up
  pthread_barrier_destroy(&ctx->barrier);
  free(producers);
  free(consumers);
}

void print_usage(const char* program_name) {
  printf("Usage: %s [options]\n", program_name);
  printf("Options:\n");
  printf("  -p, --producers NUM     Number of producer threads (default: 4)\n");
  printf("  -c, --consumers NUM     Number of consumer threads (default: 4)\n");
  printf("  -n, --items NUM         Total number of items to produce (default: 10000000)\n");
  printf("  -b, --buffer NUM        Size of thread-local buffer (default: 32)\n");
  printf("  -d, --duration NUM      Duration of benchmark in seconds (default: 5.0)\n");
  printf("  -h, --help              Show this help message\n");
}

int main(int argc, char* argv[]) {
  benchmark_context_t ctx;
  ctx.num_producers = 4;
  ctx.num_consumers = 4;
  ctx.num_items = 10000000;
  ctx.buffer_size = 32;
  ctx.duration_seconds = 5.0;
  
  // Parse command line arguments
  static struct option long_options[] = {
    {"producers", required_argument, 0, 'p'},
    {"consumers", required_argument, 0, 'c'},
    {"items",     required_argument, 0, 'n'},
    {"buffer",    required_argument, 0, 'b'},
    {"duration",  required_argument, 0, 'd'},
    {"help",      no_argument,       0, 'h'},
    {0, 0, 0, 0}
  };
  
  int option_index = 0;
  int c;
  
  while ((c = getopt_long(argc, argv, "p:c:n:b:d:h", long_options, &option_index)) != -1) {
    switch (c) {
      case 'p':
        ctx.num_producers = atoi(optarg);
        break;
      
      case 'c':
        ctx.num_consumers = atoi(optarg);
        break;
      
      case 'n':
        ctx.num_items = atoi(optarg);
        break;
      
      case 'b':
        ctx.buffer_size = atoi(optarg);
        break;
      
      case 'd':
        ctx.duration_seconds = atof(optarg);
        break;
      
      case 'h':
        print_usage(argv[0]);
        return 0;
      
      case '?':
        print_usage(argv[0]);
        return 1;
      
      default:
        break;
    }
  }
  
  printf("Starting benchmarks...\n\n");

  ctx.queue = new moodycamel::ConcurrentQueue<void*>();
  run_benchmark(&ctx, "cameron314 ConcurrentQueue");
  delete ctx.queue;
  
  
  printf("All benchmarks completed.\n");
  
  return 0;
}