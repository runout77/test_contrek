//https://mapshaper.org/ => viewer
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sys/resource.h>
#include <fstream>
#include <spng.h>
#include <unistd.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <cstdint>
#include <stdexcept>
#include <memory>
#include <cstdio>
#include <cmath>

// Include GDAL & OGR Header
#include "gdal_priv.h"
#include "gdal_alg.h"
#include "ogrsf_frmts.h"

#include "ContrekApi.h"
#include "polygon/finder/concurrent/VerticalMerger.h"
#include "polygon/finder/concurrent/StreamingMerger.h"
#include "polygon/finder/concurrent/SvgStreamingMerger.h"
#include "polygon/finder/concurrent/GeoJsonStreamingMerger.h"
#include "polygon/bitmaps/streaming/PngSource.h"
#include "polygon/bitmaps/streaming/TiffSource.h"
#include "polygon/bitmaps/streaming/RasterStreamer.h"

struct ImageInfo {
  std::string name;
  int w;
  int h;
};

struct BenchResult {
  std::string name;
  double c_time;
  double c_ram;
  int c_external;
  int c_holes;
  double g_time;  
  double g_ram;
  int g_external;
  int g_holes;
  int w;
  int h;
};

struct EngineMetrics {
  double time_ms;
  double ram_mb;
  int external;
  int holes;
};

double get_peak_rss() {
  struct rusage r_usage;
  getrusage(RUSAGE_SELF, &r_usage);
#ifdef __APPLE__
  return r_usage.ru_maxrss / (1024.0 * 1024.0);
#else
  return r_usage.ru_maxrss / 1024.0;
#endif
}

double now_ms() {
  return std::chrono::duration<double, std::milli>(
    std::chrono::high_resolution_clock::now().time_since_epoch()
  ).count();
}

struct PolyMetrics {
  long long external = 0;
  long long holes = 0;
};

PolyMetrics count_polygons_and_holes_gdal(const std::string& geojson_path) {
  PolyMetrics metrics{};
  CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", "0");
  auto* dataset = (GDALDataset*)GDALOpenEx(
    geojson_path.c_str(),
    GDAL_OF_VECTOR | GDAL_OF_READONLY,
    nullptr,
    nullptr,
    nullptr
  );
  if (!dataset) {
    std::cerr << "[ERR] Unable open GeoJSON by GDAL: " << geojson_path << std::endl;
    return metrics;
  }
  OGRLayer* layer = dataset->GetLayer(0);
  if (layer) {
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
      OGRGeometry* geom = feature->GetGeometryRef();
      if (geom != nullptr) {
        OGRwkbGeometryType type = wkbFlatten(geom->getGeometryType());
        if (type == wkbPolygon) {
          auto* poly = (OGRPolygon*)geom;
          metrics.external++;
          metrics.holes += poly->getNumInteriorRings();
        }
        else if (type == wkbMultiPolygon) {
          auto* multiPoly = (OGRMultiPolygon*)geom;
          int subGeomCount = multiPoly->getNumGeometries();
          for (int i = 0; i < subGeomCount; ++i) {
            auto* poly = (OGRPolygon*)multiPoly->getGeometryRef(i);
            if (poly) {
              metrics.external++;
              metrics.holes += poly->getNumInteriorRings();
            }
          }
        }
      }
      OGRFeature::DestroyFeature(feature);
    }
  }
  GDALClose(dataset);
  return metrics;
}

