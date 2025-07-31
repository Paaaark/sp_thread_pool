#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <string>
#include <iomanip>

#include "sp_thread_pool.hpp"
#include "BS_thread_pool.hpp"  // Replace with the actual BS thread-pool header

using my_clock_t = std::chrono::high_resolution_clock;

// Compute mean of values
static double mean(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

// Compute standard deviation
static double stddev(const std::vector<double>& v, double m) {
    double sum = 0.0;
    for (double x : v) sum += (x - m) * (x - m);
    return std::sqrt(sum / v.size());
}

// Mandelbrot iteration for a single index
inline int mandelbrot_pixel(int idx, int W, int H, int max_iter) {
    int y = idx / W;
    int x = idx % W;
    double cr = (x - W / 1.3) * 3.0 / W;
    double ci = (y - H / 2.0) * 3.0 / W;
    double zr = 0.0, zi = 0.0;
    int k = 0;
    while (zr*zr + zi*zi <= 4.0 && k < max_iter) {
        double tmp = zr*zr - zi*zi + cr;
        zi = 2.0*zr*zi + ci;
        zr = tmp;
        ++k;
    }
    return k;
}

// Benchmarking function template
template <typename PoolType, typename SubmitFn>
void benchmark_pool(const std::string& label,
                    int W, int H, int max_iter,
                    const std::vector<size_t>& task_counts,
                    int repeats,
                    SubmitFn submit_func) {
    size_t pixels = static_cast<size_t>(W) * H;
    std::vector<int> buffer(pixels);

    std::cout << label << ":\n";
    std::cout << "Each test will be repeated " << repeats << " times to collect reliable statistics." << std::endl;

    double single_mean = 0.0;
    // Single-task (serial) run
    {
        std::vector<double> times;
        for (int r = 0; r < repeats; ++r) {
            std::fill(buffer.begin(), buffer.end(), 0);
            auto t0 = my_clock_t::now();
            submit_func(1, buffer);
            auto t1 = my_clock_t::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            times.push_back(ms);
        }
        single_mean = mean(times);
        double sd = stddev(times, single_mean);
        double speed = pixels / single_mean;
        std::cout << "   1 task:  [..............................]  (single-threaded)" << std::endl;
        std::cout << "-> Mean:  " << single_mean << " ms, standard deviation:  " << sd
                  << " ms, speed:  " << speed << " pixels/ms." << std::endl;
    }

    // Multitask runs
    double best_speedup = 1.0;
    size_t best_tasks = 1;
    for (size_t tasks : task_counts) {
        if (tasks == 1) continue;
        std::vector<double> times;
        for (int r = 0; r < repeats; ++r) {
            std::fill(buffer.begin(), buffer.end(), 0);
            auto t0 = my_clock_t::now();
            submit_func(tasks, buffer);
            auto t1 = my_clock_t::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            times.push_back(ms);
        }
        double m = mean(times);
        auto min_it = std::min_element(times.begin(), times.end());
        double sd = stddev(times, m);
        double speed = pixels / m;
        std::cout << " " << std::setw(4) << tasks << " tasks: [..............................]" << std::endl;
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "-> Min: " << std::setw(7) << *min_it << " ms, mean: " << std::setw(7) << m << " ms, std dev: " << std::setw(6) << sd
                  << " ms, speed: " << std::setw(8) << speed << " pixels/ms." << std::endl;

        double speedup = single_mean / m;
        if (speedup > best_speedup) {
            best_speedup = speedup;
            best_tasks = tasks;
        }
    }

    std::cout << "Maximum speedup obtained by " << label
              << " vs. single-threading: " << best_speedup
              << "x, using " << best_tasks << " tasks." << std::endl << std::endl;
}

// Validate that BS and SP produce identical outputs
void validate_outputs(int W, int H, int max_iter, const std::vector<size_t>& task_counts) {
    size_t pixels = static_cast<size_t>(W) * H;
    std::vector<int> buf_bs(pixels), buf_sp(pixels);
    std::cout << "Validating correctness of BS::Thread_Pool vs SP::ThreadPool outputs..." << std::endl;
    for (size_t tasks : task_counts) {
        // BS run
        BS::thread_pool pool(tasks);
        auto f_bs = pool.submit_loop(0, pixels,
            [&](size_t idx) { buf_bs[idx] = mandelbrot_pixel(idx, W, H, max_iter); }, tasks);
        f_bs.wait();

        // SP run
        SP::ThreadPool::set_thread_count(std::thread::hardware_concurrency());
        SP::ThreadPool::soft_boot();
        auto f_sp = SP::ThreadPool::submit_task(0, pixels, tasks,
            [&](size_t idx) { buf_sp[idx] = mandelbrot_pixel(idx, W, H, max_iter); });
        f_sp.get();

        // compare buffers
        bool ok = true;
        size_t mismatch = 0;
        for (size_t i = 0; i < pixels; ++i) {
            if (buf_bs[i] != buf_sp[i]) {
                ok = false;
                mismatch = i;
                break;
            }
        }
        if (ok) {
            std::cout << "  [OK] outputs match for " << tasks << " tasks" << std::endl;
        } else {
            std::cout << "  [ERROR] mismatch at pixel " << mismatch
                      << ": bs=" << buf_bs[mismatch]
                      << ", sp=" << buf_sp[mismatch] << std::endl;
        }
    }
    std::cout << std::endl;
}

int main(int argc, char** argv) {
    bool validate = false;
    std::vector<std::string> args(argv, argv + argc);

    auto it = std::find(args.begin(), args.end(), "-v");
    if (it != args.end()) {
        validate = true;
        args.erase(it);
    }

    const int W        = (args.size() > 1) ? std::stoi(args[1]) : 3965;
    const int H        = (args.size() > 2) ? std::stoi(args[2]) : 3965;
    const int MAX_ITER = (args.size() > 3) ? std::stoi(args[3]) : 1000;
    const int THREADS  = (args.size() > 4) ? std::stoi(args[4])
                                    : std::thread::hardware_concurrency();

    std::cout << "Rendering " << W << " x " << H
              << ", max_iter = " << MAX_ITER
              << ", threads = " << THREADS << "\n";

    // Prepare task counts to test:
    std::vector<size_t> task_counts = {1, 2, 4, 8, 16, 32, 64, 128};
    const int REPEATS = 10;

    if (validate) {
        validate_outputs(W, H, MAX_ITER, task_counts);
    }

    // BS::Thread_Pool benchmark
    benchmark_pool<BS::thread_pool<>>("BS::Thread_Pool", W, H, MAX_ITER,
        task_counts, REPEATS,
        [&](size_t tasks, std::vector<int>& buffer) {
            // Initialize BS pool with `tasks` worker-tasks
            BS::thread_pool pool(tasks);
            // Submit Mandelbrot range
            auto fut = pool.submit_loop(0, buffer.size(),
                [&](size_t idx) {
                    buffer[idx] = mandelbrot_pixel(idx, W, H, MAX_ITER);
                }, tasks);
            fut.wait();
        });

    // SP::ThreadPool benchmark
    SP::ThreadPool::set_thread_count(THREADS);
    SP::ThreadPool::soft_boot();
    benchmark_pool<SP::ThreadPool>("SP::ThreadPool", W, H, MAX_ITER,
        task_counts, REPEATS,
        [&](size_t tasks, std::vector<int>& buffer) {
            auto fut = SP::ThreadPool::submit_task(0, buffer.size(), tasks,
                [&](size_t idx) {
                    buffer[idx] = mandelbrot_pixel(idx, W, H, MAX_ITER);
                });
            fut.get();
        });

    return 0;
}