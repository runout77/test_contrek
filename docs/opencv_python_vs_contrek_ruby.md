# OpenCV Python vs Contrek Ruby Comparison

The entire system is containerized via Docker and offers a high level comparison between the Contrek Ruby extension and OpenCV Python bindings using identical image sets.

Configurations have been calibrated to ensure visually identical results: both engines extract external contours and holes with equivalent topological precision. Users can enable a visual validation flag to generate PNG images of the processed polygons, highlighting external boundaries in red and internal holes in green.

> 📂 **Benchmark Sources Included:** The complete source code for all benchmark implementations—including the Ruby and Python-OpenCV scripts—is fully included in this repository for maximum transparency and reproducible results.

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
./build_ruby_env.sh
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

### Very large dataset
You can try massive images using the --image option (image are read from /images root directory)

```bash
OPENCV_IO_MAX_IMAGE_PIXELS=2147483647 python3 test_opencv.py --image test_40960x40960.png
ruby test_contrek.rb --image test_40960x40960.png
```

### Visual Validation:
To verify the precision of the results graphically, add the --draw flag:

```Bash
ruby test_contrek.rb --draw
python3 test_opencv.py --draw
```
The resulting images will be saved in the **test/output** directory. This process may take several minutes on the ruby side.

### Treemaps compare script
A ruby script to compare treemaps is provided: `compare_treemaps.rb`


## Benchmark Results
The following data was obtained on an AMD Ryzen 7 3700X 8-Core Processor (BogoMIPS: 7199.99) with 64 GB on an Ubuntu distribution.

### High-Level Benchmark Results (Ruby vs Python)
*Test environment: Ruby (Contrek) vs Python (OpenCV)*

| Image Name | Resolution | Python (OpenCV) | Ruby (Contrek) | Polylines (Outer/Inner) |
| :--- | :--- | :--- | :--- | :--- |
| **test_40960x40960** | 40960x40960 | 40.302 s | **15.460 s** | 2488 / 514758 |
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