ProcessResult* stream_progressive_png_image(const std::string& filepath, const std::string& geojson_filepath, uint32_t stripe_height) {
  ProcessResult* merged_result = nullptr;

  Options varguments = {
    //{"unsafe_mode", true},
    {"compress", Options{
      {"linear", true},
    }},
  };

  const bool is_tiff = filepath.size() >= 4 &&
    (filepath.substr(filepath.size() - 4) == ".tif" ||
     (filepath.size() >= 5 && filepath.substr(filepath.size() - 5) == ".tiff"));
  std::unique_ptr<RasterSource> source_ptr;
  if (is_tiff) source_ptr = std::make_unique<TiffSource>(filepath, true);
  else source_ptr = std::make_unique<PngSource>(filepath);
  RasterSource& source = *source_ptr;

  if (stripe_height >= source.height()) {
    throw std::invalid_argument("stripe_height must be smaller than image height");
  }

  RasterStreamer streamer(source, stripe_height);

  // allocates stripe buffer
  RawBitmap stripe_bitmap(source.width(), stripe_height);
  RGBNotMatcher not_matcher(-1);

  // allocates streaming buffer
  std::ofstream shared_stream(geojson_filepath, std::ios::out | std::ios::binary);
  if (!shared_stream) {
    std::cerr << "Error: Unable creating output streaming file!" << std::endl;
    return nullptr;
  }

  std::vector<char> buffer(4 * 1024 * 1024);  // Buffer (4MB)
  shared_stream.rdbuf()->pubsetbuf(buffer.data(), buffer.size());

  GeoJsonStreamingMerger vmerger(0, varguments, &shared_stream);

  try {
    uint32_t processed_rows = 0;
    bool first = true;
    int stripe_count = 0;

    // main stripes loop
    streamer.each(stripe_bitmap, [&](Bitmap& bitmap, uint32_t buffer_rows, std::size_t, std::size_t) {
      // stripe contour tracing
      Options finder_options = {
        {"processing_height", static_cast<int>(buffer_rows)},
        {"versus", Identifier{"a"}},
        {"bounds", true},
        {"multiclass", true},
        {"connectivity", 8}
      };

      PolygonFinder polygon_finder(&bitmap, &not_matcher, nullptr, finder_options);
      ProcessResult *result = polygon_finder.process_info();

      if (result) {
        // Temporary: enforce raster scan order
        result->polygons.sort([](const Polygon& a, const Polygon& b) {
          if (a.bounds.min_y != b.bounds.min_y)
            return a.bounds.min_y < b.bounds.min_y;

          return a.bounds.min_x < b.bounds.min_x;
        });

        std::cout << "stripe " << stripe_count << ": found polygons " << result->groups << std::endl;

        processed_rows += buffer_rows - (first ? 0 : RasterStreamer::OVERLAP);
        vmerger.add_tile(*result, processed_rows == source.height());

        delete result;
      }

      stripe_count++;
      first = false;
    });

    merged_result = vmerger.process_info();
    std::cout << "total found polygons " << merged_result->groups << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "\n[ERROR] Processing exception: " << e.what() << std::endl;
    if (shared_stream.is_open()) shared_stream.close();
  }

  return merged_result;
}

EngineMetrics run_contrek_isolated(const std::string& path, const std::string& name) {
  int fd[2];
  pipe(fd);

  pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    EngineMetrics m{};
    const std::string output_path = "./contrek_" + name + ".geojson";
    double start = now_ms();
    auto result = stream_progressive_png_image(path, output_path, 200);
    m.time_ms = now_ms() - start;
    m.ram_mb = get_peak_rss();
    delete result;
    PolyMetrics res = count_polygons_and_holes_gdal(output_path);
    m.external = res.external;
    m.holes = res.holes;
    write(fd[1], &m, sizeof(m));
    close(fd[1]);
    _exit(0);
  }
  close(fd[1]);
  EngineMetrics m{};
  read(fd[0], &m, sizeof(m));
  close(fd[0]);
  waitpid(pid, nullptr, 0);
  return m;
}

