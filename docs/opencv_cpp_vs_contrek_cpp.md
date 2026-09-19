# OpenCV CPP vs Contrek CPP Comparison

The entire system is containerized via Docker and offers a Low-Level direct C++ comparison to measure the raw efficiency of both processing engines. (OpenCV is compiled from source, version 4.10.0 in Release with -O3 -Ofast -march=native -flto -DNDEBUG flags)

Configurations have been calibrated to ensure visually identical results: both engines extract external contours and holes with equivalent topological precision. Users can enable a visual validation flag to generate PNG images of the processed polygons, highlighting external boundaries in red and internal holes in green.

> 📂 **Benchmark Sources Included:** The complete source code for all benchmark implementations—including the native C++ test runners are included in this repository for maximum transparency and reproducible results.

> **Note:** Contrek uses multiple threads while OpenCV `findContours` is single-threaded. This is intentional: the goal of this benchmark is to compare the maximum performance each engine can achieve on the same hardware, not their single-thread performance.


### Build and Launch via Docker
The entire environment is fully containerized to ensure cross-platform compatibility and reproducible results. Build the system and launch the interactive testing shell using:

```bash
# Build the image using Docker Compose
sudo docker compose build test

# Build again ignoring cache
sudo docker compose build --no-cache test

# Run and enter the container shell
sudo docker compose run test
```

### Executing Low-Level Tests (Native C++)
For a direct comparison between the C++ cores:

```Bash
cd test
./cpp_test.sh
cd build
./contrek_opencv_benchmark
```
This script downloads the source code, compiles it via CMake, and launches the benchmarks. Will create an html report cpp_benchmark_results.html under build directory.
For subsequent runs:

```Bash
cd build
make -j
./contrek_opencv_benchmark
```
Note: It is recommended to run the tests multiple times; initial runs may be slower due to library memory allocation and caching.

Run the benchmark using hierarchical contour retrieval mode.

| Flag | OpenCV mode | Contrek flag |
|------|-------------|--------------|
| *(absent)* | `cv::RETR_CCOMP` | `cfg.treemap = false` |
| `--tree` | `cv::RETR_TREE` | `cfg.treemap = true` |

**Usage:**
```bash
./contrek_opencv_benchmark --tree    # RETR_TREE + cfg.treemap=true
```

In `RETR_CCOMP` mode contours are organized in a two-level hierarchy (external + holes).
In `RETR_TREE` mode the full parent-child nesting tree is reconstructed.

### Very large dataset
You can try massive images using the --image option (image are read from /images root directory)

```bash
OPENCV_IO_MAX_IMAGE_PIXELS=2147483647 ./contrek_opencv_benchmark --image test_40960x40960.png
```

### OpenCV Version and Build Infos

```Bash
./contrek_opencv_benchmark --info
```

### Native Benchmark Results: Contrek vs OpenCV
*Environment: Native C++ Engine | Configuration: 8 Threads / 8 Tiles*

👉 [Native Benchmarks table of results](https://runout77.github.io/test_contrek/cpp_benchmark_results.html)


### Tested Configuration

- CPU: AMD Ryzen 7 3700X
- Cores / Threads: 8 / 16
- OS: Ubuntu
- Contrek threads: 8
- Contrek tiles: 8

---

### Benchmark Methodology (Cold vs. Warm Runs)

To ensure maximum accuracy, eliminate OS thread scheduling noise, and bypass transient caching effects, the benchmark is executed **11 consecutive times**.

[Live report](https://runout77.github.io/test_contrek/multiple_runs.html)
