# Contrek Python — Concurrent Progressive Streaming Benchmark

This benchmark demonstrates the Python API of **Contrek** on very large raster images.

Contrek is a raster-to-polygon library with a C++ core and Python bindings. The benchmark focuses on a workload where processing the complete decoded raster as a single in-memory bitmap is impractical: the image is decoded progressively into horizontal stripes, polygon extraction runs concurrently, and completed geometries are progressively merged and written to SVG.

## How it works

The pipeline is:

1. **Progressive PNG decoding** — `PngSource` and `RasterStreamer` decode only a horizontal stripe at a time.
2. **Concurrent polygon extraction** — detached stripe buffers are submitted to a Python `ThreadPoolExecutor`. The actual polygon finder runs in C++ and releases the Python GIL while processing.
3. **Bounded work queue** — only a limited number of stripe jobs may remain in flight. Backpressure prevents the producer from decoding and retaining the entire image faster than the workers can consume it.
4. **Topological streaming merge** — `SvgStreamingMerger` reconnects geometries that cross adjacent stripe boundaries.
5. **Progressive SVG output** — geometries that no longer need to remain active are serialized to SVG instead of accumulating the complete vector result in memory.

The important consequence is that memory usage is driven primarily by the active working set — raster stripes, concurrent finder jobs, and geometries whose topology is still open — rather than by the total number of pixels or polygons processed during the complete run.


## Build and Launch via Docker
The entire environment is fully containerized to ensure cross-platform compatibility and reproducible results. Build the system and launch the interactive testing shell using:

```bash
# Build the image using Docker Compose
sudo docker compose build test

# Run and enter the container shell
sudo docker compose run test
```

## Python environment

Once inside the container shell, run the setup script to install Python dependencies:

```bash
./build_python_env.sh
```

The large benchmark uses the image:

```text
high_complexity_81920x81920.png
```

Resolution:

```text
81,920 × 81,920
6,710,886,400 pixels (6.71 Gpx)
```

The image contains a very large number of small shapes and is intended to stress polygon throughput and streamed SVG generation. From the repository root, download the test assets with:

```bash
./scripts/download_test_assets.sh
```

Verify the imported package if necessary with:

```bash
python3 -c "import contrek; print(contrek.__file__)"
```

## Running the benchmark

For one of the smaller test images:

```bash
cd test
python3 progressive_streaming.py --image ../images/test_20480x20480.png
```
The svg file (streaming_buffer.svg) will be created under the test/output directory.
Using the high complexity image

```bash
python3 progressive_streaming.py --image ../images/high_complexity_81920x81920.png
```

Use `--help` to see all available options:

```bash
python3 progressive_streaming.py --help
```


## Reproducibility

The benchmark reports enough information to make runs comparable, including:

- input image and resolution;
- total pixel count;
- stripe height;
- number of processed stripes;
- worker count;
- processed rows;
- execution time;
- peak resident memory;
- SVG output size;
- SHA-256 of the generated SVG.

The SVG hash is particularly useful when comparing different worker counts: identical hashes demonstrate that concurrent processing produced byte-for-byte identical output.

For a reproducibility check, run the same image with different worker counts and compare the reported SHA-256 values:

```bash
python progressive_streaming.py --image ../images/test_20480x20480.png --threads 1
python progressive_streaming.py --image ../images/test_20480x20480.png --threads 4
python progressive_streaming.py --image ../images/test_20480x20480.png --threads 8
```

## Current benchmark results

The following results were obtained using progressive_streaming.py, running inside a Docker container

```text
Machine / CPU:       AMD Ryzen 7 3700X 8-Core Processor
Host RAM:            64 GB
Environment:         Docker container
Input image:         high_complexity_81920x81920.png
Resolution:          81,920 × 81,920
Pixels:              6,710,886,400
Stripe height:       1,000
Workers:             4
Connectivity:        4
Stripes:             83
Polygons:            23,556,642
Execution time:      4 min 27.8 s (267.825 s)
Peak RSS:            5,866.7 MiB (5.73 GiB)
SVG size:            10.89 GiB
SVG SHA-256:         950c35ed6c5c0327c922f3de01b58fcd6a277de07a1f088528759902dca7dfa7
```