EngineMetrics run_gdal_isolated_multiclass(const std::string& path, const std::string& name) {
  int fd[2];
  if (pipe(fd) != 0) {
    throw std::runtime_error("pipe failed");
  }

  pid_t pid = fork();

  if (pid == 0) {
    close(fd[0]);
    EngineMetrics m{};

    const double start_time = now_ms();
    auto* src = (GDALDataset*)GDALOpen(path.c_str(), GA_ReadOnly);
    if (!src) {
      write(fd[1], &m, sizeof(m));
      close(fd[1]);
      _exit(1);
    }

    // Native single-band Byte land-cover raster: no UInt32 expansion or mask.
    // Match Contrek multiclass behavior: include class 0 as well.
    const bool is_tiff_input = path.size() >= 4 &&
        (path.substr(path.size() - 4) == ".tif" ||
         (path.size() >= 5 && path.substr(path.size() - 5) == ".tiff"));
    if (is_tiff_input && src->GetRasterCount() == 1 &&
        src->GetRasterBand(1)->GetRasterDataType() == GDT_Byte) {
      const std::string geojson = "./gdal_multiclass_" + name + ".geojson";
      std::remove(geojson.c_str());
      auto* driver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
      auto* dst = driver ? driver->Create(geojson.c_str(), 0, 0, 0, GDT_Unknown, nullptr) : nullptr;
      if (!dst) {
        GDALClose(src);
        _exit(1);
      }
      OGRLayer* layer = dst->CreateLayer("gdal_contours", nullptr, wkbPolygon, nullptr);
      if (!layer) {
        GDALClose(dst);
        GDALClose(src);
        _exit(1);
      }
      OGRFieldDefn field("PixelVal", OFTInteger);
      if (layer->CreateField(&field) != OGRERR_NONE) {
        GDALClose(dst);
        GDALClose(src);
        _exit(1);
      }
      const int pix = layer->GetLayerDefn()->GetFieldIndex("PixelVal");
      char** opts = nullptr;
      opts = CSLSetNameValue(opts, "8CONNECTED", "8");
      CPLErr err = GDALPolygonize(src->GetRasterBand(1), nullptr,
                                  layer, pix, opts, nullptr, nullptr);
      CSLDestroy(opts);
      GDALClose(dst);
      GDALClose(src);
      m.time_ms = now_ms() - start_time;
      m.ram_mb = get_peak_rss();
      if (err != CE_None) {
        std::cerr << "[ERR] GDALPolygonize failed" << std::endl;
        _exit(1);
      }
      PolyMetrics res = count_polygons_and_holes_gdal(geojson);
      m.external = res.external;
      m.holes = res.holes;
      write(fd[1], &m, sizeof(m));
      close(fd[1]);
      _exit(0);
    }

    const int xsize = src->GetRasterXSize();
    const int ysize = src->GetRasterYSize();
    const int band_count = src->GetRasterCount();

    if (band_count < 1 || band_count > 4) {
      std::cerr << "Unsupported PNG band count: " << band_count << std::endl;
      GDALClose(src);
      _exit(1);
    }

    auto* memDriver = GetGDALDriverManager()->GetDriverByName("MEM");

    auto* colorDS = memDriver->Create("", xsize, ysize, 1, GDT_UInt32, nullptr);
    auto* maskDS  = memDriver->Create("", xsize, ysize, 1, GDT_Byte, nullptr);

    if (!colorDS || !maskDS) {
      if (colorDS) GDALClose(colorDS);
      if (maskDS) GDALClose(maskDS);
      GDALClose(src);
      _exit(1);
    }

    auto* colorBand = colorDS->GetRasterBand(1);
    auto* maskBand  = maskDS->GetRasterBand(1);

    std::vector<GByte> r(xsize), g(xsize), b(xsize), a(xsize);
    std::vector<GUInt32> colorRow(xsize);
    std::vector<GByte> maskRow(xsize);

    bool read_ok = true;

    // Convert PNG pixels to the same logical 32-bit color representation.
    for (int y = 0; y < ysize && read_ok; ++y) {
      auto readBand = [&](int index, std::vector<GByte>& buffer) {
        return src->GetRasterBand(index)->RasterIO(
          GF_Read, 0, y, xsize, 1,
          buffer.data(), xsize, 1, GDT_Byte, 0, 0
        ) == CE_None;
      };

      if (!readBand(1, r)) {
        read_ok = false;
        break;
      }

      if (band_count >= 3) {
        read_ok = readBand(2, g) && readBand(3, b);
      } else {
        g = r;
        b = r;
      }

      if (!read_ok) break;

      if (band_count == 2 || band_count == 4) {
        read_ok = readBand(band_count, a);
      } else {
        std::fill(a.begin(), a.end(), 255);
      }

      if (!read_ok) break;

      for (int x = 0; x < xsize; ++x) {
        colorRow[x] =
          (static_cast<GUInt32>(a[x]) << 24) |
          (static_cast<GUInt32>(r[x]) << 16) |
          (static_cast<GUInt32>(g[x]) << 8)  |
           static_cast<GUInt32>(b[x]);

        // Equivalent to RGBNotMatcher(-1) for opaque white pixels.
        maskRow[x] = (colorRow[x] != 0xFFFFFFFFu) ? 1 : 0;
      }

      read_ok =
        colorBand->RasterIO(
          GF_Write, 0, y, xsize, 1,
          colorRow.data(), xsize, 1, GDT_UInt32, 0, 0
        ) == CE_None &&
        maskBand->RasterIO(
          GF_Write, 0, y, xsize, 1,
          maskRow.data(), xsize, 1, GDT_Byte, 0, 0
        ) == CE_None;
    }

    GDALClose(src);

    if (!read_ok) {
      GDALClose(colorDS);
      GDALClose(maskDS);
      _exit(1);
    }

    const std::string geojson = "./gdal_multiclass_" + name + ".geojson";
    std::remove(geojson.c_str());

    auto* driver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    auto* dst = driver->Create(geojson.c_str(), 0, 0, 0, GDT_Unknown, nullptr);

    if (!dst) {
      GDALClose(colorDS);
      GDALClose(maskDS);
      _exit(1);
    }

    auto* layer = dst->CreateLayer("gdal_contours", nullptr, wkbPolygon, nullptr);

    // UInt32 values can exceed signed 32-bit range.
    OGRFieldDefn field("PixelVal", OFTInteger64);
    layer->CreateField(&field);

    int pix = layer->GetLayerDefn()->GetFieldIndex("PixelVal");

    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "8CONNECTED", "8");

    // Measure polygonization + GeoJSON writing, including dataset closure.
    CPLErr err = GDALPolygonize(
      colorBand,
      maskBand,
      layer,
      pix,
      opts,
      nullptr,
      nullptr
    );

    CSLDestroy(opts);
    GDALClose(dst);

    m.time_ms = now_ms() - start_time;
    m.ram_mb = get_peak_rss();

    GDALClose(colorDS);
    GDALClose(maskDS);

    if (err != CE_None) {
      std::cerr << "[ERR] GDALPolygonize failed" << std::endl;
      _exit(1);
    }

    PolyMetrics res = count_polygons_and_holes_gdal(geojson);
    m.external = res.external;
    m.holes = res.holes;

    write(fd[1], &m, sizeof(m));
    close(fd[1]);
    _exit(0);
  }

  close(fd[1]);

  EngineMetrics m{};
  const ssize_t received = read(fd[0], &m, sizeof(m));
  close(fd[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (received != sizeof(m) ||
      !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    m.time_ms = -1;
    m.ram_mb = -1;
  }

  return m;
}

bool get_png_dimensions(const std::string& filepath, int& width, int& height) {
  std::ifstream file(filepath, std::ios::binary);
  if (!file) return false;
  file.seekg(16);
  uint32_t w_be, h_be;
  file.read(reinterpret_cast<char*>(&w_be), sizeof(w_be));
  file.read(reinterpret_cast<char*>(&h_be), sizeof(h_be));
  if (!file) return false;
  width = static_cast<int>(ntohl(w_be));
  height = static_cast<int>(ntohl(h_be));
  return true;
}


int main(int argc, char* argv[]) {
  std::string image_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " --image FILE\n"
                << "  --image FILE   Raster image in ../../images/ (required)\n"
                << "  --help, -h     Show this help message\n";
      return 0;
    }

    if (arg == "--image") {
      if (i + 1 >= argc || argv[i + 1][0] == '-') {
        std::cerr << "[ERROR] --image requires a filename\n";
        return 1;
      }
      image_path = argv[++i];
    } else {
      std::cerr << "[ERROR] Unknown argument: " << arg << "\n";
      return 1;
    }
  }

  if (image_path.empty()) {
    std::cerr << "[ERROR] Missing required argument: --image\n"
              << "Usage: " << argv[0] << " --image FILE\n";
    return 1;
  }

  GDALAllRegister();

  const std::string path = "../../images/" + image_path;

  auto* info_ds = static_cast<GDALDataset*>(
    GDALOpen(path.c_str(), GA_ReadOnly)
  );

  if (!info_ds) {
    std::cerr << "[ERROR] Cannot open image: " << path << "\n";
    return 1;
  }

  const int width = info_ds->GetRasterXSize();
  const int height = info_ds->GetRasterYSize();
  GDALClose(info_ds);

  std::cout << "Reading: " << path << "\n"
            << "Image dimensions: " << width << "x" << height << " px\n";

  std::cout << "=== Processing " << path << " ===\n";

  auto contrek = run_contrek_isolated(path, image_path);
  auto gdal = run_gdal_isolated_multiclass(path, image_path);

  std::vector<BenchResult> results;
  results.push_back({
    image_path,
    contrek.time_ms, contrek.ram_mb,
    contrek.external, contrek.holes,
    gdal.time_ms, gdal.ram_mb,
    gdal.external, gdal.holes,
    width, height
  });

  // --- REPORT HTML ---


  // --- REPORT HTML ---
  std::ofstream html("cpp_geojson_benchmark_results.html");

  html << "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
       << "<style>"
       << "body { font-family: 'Segoe UI', sans-serif; background: #f0f2f5; padding: 30px; }"
       << "table { border-collapse: collapse; width: 100%; background: white; border-radius: 8px; overflow: hidden; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }"
       << "th, td { padding: 10px 6px; text-align: center; border-bottom: 1px solid #eee; font-size: 12px; }"
       << "th { background-color: #232f3e; color: white; text-transform: uppercase; font-size: 10px; letter-spacing: 1px; text-align: center; }"
       << ".img-name { text-align: left; font-weight: bold; color: #1a73e8; font-size: 11px; }"
       << ".contrek-col { background-color: #f1f8ff; }"
       << ".gdal-col { background-color: #fcf2ff; }"
       << ".setup-cell { text-align: center; font-family: monospace; color: #666; font-weight: bold; }"
       << ".winner-label { font-weight: bold; text-align: center; font-size: 9px; padding: 2px 4px; border-radius: 4px; }"
       << ".winner-ct { background: #d4edda; color: #155724; box-shadow: 0 0 6px 3px rgba(46, 204, 113, 0.3);}"
       << ".winner-gd { background: #f8d7da; color: #721c24; }"
       << ".check { display:inline-block; color:#2ecc71; font-weight:bold; font-size:18px; text-align:center; margin-left:4px; }"
       << ".ratio-pill { padding: 4px 8px; border-radius: 12px; font-size: 11px; font-weight: bold; display: inline-block; min-width: 40px; }"
       << ".good { background: #2ecc71; color: white; }"
       << ".bad { background: #e67e22; color: white; }"
       << "</style></head><body>";

  html << "<h1 style='color: #232f3e;'>Contrek vs GDAL: Native Benchmark Report</h1>";
  html << "<table><thead>";
  html << "<tr><th rowspan='2'>Target Image</th><th rowspan='2'>MP</th>"
       << "<th colspan='6'>CONTREK ENGINE</th>"
       << "<th colspan='6'>GDAL ENGINE (GDALPolygonize)</th>"
       << "<th colspan='2'>RATIOS</th></tr>";
  html << "<tr><th>Ext.</th><th>Hole</th><th>Time</th><th>Time Better?</th><th>RAM</th><th>Ram Better?</th>"
       << "<th>Ext.</th><th>Hole</th><th>Time</th><th>Time Better?</th><th>RAM</th><th>Ram Better?</th>"
       << "<th>Contrek Speed</th><th>Contrek RAM Usage</th></tr>";
  html << "</thead><tbody>";

  for (const auto& r : results) {
    double mp = (static_cast<double>(r.w) * r.h) / 1000000.0;
    double speed_ratio = r.c_time / r.g_time;
    double ram_ratio   = r.c_ram / r.g_ram;
    
    bool ct_speed_win = r.c_time < r.g_time;
    bool ct_ram_win   = r.c_ram < r.g_ram;

    html << "<tr>";
    html << "<td class='img-name'>" << r.name << "</td>";
    html << "<td>" << std::fixed << std::setprecision(1) << mp << "</td>";
    
    // --- CONTREK STATS ---
    html << "<td class='contrek-col'>" << r.c_external << "</td>";
    html << "<td class='contrek-col'>" << r.c_holes << "</td>";
    html << "<td class='contrek-col'>" << (int)r.c_time << "ms</td>";
    html << "<td class='contrek-col'>" << (ct_speed_win ? "<span class='winner-label winner-ct check'>\u2713</span>" : "-") << "</td>";
    html << "<td class='contrek-col'>" << (int)r.c_ram << "MB</td>";
    html << "<td class='contrek-col'>" << (ct_ram_win ? "<span class='winner-label winner-ct check'>\u2713</span>" : "-") << "</td>";

    // --- GDAL STATS ---
    html << "<td class='gdal-col'>" << r.g_external << "</td>";
    html << "<td class='gdal-col'>" << r.g_holes << "</td>";
    html << "<td class='gdal-col'>" << (int)r.g_time << "ms</td>";
    html << "<td class='gdal-col'>" << (!ct_speed_win ? "<span class='winner-label winner-gd'>\u2713</span>" : "-") << "</td>";
    html << "<td class='gdal-col'>" << (int)r.g_ram << "MB</td>";
    html << "<td class='gdal-col'>" << (!ct_ram_win ? "<span class='winner-label winner-gd'>\u2713</span>" : "-") << "</td>";

    // --- RATIOS ---
    if (r.c_time > 0 && r.g_time > 0) {
      bool contrek_faster = r.c_time < r.g_time;
      double ratio = contrek_faster
        ? r.g_time / r.c_time
        : r.c_time / r.g_time;

      html << "<td><span class='ratio-pill "
           << (contrek_faster ? "good" : "bad") << "'>"
           << std::fixed << std::setprecision(2)
           << ratio
           << (contrek_faster ? "&times; faster" : "&times; slower")
           << "</span></td>";
    } else {
      html << "<td>N/A</td>";
    }

    if (r.c_ram > 0 && r.g_ram > 0) {
      double saving = (1.0 - r.c_ram / r.g_ram) * 100.0;

      html << "<td><span class='ratio-pill "
           << (saving >= 0 ? "good" : "bad") << "'>"
           << std::fixed << std::setprecision(1)
           << std::abs(saving)
           << (saving >= 0 ? "% less RAM" : "% more RAM")
           << "</span></td>";
    } else {
      html << "<td>N/A</td>";
    }

  }

  html << "</tbody></table></body></html>";
  html.close();
  
  std::cout << "\n[OK] Benchmark done! Report data to 'cpp_geojson_benchmark_results.html'" << std::endl;
}
