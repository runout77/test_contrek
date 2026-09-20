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

For the rules that allow independently processed tiles to be correctly merged,
see [The Laws of the Tiles](https://github.com/runout77/contrek#merging-external-geometry).

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

To measure the standalone OpenCV execution time on your machine, run:
```Bash
OPENCV_IO_MAX_IMAGE_PIXELS=2147483647 ./contrek_opencv_benchmark --image test_40960x40960.png
```

For benchmark details and instructions, see [OpenCV CPP vs Contrek CPP comparison](opencv_cpp_vs_contrek_cpp.md)

## How It Works

The following snippets show the main steps of the technique. The complete
working implementation is available in the repository in
`test_opencv_multithread.cpp` source file under the test/ folder.

The image is read progressively in horizontal stripes. Each stripe can be
processed independently by a different worker.

### 1. Process a stripe with OpenCV

A `cv::Mat` is created directly over the stripe bitmap and OpenCV extracts
the contours from that portion of the image.

```cpp
cv::Mat rgba(
  static_cast<int>(buffer_rows), bitmap->w(), CV_8UC4,
  const_cast<unsigned char*>(bitmap->get_row_ptr(0)),
  static_cast<size_t>(bitmap->w()) * bitmap->get_bytes_per_pixel()
);

cv::Mat gray, binary;
cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
cv::threshold(gray, binary, 127, 255, cv::THRESH_BINARY_INV);

std::vector<std::vector<cv::Point>> contours;
std::vector<cv::Vec4i> hierarchy;

cv::findContours(
  binary, contours, hierarchy,
  cv::RETR_CCOMP, cv::CHAIN_APPROX_NONE
);
// Convert the extracted data into a Contrek tile result
ProcessResult* result = make_opencv_result(
  contours,
  hierarchy,
  bitmap->w(),
  buffer_rows,
  options
);
```

### 2. Convert the contours to Contrek geometry

Inside make_opencv_result() OpenCV contour coordinates are converted to the cell-boundary representation
required by the Contrek merger.

```cpp
RectBounds bounds = opencvrect_to_contrekbounds(
  cv::boundingRect(contours[i])
);

poly.outer =
  OpencvConverter::contour_to_cell_boundary(contours[i], bounds);

poly.bounds = bounds;
```

The resulting polygons are stored in a `ProcessResult` describing the
geometry of that stripe.

### 3. Process multiple stripes concurrently

Each stripe is detached from the streaming buffer and submitted to an
independent worker which will return the ProcessResult to submit to the Contrek merger.

```cpp
jobs.push_back({
  current_stripe,
  std::async(std::launch::async,
    [bitmap = std::move(current_bitmap),
     buffer_rows, current_stripe, options]() mutable {
      return process_opencv_stripe(
        std::move(bitmap),
        buffer_rows,
        current_stripe,
        options
      );
    })
});
```

The number of simultaneously active jobs is limited by `max_threads`.

### 4. Merge the partial results

Completed stripe results are progressively passed to the same `VerticalMerger`.

```cpp
ProcessResult* result = job.future.get();

if (result) {
  result_clones.push_back(result);
  vmerger.add_tile(*result);
}
```

Once all stripes have been processed, the complete geometry is produced:

```cpp
ProcessResult* merged_result = vmerger.process_info();
```

OpenCV therefore works only on relatively small independent raster regions,
while Contrek reconstructs their polygon geometry into a single result.

## Streaming Very Large Results

For images producing very large amounts of geometry, retaining all partial
results in memory may become impractical.

The same parallel OpenCV stripe-processing technique can be combined with
Contrek's `SvgStreamingMerger`. In this mode, completed geometry is
progressively merged and written to the SVG output, allowing processed
`ProcessResult` objects to be released instead of being retained until the
entire image has been processed.

This makes it possible to process images whose generated vector output can be
significantly larger than the memory required by the processing pipeline.

### Large-scale test

The streaming implementation can be tested on an **81920 × 81920** image
(**6.71 Gpixels**) containing approximately **20 million contours**, including
inner contours.

To get the sample, run the following commands from the repository root:
```Bash
./scripts/download_test_assets.sh
```
This script will download several very large images into the images/ directory.

Then use the test_opencv_multithread_streaming executable.

```Bash
./test_opencv_multithread_streaming --image high_complexity_81920x81920.png --threads 4
```

The resulting SVG has an exact size of **17,433,275,623 bytes (17.43 GB)**.

| Threads | Execution Time | Peak Memory | SVG Size |
|---:|---:|---:|---:|
| 1 | 962.29 s | 2.04 GB | 17.43 GB |
| 2 | 534.58 s | 3.31 GB | 17.43 GB |
| 4 | **335.39 s** | **5.71 GB** | 17.43 GB |
| 8 | 382.46 s | 9.71 GB | 17.43 GB |

All tested configurations produced **13,133,700 outer polygons**.

With a single worker, a **17.43 GB SVG** was therefore generated while using
only **2.04 GB of peak resident memory**.

Increasing the number of workers trades memory for processing speed. On the
8 core cpu machine used for this test, four workers provided the best
execution time among the tested configurations. Increasing the worker count
from four to eight did not improve throughput and increased peak memory
consumption.

This behavior reflects the structure of the processing pipeline: OpenCV
contour extraction can run concurrently on multiple stripes, while completed
results are progressively consumed by the streaming merger and serialized to
the SVG output.

### How Streaming Works

The OpenCV workers are the same as in the previous example. The main
difference is that completed stripe results are passed to an
`SvgStreamingMerger`, which progressively writes the merged geometry to the
output stream.

```cpp
std::ofstream shared_stream(
  "opencv_streaming.svg",
  std::ios::out | std::ios::binary
);

std::vector<char> output_buffer(4 * 1024 * 1024);
shared_stream.rdbuf()->pubsetbuf(
  output_buffer.data(),
  output_buffer.size()
);

SvgStreamingMerger vmerger(
  0,
  options,
  &shared_stream,
  source.width(),
  source.height()
);
```

As soon as a completed stripe reaches the merger, its `ProcessResult` can be
released:

```cpp
ProcessResult* result = job.future.get();

if (result) {
  processed_rows += job.rows_read;
  const bool last = processed_rows == source.height();

  vmerger.add_tile(*result, last);
  delete result;
}
```

This prevents completed stripe results from accumulating in memory while the
SVG geometry is progressively produced.