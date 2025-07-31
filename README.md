# SP::ThreadPool

## tl;dr

```cpp
#include "thread_pool.hpp"   // just drop the header somewhere in your include path

std::future<void> fut = SP::ThreadPool::submit_task(0, N, [](std::size_t i) {
    heavy_work(i);
});

SP::ThreadPool::wait_for_all();   // blocks until every submitted task finishes
// fut.get() to wait for a specific task the future is returned from
```

*   **No manual boot‑up required** – first call auto‑initialises the pool.  
*   `submit_task(...)` has overloads for range loops, custom chunking, and per‑task thread caps.  
*   `set_thread_count(n)` changes the pool size globally, any time.  
*   `shutdown()` tears everything down (use once at program exit).  

## Key Features
**`SP::ThreadPool` is a **single‑header, C++17** work‑stealing thread pool designed for embarrassingly parallel workloads.**   
### Zero Dependency
The pool only uses the C++ Standard Library
### Work Stealing
Idle threads pull tasks from heavier queues for near-perfect load balancing.
### Highly-Customizable Workload
You can divide a loop into either by `chunk_count`, `chunk_size`, or `chunk_multiplier`. \
`chunk_count`: Exact number of chunks to divide the task into \
`chunk_size`: How big each task should be. The pool, then, computes appropriate `chunk_count`. \
`chunk_multiplier`: The number of chunks each threads should be assigned to. The pool, then, computes appropriate `chunk_count`.
### Threads Cap
For simple tasks, either set `chunk_count < number_of_available_threads` or `threads_cap` to reduce the overhead of using multi-threading.

## Getting Started

1. **Add the header**

   ```cpp
   #include "thread_pool.hpp"
   ```

2. **Add compiler flag**

    ```cpp
    -pthread
    ```

    That's it! The thread pool is self-contained inside that header.

3. **Submit work**

   ```cpp
   std::size_t N = 1'000'000;
   SP::ThreadPool::submit_task(0, N, [](std::size_t i) { compute(i); });
   SP::ThreadPool::wait_for_all();
   ```

4. **Tune if required**

   ```cpp
   SP::ThreadPool::set_thread_count(8);      // fixed pool size
   SP::ThreadPool::set_work_stealing(false); // FIFO scheduling only
   ```

## How It Works (under the hood)

| Mechanism               | Summary                                                                                                                                         |
|-------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| **Per‑thread deques**   | Each worker owns a double‑ended queue. It pushes its own tasks at the back and pops from the front (FIFO, cache‑friendly for recursion).          |
| **Work stealing**       | When a worker runs dry, it scans the other queues and **steals** from the *back* of the fullest one, minimizing contention. Stealing can be disabled anytime using `set_work_stealing(bool)`                     |
| **Thread cap**          | `set_threads_cap(k)` (permanent) or `submit_task_with_threads_cap()` (temporarily) limit active workers to `k` (useful when sharing CPUs). The pool will automatically use the first `k` threads.                 |
| **Processor affinity**  | `set_processor_affinity()` pins each worker to a core (`SetThreadAffinityMask`/`pthread_setaffinity_np`). The pool uses sequential pinning. If there are more threads than cores, we will wrap around and assign multiple threads to a single core. Optional; call once after boot.        |
| **Auto‑teardown**       | When the last task finishes and you call `shutdown()`, all threads join cleanly. The destructor also triggers this on program exit.             |

## Public API

All methods are **static** – call them via the class.

| Method | Purpose | Notes / Warnings |
|--------|---------|------------------|
| `submit_task(F&& f)` | Enqueue a single functor. | Round‑robin assignment; returns `std::future<void>` immediately. |
| `submit_task(start, end, F&& f)` | Split `[start,end)` into `threads × 4` chunks (default multiplier) and process in parallel. | **Non‑blocking**; wait via the returned future *or* `wait_for_all()`. |
| `submit_task(start,end,chunk_cnt,F&& f)` | Explicit number of chunks. | Chunk count 0 is a no‑op (future resolves instantly). |
| `submit_task_with_chunk_multiplier(start,end,mul,F&& f)` | `chunks = mul × active_threads`. | Use for coarse‑/fine‑grained tuning. |
| `submit_task_with_chunk_size(start,end,chunk_sz,F&& f)` | Fixed item count per chunk. | |
| `submit_task_with_threads_cap(start,end,cap,F&& f)` | Temporarily restrict workers for this task. | **Important:** tasks submitted with a cap must finish (wait on the future) **before** submitting new capped tasks, to avoid starvation. |
| `submit_task_with_threads_cap(start,end,cap,mul,F&& f)` | Cap + custom chunking. | Same caution as above. |
| `wait_for_all()` | Block until every queued task completes. | Safe to call multiple times. |
| `set_thread_count(n)` | Resize pool (spawns or joins threads). | Active tasks continue; new size applies to future submissions. |
| `get_thread_count()` | Current configured pool size. | Does **not** reflect temporary caps. |
| `set_threads_cap(k)` | Manually cap active workers. | Applies to *all* subsequent tasks until changed again. |
| `get_threads_cap()` | Current global cap. | |
| `set_work_stealing(bool)` | Enable/disable stealing. | Disabling may reduce jitter for real‑time work at the cost of load balance. |
| `set_processor_affinity()` | Pin threads to cores. | Call once after the pool is running. No‑op on some platforms. |
| `shutdown()` | Join all workers and free memory. | Call once, usually from `main()` shutdown path. |
| `soft_boot()` | Pre‑launch threads without submitting a task. | Rarely needed; used in low‑latency systems. |

## Usage Patterns

### Simple parallel loop

```cpp
std::vector<int> data(1'000'000);
auto fut = SP::ThreadPool::submit_task(0, data.size(), [&](std::size_t i) {
    data[i] = heavy_compute(i);
});
fut.get(); // or SP::ThreadPool::wait_for_all();
```

### Cap workers for an I/O bound section

```cpp
auto fut = SP::ThreadPool::submit_task_with_threads_cap(
            0, files.size(), /*cap=*/4, [&](std::size_t i) {
    parse_file(files[i]);
});
fut.get();   // MUST finish before you raise the cap or submit further capped work
```

### Custom chunk size

```cpp
constexpr std::size_t CHUNK = 16;
SP::ThreadPool::submit_task_with_chunk_size(0, N, CHUNK, do_work);
```

### Single, fire‑and‑forget task

```cpp
SP::ThreadPool::submit_task([] { prewarm(); });
```

## Best Practices

* **Always wait** (`future::get()` or `wait_for_all()`) when using per‑call thread caps to avoid deadlocks.
* Call `set_processor_affinity()` **after** the pool is up and before heavy computation phases.
* Avoid very small chunk sizes (< 2–4 µs of work) to minimize scheduler overhead.
* For library authors: wrap pool calls so that you can fall back to the caller’s executor in the future.

## Building / Integration

No build system magic – include the header and compile with **C++17** or newer.  
On Windows you may need to link against `Synchronize.lib` implicitly provided by MSVC (nothing extra to do on recent toolchains).

## Roadmap

* `submit_bulk()` returning a `future<void>` per element group.  
* Integration with C++26 `std::execution`.  
* Optional task priorities.  

## Tests

*(to be filled by repo owner)*

## Benchmark

*(to be filled by repo owner)*

## Contributing

1. Fork & branch.  
2. Follow the style in the existing header (clang‑format file forthcoming).  
3. Open a PR; GitHub CI runs sanitizer + unit tests.  

## License

MIT © 2025 – see `LICENSE` file.

