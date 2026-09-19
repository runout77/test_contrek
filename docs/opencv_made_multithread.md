# OpenCV Made Multithread with Contrek

This example demonstrates how to make OpenCV contour extraction work in
parallel by processing independent image stripes and recomposing the resulting
geometry with Contrek.

OpenCV `findContours()` is applied independently to multiple stripes, which
can be processed concurrently by different CPU threads. Contrek then converts the partial contours to its cell-boundary representation
and merges them into the geometry of the complete image.

To make independent OpenCV stripes mergeable, contour coordinates must be
converted to cell-boundary coordinates, as required by Contrek's merging
algorithm. A single OpenCV point may therefore generate up to four boundary
vertices, increasing the number of points to process.

## Build and Launch via Docker
The entire environment is fully containerized to ensure cross-platform compatibility and reproducible results. Build the system and launch the interactive testing shell using:

```bash
# Build the image using Docker Compose
sudo docker compose build test

# Run and enter the container shell
sudo docker compose run test
```


## Build and Run on Your Machine
```Bash
cd test
./cpp_test.sh
cd build
./test_opencv_multithread
```

For subsequent runs:

```Bash
cd build
make -j
./test_opencv_multithread
```
Bigger image test:

```Bash
./test_opencv_multithread --stripe-height 1000 --image test_40960x40960.png --threads 8
```
## Performance

The following results were measured on the same 40960 × 40960 test image.

| Method | Threads | Stripe Height | Execution Time | Peak Memory |
|---|---:|---:|---:|---:|
| OpenCV (full image) | 1 | - | 44.33 s | 9.15 GB |
| Contrek (8 tiles) | 8 | about 5120 | 10.05 s | 12.25 GB |
| OpenCV + Contrek | 8 | 2000 | **11.26 s** | **4.93 GB** |
| OpenCV + Contrek | 8 | 1000 | **10.98 s** | **3.42 GB** |
| OpenCV + Contrek | 8 | 500 | **11.51 s** | **2.87 GB** |

With 1000-row stripes and 8 concurrent workers, the complete pipeline takes
about 11 seconds instead of 44.3 seconds for full-image OpenCV, while peak
memory drops from about 9.2 GB to 3.4 GB.

These results include contour conversion and Contrek merging.