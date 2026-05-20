# Benchmark Suite: Contrek vs OpenCV

## 1. Project Description
This project was developed to evaluate and compare the performance of **Contrek** against **OpenCV** in the field of contour extraction. The entire system is containerized via Docker and offers two testing modalities:

* **High-Level:** A comparison between the Contrek Ruby extension and OpenCV Python bindings using identical image sets.
* **Low-Level (Native):** A direct C++ comparison to measure the raw efficiency of both processing engines.

Configurations have been calibrated to ensure visually identical results: both engines extract external contours and holes with equivalent topological precision. Users can enable a visual validation flag to generate PNG images of the processed polygons, highlighting external boundaries in red and internal holes in green.

## 2. Philosophy and Objectives

While OpenCV is the industry standard for single-threaded efficiency, Contrek explores a different architectural territory where multi-core parallelism is the priority:

* **A) Single-Image Latency:** While OpenCV excels in *throughput* (processing multiple images simultaneously across different processes), Contrek focuses on minimizing the processing time of a **single ultra-high-resolution image**. By utilizing all available CPU cores through a *Stripe-Merging* algorithm, it significantly reduces end-user latency for gigapixel-scale workloads.
* **B) Memory Efficiency:** Moving away from a strictly monolithic loading approach, Contrek adopts a "streaming-oriented" philosophy. This allows the engine to process extreme-resolution images with a significantly lower and more stable RAM footprint compared to standard methods, making high-end analysis feasible on standard hardware where OpenCV might hit memory limits.

Contrek is not intended as a general-purpose replacement for OpenCV, but rather as a specialized high-performance tool for scenarios where single-image speed and memory scalability are the primary constraints.

### Build and Launch
```bash
# Build the image using Docker Compose
sudo docker compose build test

# Run the container
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
```
This script downloads the source code, compiles it via CMake, and launches the benchmarks. For subsequent runs:

```Bash
cd build
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

### 4. Benchmark Results
The following data was obtained in a virtualized environment (VMware Virtual Machine) featuring an AMD Ryzen 7 3700X 8-Core Processor (BogoMIPS: 7199.99) on an Ubuntu distribution.

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

| Image Target | Res (MP) | Contrek Time | OpenCV Time | Speed Ratio | Contrek RAM | OpenCV RAM | RAM Ratio |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **test_1024x1024** | 1.0 | 18 ms | **13 ms** | 1.39x | **75 MB** | 89 MB | 0.84x |
| **test_4096x4096** | 16.8 | **647 ms** | 842 ms | **0.77x** | **492 MB** | 508 MB | 0.97x |
| **test_10000x10000** | 100.0 | **588 ms** | 646 ms | **0.91x** | **876 MB** | 969 MB | 0.90x |
| **test_10240x10240** | 104.9 | **340 ms** | 479 ms | **0.71x** | 970 MB | 970 MB | 1.00x |
| **test_10240x10240_2**| 104.9 | 507 ms | **426 ms** | 1.19x | 1090 MB | 1090 MB | 1.00x |
| **test_15360x15360** | 235.9 | 938 ms | **903 ms** | 1.04x | **1594 MB** | 2040 MB | 0.78x |
| **test_20480x20480** | 419.4 | **2259 ms** | 2748 ms | **0.82x** | 3379 MB | 3379 MB | 1.00x |

---

**Performance Notes:**
* **Latency Optimization:** Contrek shows a clear advantage in processing time on high-resolution targets (4k, 10k, 20k), proving the effectiveness of the parallel Stripe-Merging approach on single images.
* **Memory Footprint:** On critical workloads like the 15k test (235MP), Contrek saved approximately **446 MB** of RAM compared to OpenCV.
* **Efficiency Balance:** OpenCV remains highly efficient on single-core tasks, while Contrek's multi-threaded architecture shines as image complexity and resolution scale up.
* OpenCV remains the industry standard for general-purpose computer vision. Contrek is a specialized engine designed specifically to optimize latency and memory on extreme-resolution single images.