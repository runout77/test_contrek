# GDALPolygonize vs Contrek — Multiclass Benchmark

This benchmark compares **Contrek** and **GDALPolygonize** when extracting polygons for **all raster classes in a single run**. Unlike the [single-class benchmark](gdalpolygonize_vs_contrek.md), these tests use real land-cover GeoTIFFs and retain the source pixel values as polygon classes.

Contrek uses a stripe-based, progressive contour extraction and merging pipeline. The goal is not to replace GDAL's broader geospatial functionality, but to evaluate polygonization speed and peak memory usage on large classified rasters.

## Test configuration

- **Input:** single-band, byte-valued, classified GeoTIFFs (including palette-indexed rasters).
- **Classes:** all pixel values, rather than a single selected class.
- **Connectivity:** 8-connected in both implementations (`8CONNECTED=8` for GDAL).
- **Contrek:** `multiclass: true`, stripe height **200 pixels**; adjacent stripes share the required boundary row.
- **Output:** GeoJSON polygons with class values and holes.
- **Timing:** wall-clock execution time, in seconds.
- **Memory:** peak resident memory, in MB.

These are measurements from the test runs described below, **not** an assertion of hardware-independent performance. For a strict reproduction, record CPU, RAM, operating system, compiler, GDAL and Contrek versions, and use the same build and output-storage configuration. The exact benchmark harness revision and machine specification for these runs are not recorded here.

## Download the datasets

Requirements: `wget`, GDAL command-line utilities (`gdalinfo`, `gdal_translate`, `gdal_polygonize.py`), and sufficient disk space for the generated GeoJSON files.

```bash
cd images

# ESA WorldCover 2021 v200 — Italy (36,000 × 36,000)
wget -c 'https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_N42E009_Map.tif'

# ESA WorldCover 2021 v200 — Amazon (36,000 × 36,000)
wget -c 'https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_S03W060_Map.tif'

# NOAA C-CAP 2021 — full CONUS source raster
wget -c 'https://ocmgeodatastor1.blob.core.windows.net/ccap/bulk_download/C-CAP_Regional_30-meter_Data/C-CAP_Regional_Land_Cover_Classification/CONUS/conus_2021_ccap_landcover.tif'
```

ESA WorldCover: https://esa-worldcover.org/en/data-access  
NOAA C-CAP: https://coast.noaa.gov/digitalcoast/data/ccapregional.html



Check the raster dimensions and bands before running:

```bash
gdalinfo ESA_WorldCover_10m_2021_v200_N42E009_Map.tif
gdalinfo ESA_WorldCover_10m_2021_v200_S03W060_Map.tif
gdalinfo conus_center_quarter.tif
```

## Run benchmarks

The following commands polygonize **all source band values** with 8-connectivity. The `class_id` field stores the original raster class value.

```bash
cd test
./cpp_test.sh
cd build
./gdal_test_multiclass --image ESA_WorldCover_10m_2021_v200_N42E009_Map.tif
```

**Important:** these are convenient standalone GDAL commands. The reported results below came from the project's C++ comparison harness using GDALPolygonize on the source byte band. Standalone Python CLI overhead, GDAL version, output driver behavior and disk speed can affect timing. Do not treat these commands as guaranteed to reproduce the exact recorded seconds.

For Contrek, use the repository's multiclass-enabled C++ benchmark harness (`test/gdal_test.cpp` in the version used for these measurements), with `multiclass: true`, `connectivity: 8`, and stripe height `200`. The existing [single-class instructions](gdalpolygonize_vs_contrek.md) describe the Docker/build environment, but its default invocation is **not automatically a multiclass reproduction**. Verify that the checked-out harness actually enables multiclass mode and uses the same input files before comparing timings.

## Performance results

| Dataset | Raster size | Contrek time | GDAL time | Contrek speedup | Contrek peak RAM | GDAL peak RAM | RAM reduction |
|---|---:|---:|---:|---:|---:|---:|---:|
| ESA WorldCover — Italy | 36,000 × 36,000 | 63.56 s | 919.52 s | **14.47×** | 781 MB | 9,600 MB | **91.9%** |
| ESA WorldCover — Amazon | 36,000 × 36,000 | 13.87 s | 83.34 s | **6.01×** | 309 MB | 2,216 MB | **86.1%** |
| NOAA C-CAP — CONUS central quarter | 77,475 × 48,735 | 79.35 s | 543.18 s | **6.85×** | 689 MB | 2,279 MB | **69.8%** |

### Polygon and hole counts

| Dataset | Contrek polygons | GDAL polygons | Difference | Contrek holes | GDAL holes | Difference |
|---|---:|---:|---:|---:|---:|---:|
| Italy | 3,075,841 | 3,075,836 | +5 | 1,884,009 | 1,884,003 | +6 |
| Amazon | 351,857 | 351,857 | **0** | 286,500 | 286,500 | **0** |
| CONUS central quarter | 9,657,514 | 9,657,488 | +26 | 2,721,090 | 2,721,047 | +43 |

Counts match exactly on the Amazon dataset. Small differences remain on the Italy and CONUS datasets and **have not yet been explained**. Polygon/hole counts alone do not establish full geometric equivalence.

### GeoJSON output size

| Dataset | Contrek output | GDAL output |
|---|---:|---:|
| Italy | 1.759 GB | Not verified |
| Amazon | 280.43 MB | Not verified |
| CONUS central quarter | 2.975 GB | 8.877 GB |

Output size is not a direct measure of polygonization efficiency: GDAL's GeoJSON uses georeferenced coordinates while the tested Contrek output uses pixel-edge coordinates. Serialization choices and coordinate precision affect file size.

## Small-raster topology validation (300 × 300)

A small crop makes it practical to inspect individual polygons, holes and diagonal connections. The following command creates a **300 × 300 pixel GeoTIFF** from the downloaded Italy WorldCover tile:

```bash
# From benchmark_data/
cd images/
gdal_translate -of GTiff \
  -srcwin 15000 15000 300 300 \
  ESA_WorldCover_10m_2021_v200_N42E009_Map.tif \
  italy_debug_300x300.tif

gdalinfo italy_debug_300x300.tif
```

```bash
cd test
./cpp_test.sh
cd build
./gdal_test_multiclass --image italy_debug_300x300.tif
```