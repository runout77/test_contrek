# GDAL Polygonize vs Contrek benchmark

This benchmark suite evaluates Contrek against GDALPolygonize on single-class polygonization, comparing the core contour-tracing algorithms under equivalent conditions.

Contrek uses a progressive streaming architecture. It's not a replacement for GDAL — it's a different approach aimed at specific scenarios: very large rasters and specialized processing pipelines where streaming matters.

This is a proof-of-concept implementation. There's room for improvement, but the benchmark shows the algorithm scales well on large datasets.

Contrek's core is matcher-based: a matcher recognizes arbitrary pixel patterns. These benchmarks use a single matcher (one target class), but the same engine could handle multiple classes simultaneously or custom extraction logic — no changes to the core required.

Contributions, discussions, and ideas for improvement are welcome.

## Run the benchmarks

```bash
# Build the image using Docker Compose
sudo docker compose build test

# Run and enter the container shell
sudo docker compose run test
```

```bash
cd test
./cpp_test.sh
cd build
./gdal_test
```

Or, if the project is already built:

```bash
cd build
make -j
./gdal_test
```

The default benchmark suite compares Contrek and GDAL on **seven PNG datasets**, ranging from **4096×4096** to **40960×40960** pixels, and typically completes in **less than 10 minutes** on the reference test machine.

Heavier datasets can be executed individually using the `--image` option:

```bash
./gdal_test --help
./gdal_test --image test_10000x10000.png
./gdal_test --image test_81920x81920.png
./gdal_test --image high_complexity_81920x81920.png
```

## Benchmark results

The generated HTML report is available **[HERE](https://runout77.github.io/test_contrek/cpp_geojson_benchmark_results.html)**