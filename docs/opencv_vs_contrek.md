# OpenCV vs Contrek Comparison

The entire system is containerized via Docker and offers two testing modalities:

* **High-Level:** A comparison between the Contrek Ruby extension and OpenCV Python bindings using identical image sets.
* **Low-Level (Native):** A direct C++ comparison to measure the raw efficiency of both processing engines. (OpenCV is compiled from source, version 4.10.0 in Release with -O3 -Ofast -march=native -flto -DNDEBUG flags)

Configurations have been calibrated to ensure visually identical results: both engines extract external contours and holes with equivalent topological precision. Users can enable a visual validation flag to generate PNG images of the processed polygons, highlighting external boundaries in red and internal holes in green.

> 📂 **Benchmark Sources Included:** The complete source code for all benchmark implementations—including the native C++ test runners, Ruby and Python-OpenCV scripts—is fully included in this repository for maximum transparency and reproducible results.

## Philosophy and Objectives

While OpenCV is the industry-standard computer vision framework and remains highly optimized for many contour extraction workloads, Contrek explores a different architectural territory where multi-core parallelism is the primary focus.

* **A) Single-Image Latency:** While OpenCV excels in *throughput* (processing multiple images simultaneously across different processes), Contrek focuses on minimizing the processing time of a **single ultra-high-resolution image**. By utilizing all available CPU cores through a *Stripe-Merging* algorithm, it significantly reduces end-user latency for gigapixel-scale workloads.
* **B) Memory Efficiency:** Moving away from a strictly monolithic loading approach, Contrek adopts a "streaming-oriented" philosophy. This allows the engine to process extreme-resolution images with a significantly lower and more stable RAM footprint compared to standard methods, making high-end analysis feasible on standard hardware where OpenCV might hit memory limits.

Contrek is not intended as a general-purpose replacement for OpenCV, but rather as a specialized high-performance tool for scenarios where single-image speed and memory scalability are the primary constraints.

## Setup and Execution

### Build and Launch via Docker
The entire environment is fully containerized to ensure cross-platform compatibility and reproducible results. Build the system and launch the interactive testing shell using:

```bash
# Build the image using Docker Compose
sudo docker compose build test

# Run and enter the container shell
sudo docker compose run test
```

### Internal Configuration
Once inside the container shell, run the setup script to install Ruby dependencies:

```bash
./build.sh
```
To ensure you are aligned with the latest core updates, it is recommended to run gem update contrek.
```bash
gem update contrek
```

### Executing High-Level Tests (Ruby vs Python)
Navigate to the test directory and run the benchmarks:

```Bash
cd test
ruby test_contrek.rb
python3 test_opencv.py
```
Results will be aggregated into a **test/report.html** file.

See other options by
```Bash
ruby test_contrek.rb --help
```

The Python script supports the --tree option too which uses cv2.RETR_TREE in place of cv2.RETR_CCOMP (similar to the Contrek's `treemap: true` flag).

### Visual Validation:
To verify the precision of the results graphically, add the --draw flag:

```Bash
ruby test_contrek.rb --draw
python3 test_opencv.py --draw
```
The resulting images will be saved in the **test/output** directory. This process may take several minutes on the ruby side.

### Treemaps compare script
A ruby script to compare treemaps is provided: `compare_treemaps.rb`

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

## Benchmark Results
The following data was obtained on an AMD Ryzen 7 3700X 8-Core Processor (BogoMIPS: 7199.99) with 64 GB on an Ubuntu distribution.

### 📊 High-Level Benchmark Results (Ruby vs Python)
*Test environment: Ruby (Contrek) vs Python (OpenCV)*

| Image Name | Resolution | Python (OpenCV) | Ruby (Contrek) | Polylines (Outer/Inner) |
| :--- | :--- | :--- | :--- | :--- |
| **test_20480x20480** | 20480x20480 | 3.354 s | 4.383 s | 625 / 128689 |
| **test_15360x15360** | 15360x15360 | 1.100 s | 1.596 s | 2447 / 5716 |
| **test_10240x10240_2**| 10240x10240 | 0.542 s | 0.915 s | 2447 / 5716 |
| **test_10240x10240** | 10240x10240 | 0.566 s | **0.468 s** | 219 / 2259 |
| **test_10000x10000** | 10000x10000 | 0.706 s | 0.794 s | 806 / 371 |
| **test_4096x4096** | 4096x4096 | 1.292 s | **0.998 s** | 625 / 128689 |
| **test_1024x1024** | 1024x1024 | 0.023 s | 0.049 s | 219 / 2259 |

**Performance Notes:**
* In high-density **4k** and **10k** tests, the Contrek Ruby extension outperforms OpenCV's Python bindings despite language overhead, thanks to parallel thread management (8 threads / 8 tiles).
* Results confirm that the precision of the extracted polygons is nearly identical between the two systems.


### 🚀 Native Benchmark Results: Contrek vs OpenCV
*Environment: Native C++ Engine | Configuration: 8 Threads / 8 Tiles*

👉 [Native Benchmarks table of results](https://runout77.github.io/test_contrek/cpp_benchmark_results.html)


### Tested Configuration

- CPU: AMD Ryzen 7 3700X
- Cores / Threads: 8 / 16
- OS: Ubuntu
- Contrek threads: 8
- Contrek tiles: 8

---

### 📂 Benchmark Methodology (Cold vs. Warm Runs)

To ensure maximum accuracy, eliminate OS thread scheduling noise, and bypass transient caching effects, the benchmark is executed **11 consecutive times**.


🌐 [Live report](https://runout77.github.io/test_contrek/multiple_runs.html)

