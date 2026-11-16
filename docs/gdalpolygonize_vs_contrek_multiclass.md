# GDALPolygonize vs Contrek — Multiclass Benchmark

This benchmark compares **Contrek** and **GDALPolygonize** when extracting polygons for **all raster classes in a single run**. Unlike the [single-class benchmark](gdalpolygonize_vs_contrek.md), these tests use real land-cover GeoTIFFs and retain the source pixel values as polygon classes.

Contrek uses a stripe-based, progressive contour extraction and merging pipeline. The goal is not to replace GDAL's broader geospatial functionality, but to evaluate polygonization speed and peak memory usage on large classified rasters.


## Test configuration

- **Input:** single-band, byte-valued, classified GeoTIFFs (including palette-indexed rasters).
- **Classes:** all pixel values, rather than a single selected class.
- **Connectivity:** 8-connected in both implementations (`8CONNECTED=8` for GDAL).
- **GDAL version:** 3.11.4
- **Contrek version:** 1.4.8
- **Contrek:** `multiclass: true`, stripe height **200 pixels**; adjacent stripes share the required boundary row.
- **Output:** GeoJSON polygons with class values and holes.
- **Timing:** wall-clock execution time, in seconds.
- **Memory:** peak resident memory, in MB.

## Run the benchmarks

```bash
# Build the image using Docker Compose
sudo docker compose build test

# Run and enter the container shell
sudo docker compose run test
```

## Download the datasets

From the project root directory, run:

```bash
# cd into images root directory
cd images

# ESA WorldCover 2021 v200 — Italy (36,000 × 36,000)
wget -c 'https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_N42E009_Map.tif'

# ESA WorldCover 2021 v200 — Amazon (36,000 × 36,000)
wget -c 'https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_S03W060_Map.tif'

# NOAA C-CAP 2021 — full CONUS source raster (154,951 x 97,470 about 15.1 Gpx)
wget -c 'https://ocmgeodatastor1.blob.core.windows.net/ccap/bulk_download/C-CAP_Regional_30-meter_Data/C-CAP_Regional_Land_Cover_Classification/CONUS/conus_2021_ccap_landcover.tif'
```
Data sources
ESA WorldCover: https://esa-worldcover.org/en/data-access  
NOAA C-CAP: https://coast.noaa.gov/digitalcoast/data/ccapregional.html

Check the raster dimensions and bands before running:

```bash
gdalinfo ESA_WorldCover_10m_2021_v200_N42E009_Map.tif
gdalinfo ESA_WorldCover_10m_2021_v200_S03W060_Map.tif
gdalinfo conus_2021_ccap_landcover.tif
```

Build the benchmark executable **gdal_test_multiclass.cpp** from the test directory.
From the project root directory, run:

```bash
cd test
./cpp_test.sh
cd build
./gdal_test_multiclass --help
```

Or, if the project is already built:

```bash
cd test/build
make -j
./gdal_test_multiclass --help
```

## Run benchmarks

The following commands polygonize **all source band values** with 8-connectivity. The `class_id` field stores the original raster class value. From the project root directory, run:

```bash
cd test
./cpp_test.sh
cd build
./gdal_test_multiclass --image ESA_WorldCover_10m_2021_v200_N42E009_Map.tif
```

The executable generates an HTML report named **cpp_geojson_benchmark_results.html** and two geojson files, one for Contrek and one for GDAL.

## Performance results (AMD Ryzen 7 3700X 8 cores / 16 thread, 3.6 HHz)

The generated HTML report is available **[HERE](https://runout77.github.io/test_contrek/cpp_geojson_benchmark_results_multiclass.html)**


## Small-raster topology validation

A small crop makes it practical to inspect individual polygons, holes and diagonal connections. The following command creates a **360 × 360 pixel GeoTIFF** from the downloaded WorldCover tile:
From the project root directory, run:

```bash
cd images/
gdal_translate -of GTiff \
  -projwin 10.235 44.085 10.265 44.055 \
  ESA_WorldCover_10m_2021_v200_N42E009_Map.tif \
  apuane_debug.tif

gdalinfo apuane_debug.tif
cd ../test
./cpp_test.sh
cd build
./gdal_test_multiclass --image apuane_debug.tif
```

Both implementations produced 443 exterior polygons and 280 holes across 6 raster classes.
After transforming GDAL's georeferenced coordinates back to pixel coordinates, all 443 polygons were found to be topologically equivalent, including their holes. Shapely flagged the same 48 geometries as invalid in both outputs.