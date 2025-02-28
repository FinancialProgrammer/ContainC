# ContainC
A collection of fast containers built for GNU C.
In the future these containers should be backtraced to C99 and C89 to help with obscure operating systems that don't support C11 (windows).

## MPMC Queue
A multi producer and multi consumer queue which is benchmarked against rigtorp/MPMCQueue and cameron314/concurrentqueue both on github. These queues seem to outperform standard boost and C++ std queues and this C queue performs as well if not better. Unfortunately no noticeable performance improvements have been made but this is expected. This can be though as the MPMCQueue of C as it performs about as well without locking you into using a complicated C++ compiler helping with compatibility.
