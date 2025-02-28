project('mpmc_queue', 'c', 'cpp',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'cpp_std=c++14',
    'warning_level=3'
  ]
)

# Add include directories
include_dirs = include_directories(
  'include/'
)

# Dependencies
thread_dep = dependency('threads')

# Compile our queue implementation
queue_lib = static_library('queue',
  sources: ['src/queue.c'],
  include_directories: include_dirs,
  dependencies: [thread_dep]
)

# Create benchmark executable
bench_mpmc_queue = executable('queue_benchmark',
  sources: ['unittests/bench_mpmcqueue.cpp'],
  include_directories: include_dirs,
  link_with: [queue_lib],
  dependencies: [thread_dep],
  cpp_args: ['-fpermissive']  # Some C++ comparison libraries might need this
)

# mpmc queue benchmarks as tests
test('small_benchmark', bench_mpmc_queue,
  args: ['--producers', '2', '--consumers', '2', '--items', '100000', '--duration', '1'],
  timeout: 60
)

test('balanced_benchmark', bench_mpmc_queue,
  args: ['--producers', '4', '--consumers', '4', '--items', '1000000', '--duration', '3'],
  timeout: 120
)

test('producer_heavy', bench_mpmc_queue,
  args: ['--producers', '8', '--consumers', '2', '--items', '1000000', '--duration', '3'],
  timeout: 120
)

test('consumer_heavy', bench_mpmc_queue,
  args: ['--producers', '2', '--consumers', '8', '--items', '1000000', '--duration', '3'],
  timeout: 120
)