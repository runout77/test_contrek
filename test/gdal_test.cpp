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
#include <nlohmann/json.hpp>
#include <arpa/inet.h>

// Include GDAL & OGR Header
#include "gdal_priv.h"
#include "gdal_alg.h"
#include "ogrsf_frmts.h"

#include "ContrekApi.h"
#include "polygon/finder/concurrent/VerticalMerger.h"
#include "polygon/finder/concurrent/StreamingMerger.h"
#include "polygon/finder/concurrent/SvgStreamingMerger.h"
#include "polygon/finder/concurrent/GeoJsonStreamingMerger.h"

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

ProcessResult* stream_progressive_png_image(const std::string& filepath, const std::string& geojson_filepath, uint32_t stripe_height) {
  ProcessResult* merged_result = nullptr;
  Options varguments = {
    {"compress", Options{
      {"linear", true},
    }},
  };
  // opens image to stream
  FILE* fp = fopen(filepath.c_str(), "rb");
  if (!fp) {
    std::cerr << "Unable open file: " << filepath << std::endl;
    return nullptr;
  }
  // exams image
  spng_ctx *ctx = spng_ctx_new(0);
  spng_set_png_file(ctx, fp);
  struct spng_ihdr ihdr;
  if (spng_get_ihdr(ctx, &ihdr)) {
    fclose(fp);
    spng_ctx_free(ctx);
    return nullptr;
  }
  uint32_t total_width = ihdr.width;
  uint32_t total_height = ihdr.height;
  if (stripe_height >= total_height) {
    spng_ctx_free(ctx);
    fclose(fp);
    throw std::invalid_argument("stripe_height must be smaller than image height");
  }
  // allocates stripe buffer
  uint32_t stripe_bitmap_height = std::min(stripe_height, total_height);
  RawBitmap stripe_bitmap;
  stripe_bitmap.define(total_width, stripe_bitmap_height, 4, true);
  RGBNotMatcher not_matcher(-1);
  if (spng_decode_image(ctx, NULL, 0, SPNG_FMT_RGBA8, SPNG_DECODE_PROGRESSIVE)) {
    fclose(fp);
    spng_ctx_free(ctx);
    return nullptr;
  }
  // allocates streaming svg buffer
  std::ofstream shared_stream(geojson_filepath, std::ios::out | std::ios::binary);
  if (!shared_stream) {
    std::cerr << "Error: Unable creating output streaming file!" << std::endl;
  }
  std::vector<char> buffer(4 * 1024 * 1024);  // Buffer (4MB)
  shared_stream.rdbuf()->pubsetbuf(buffer.data(), buffer.size());

  GeoJsonStreamingMerger vmerger(0, varguments, &shared_stream,0);
  try {
    size_t row_size = static_cast<size_t>(total_width) * 4;
    int stripe_count = 0;
    uint32_t current_y_offset = 0;
    // main stripes loop
    while (current_y_offset < total_height) {
      uint32_t first_line = current_y_offset == 0 ? 0 : 1;
      uint32_t lines_to_read = std::min(
        stripe_height - first_line,
        total_height - current_y_offset
      );
      uint32_t current_stripe_height = first_line + lines_to_read;
      std::vector<unsigned char> overlap_row;
      if (current_y_offset > 0) {
        overlap_row.resize(row_size);
        const unsigned char* last_row_prev =
          stripe_bitmap.get_row_ptr(stripe_bitmap_height - 1);
        std::memcpy(overlap_row.data(), last_row_prev, row_size);
      }
      if (current_stripe_height != stripe_bitmap_height) {
        stripe_bitmap.define(total_width, current_stripe_height, 4, true);
        stripe_bitmap_height = current_stripe_height;
      }
      // copy previous last line to the next new one (each contigue stripe must share one pixel scanline)
      if (current_y_offset > 0) {
        unsigned char* first_row_curr =
          const_cast<unsigned char*>(stripe_bitmap.get_row_ptr(0));
        std::memcpy(first_row_curr, overlap_row.data(), row_size);
      }
      // decoding data directly in the stripe buffer
      for (uint32_t y = first_line; y < first_line + lines_to_read; y++) {
        unsigned char* row_ptr = const_cast<unsigned char*>(stripe_bitmap.get_row_ptr(y));
        int ret = spng_decode_row(ctx, row_ptr, row_size);
        if (ret != 0 && ret != SPNG_EOI) break;
      }
      // stripe contour tracing
      Options finder_options = {
        {"versus", Identifier{"a"}},
        {"bounds", true},
        {"connectivity", 8}
      };
      PolygonFinder polygon_finder(&stripe_bitmap, &not_matcher, nullptr, finder_options);
      ProcessResult *result = polygon_finder.process_info();
      if (result) {
        //std::cout << "stripe " << stripe_count << ": found polygons " << result->groups << std::endl;
        vmerger.add_tile(*result, current_y_offset + lines_to_read >= total_height);
        delete result;
      }
      current_y_offset += lines_to_read;
      stripe_count++;
    }
    merged_result = vmerger.process_info();
    //std::cout << "total found polygons " << merged_result->groups << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "\n[ERROR] Processing exception: " << e.what() << std::endl;
    if (shared_stream.is_open()) shared_stream.close();
  }
  spng_ctx_free(ctx);
  fclose(fp);
  return(merged_result);
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
    auto result = stream_progressive_png_image(path, output_path, 2000);
    m.time_ms = now_ms() - start;
    m.ram_mb = get_peak_rss();
    delete result;
    std::ifstream input(output_path);
    nlohmann::json geojson;
    input >> geojson;
    for (const auto& feature : geojson["features"]) {
      m.external++;
      m.holes += static_cast<int>(
        feature["geometry"]["coordinates"].size() - 1
      );
    }
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

EngineMetrics run_gdal_isolated_single_class(const std::string& path, const std::string& name) {
  int fd[2];
  pipe(fd);
  if (fork() == 0) {
    close(fd[0]);
    EngineMetrics m{};

    char** openOpts = nullptr;
    openOpts = CSLSetNameValue(openOpts, "FORCE_COLOR_INTERP", "GRAY");
    auto* src = (GDALDataset*)GDALOpenEx(path.c_str(), GDAL_OF_RASTER, nullptr, openOpts, nullptr);
    CSLDestroy(openOpts);
    auto* band = src->GetRasterBand(1);

    int xsize = band->GetXSize();
    int ysize = band->GetYSize();
    const GByte WHITE = 255;

    auto* memDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    auto* maskDS = memDriver->Create("", xsize, ysize, 1, GDT_Byte, nullptr);
    auto* maskBand = maskDS->GetRasterBand(1);

    std::vector<GByte> rowBuf(xsize);
    std::vector<GByte> maskRow(xsize);
    for (int y = 0; y < ysize; y++) {
      band->RasterIO(GF_Read, 0, y, xsize, 1, rowBuf.data(), xsize, 1, GDT_Byte, 0, 0);
      for (int x = 0; x < xsize; x++) {
        maskRow[x] = (rowBuf[x] != WHITE) ? 1 : 0;
      }
      maskBand->RasterIO(GF_Write, 0, y, xsize, 1, maskRow.data(), xsize, 1, GDT_Byte, 0, 0);
    }

    double start = now_ms();  // timer

    std::string geojson = "./gdal_singleclass_" + name + ".geojson";
    std::remove(geojson.c_str());
    auto* driver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    auto* dst = driver->Create(geojson.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    auto* layer = dst->CreateLayer("gdal_contours", nullptr, wkbPolygon, nullptr);
    OGRFieldDefn field("PixelVal", OFTInteger);
    layer->CreateField(&field);
    int pix = layer->GetLayerDefn()->GetFieldIndex("PixelVal");

    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "8CONNECTED", "8");

    GDALPolygonize(band, maskBand, layer, pix, opts, nullptr, nullptr);

    CSLDestroy(opts);
    GDALClose(maskDS);
    GDALClose(src);
    GDALClose(dst);
    m.time_ms = now_ms() - start;
    m.ram_mb = get_peak_rss();
    CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", "0");
    auto* readDS = (GDALDataset*)GDALOpenEx(geojson.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    auto* readLayer = readDS->GetLayer(0);
    readLayer->ResetReading();
    while (auto* f = readLayer->GetNextFeature()) {
      auto* poly = (OGRPolygon*)f->GetGeometryRef();
      m.external++;
      m.holes += poly->getNumInteriorRings();
      OGRFeature::DestroyFeature(f);
    }
    GDALClose(readDS);

    write(fd[1], &m, sizeof(m));
    close(fd[1]);
    _exit(0);
  }
  close(fd[1]);
  EngineMetrics m{};
  read(fd[0], &m, sizeof(m));
  close(fd[0]);
  wait(nullptr);
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
    if (arg == "--image" && i + 1 < argc) {
      image_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout
        << "Usage: " << argv[0]
        << " [--image FILE_OR_PATH]\n";
      return 0;
    }
  }

  // Registers GDAL drivers once
  GDALAllRegister();

  std::vector<ImageInfo> images = {
    {"test_4096x4096.png", 4096, 4096},
    {"test_10000x10000.png", 10000, 10000},
    {"test_10240x10240.png", 10240, 10240},
    {"test_10240x10240_2.png", 10240, 10240},
    {"test_15360x15360.png", 15360, 15360},
    {"test_20480x20480.png", 20480, 20480},
    {"test_40960x40960.png", 40960, 40960}
  };

  if (!image_path.empty()) {
    std::cout <<  "Reading... " << image_path << std::endl;
    ImageInfo user_info;
    if (get_png_dimensions("../../images/" + image_path,user_info.w,user_info.h)) {
      std::cout << "Given image dimension: " << user_info.w << "x" << user_info.h << " px\n";
      user_info.name = image_path;
      images.clear();
      images.push_back(user_info);
    }
  }

  std::vector<BenchResult> results;

  for (auto &imgInfo : images) {
    std::string path = "../../images/" + imgInfo.name;
    std::cout << "=== Processing " << path << " ===" << std::endl;
    
    // --- CONTREK ---
    auto contrek = run_contrek_isolated(path, imgInfo.name);

    // --- GDAL POLYGONIZE (with GeoJSON Output) ---
    auto gdal    = run_gdal_isolated_single_class(path, imgInfo.name);
    
    // --- STATS ---
    results.push_back({
      imgInfo.name,
      contrek.time_ms,contrek.ram_mb,contrek.external,contrek.holes,
      gdal.time_ms,gdal.ram_mb,gdal.external,gdal.holes,
      imgInfo.w,
      imgInfo.h
    });
  }

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
       << "<th>Contrek Speedup</th><th>Contrek RAM Saving</th></tr>";
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
    html << "<td><span class='ratio-pill " << (speed_ratio < 1.0 ? "good" : "bad") << "'>"
     << std::fixed << std::setprecision(2)
     << (speed_ratio < 1.0 ? 1.0 / speed_ratio : speed_ratio)
     << "&times; faster</span></td>";

    html << "<td><span class='ratio-pill " << (ram_ratio < 1.0 ? "good" : "bad") << "'>"
     << std::fixed << std::setprecision(2)
     << (ram_ratio < 1.0 ? 1.0 / ram_ratio : ram_ratio)
     << "&times; less RAM</span></td>";
    html << "</tr>";
  }

  html << "</tbody></table></body></html>";
  html.close();
  
  std::cout << "\n[OK] Benchmark done! Report data to 'cpp_gdal_benchmark_results.html'" << std::endl;
}
