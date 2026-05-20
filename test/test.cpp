#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sys/resource.h>
#include <fstream>

#include "ContrekApi.h"
#include <opencv2/opencv.hpp>

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
  double o_time;
  double o_ram;
  int o_external;
  int o_holes;
  int w;
  int h;
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

int main(int argc, char* argv[]) {
  bool use_tree = false;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--tree") use_tree = true;
  }

  std::vector<ImageInfo> images = {
      {"test_1024x1024", 1024, 1024},
      {"test_4096x4096", 4096, 4096},
      {"test_10000x10000", 10000, 10000},
      {"test_10240x10240", 10240, 10240},
      {"test_10240x10240_2", 10240, 10240},
      {"test_15360x15360", 15360, 15360},
      {"test_20480x20480", 20480, 20480}
  };

  std::vector<BenchResult> results;

  Contrek::Config cfg;
  cfg.threads = 8;
  cfg.tiles = 8;
  cfg.compress_unique = true;
  cfg.connectivity_mode = Contrek::Connectivity::OMNIDIRECTIONAL;
  cfg.treemap = use_tree;

  for (auto &imgInfo : images) {
    std::string path = "../images/" + imgInfo.name + ".png";
    std::cout << "=== Processing " << path << " ===" << std::endl;
    // --- CONTREK ---
    double start_c = now_ms();
    auto result = Contrek::trace(path, cfg);
    double end_c = now_ms();
    
    double c_ram = get_peak_rss();
    int c_polys = result->polygons.size();
    int c_inner = 0;
    for (Polygon& x : result->polygons)
    { c_inner += x.inner.size();
    }

    double c_time = end_c - start_c;

    // --- OPENCV ---
    double start_o = now_ms();
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (img.empty()) continue;

    cv::Mat mask;
    cv::Scalar exclude_color = (img.channels() == 4) ? cv::Scalar(255,255,255,255) : cv::Scalar(255,255,255);
    cv::inRange(img, exclude_color, exclude_color, mask);
    cv::bitwise_not(mask, mask);

    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(mask, contours, hierarchy, use_tree ? cv::RETR_TREE : cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);
    
    int external_count = 0;
    int hole_count = 0;

    for (int i = 0; i < contours.size(); i++) {
      // if hierarchy[i][3] (the parent) è -1, external contour
      if (hierarchy[i][3] == -1) {
        external_count++;
      } else {
        hole_count++;
      }
    }
    double end_o = now_ms();
    
    results.push_back({
      imgInfo.name,
      c_time, c_ram,c_polys, c_inner,
      (end_o - start_o), get_peak_rss(), external_count,hole_count,imgInfo.w,imgInfo.h
    });
  }

  std::ofstream html("cpp_benchmark_results.html");

html << "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    << "<style>"
    << "body { font-family: 'Segoe UI', sans-serif; background: #f0f2f5; padding: 30px; }"
    << "table { border-collapse: collapse; width: 100%; background: white; border-radius: 8px; overflow: hidden; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }"
    << "th, td { padding: 10px 6px; text-align: right; border-bottom: 1px solid #eee; font-size: 12px; }"
    << "th { background-color: #232f3e; color: white; text-transform: uppercase; font-size: 10px; letter-spacing: 1px; text-align: center; }"
    << ".img-name { text-align: left; font-weight: bold; color: #1a73e8; font-size: 11px; }"
    << ".contrek-col { background-color: #f1f8ff; }"
    << ".opencv-col { background-color: #fffdf2; }"
    << ".setup-cell { text-align: center; font-family: monospace; color: #666; font-weight: bold; }"
    << ".winner-label { font-weight: bold; text-align: center; font-size: 9px; padding: 2px 4px; border-radius: 4px; }"
    << ".winner-ct { background: #d4edda; color: #155724; box-shadow: 0 0 6px 3px rgba(255, 0, 0, 0.5);}"
    << ".winner-cv { background: #ffeeba; color: #856404; }"
    << ".ratio-pill { padding: 4px 8px; border-radius: 12px; font-size: 11px; font-weight: bold; display: inline-block; min-width: 40px; }"
    << ".good { background: #2ecc71; color: white; }"
    << ".bad { background: #e67e22; color: white; }"
    << "</style></head><body>";

  html << "<h1 style='color: #232f3e;'>🚀 Contrek vs OpenCV: Native Benchmark Report</h1>";
  html << "<p>Mode: <b>" << (use_tree ? "RETR_TREE / treemap=true" : "RETR_CCOMP / treemap=false") << "</b></p>";
  html << "<table><thead>";
  html << "<tr><th rowspan='2'>Target Image</th><th rowspan='2'>MP</th>"
    << "<th colspan='7'>CONTREK ENGINE</th>"
    << "<th colspan='6'>OPENCV ENGINE</th>"
    << "<th colspan='2'>RATIOS</th></tr>";
  html << "<tr><th>Ext.</th><th>Hole</th><th>Setup (T/L)</th><th>Time</th><th>Better?</th><th>RAM</th><th>Better?</th>"
    << "<th>Ext.</th><th>Hole</th><th>Time</th><th>Better?</th><th>RAM</th><th>Better?</th>"
    << "<th>Speed</th><th>RAM</th></tr>";
  html << "</thead><tbody>";

  for (const auto& r : results) {
    double mp = (r.w * r.h) / 1000000.0;
    double speed_ratio = r.c_time / r.o_time;
    double ram_ratio   = r.c_ram / r.o_ram;
    
    bool ct_speed_win = r.c_time < r.o_time;
    bool ct_ram_win   = r.c_ram < r.o_ram;

    html << "<tr>";
    html << "<td class='img-name'>" << r.name << "</td>";
    html << "<td>" << std::fixed << std::setprecision(1) << mp << "</td>";
    
    // --- CONTREK STATS ---
    html << "<td class='contrek-col'>" << r.c_external << "</td>";
    html << "<td class='contrek-col'>" << r.c_holes << "</td>";
    html << "<td class='contrek-col setup-cell'>" << cfg.threads << "/" << cfg.tiles << "</td>";
    html << "<td class='contrek-col'>" << (int)r.c_time << "ms</td>";
    html << "<td class='contrek-col'>" << (ct_speed_win ? "<span class='winner-label winner-ct'>WINNER</span>" : "-") << "</td>";
    html << "<td class='contrek-col'>" << (int)r.c_ram << "MB</td>";
    html << "<td class='contrek-col'>" << (ct_ram_win ? "<span class='winner-label winner-ct'>EFFICIENT</span>" : "-") << "</td>";

    // --- OPENCV STATS ---
    html << "<td class='opencv-col'>" << r.o_external << "</td>";
    html << "<td class='opencv-col'>" << r.o_holes << "</td>";
    html << "<td class='opencv-col'>" << (int)r.o_time << "ms</td>";
    html << "<td class='opencv-col'>" << (!ct_speed_win ? "<span class='winner-label winner-cv'>WINNER</span>" : "-") << "</td>";
    html << "<td class='opencv-col'>" << (int)r.o_ram << "MB</td>";
    html << "<td class='opencv-col'>" << (!ct_ram_win ? "<span class='winner-label winner-cv'>EFFICIENT</span>" : "-") << "</td>";

    // --- RATIOS ---
    html << "<td><span class='ratio-pill " << (speed_ratio < 1.0 ? "good" : "bad") << "'>" 
      << std::fixed << std::setprecision(2) << speed_ratio << "x</span></td>";
    html << "<td><span class='ratio-pill " << (ram_ratio < 1.0 ? "good" : "bad") << "'>" 
      << ram_ratio << "x</span></td>";
    html << "</tr>";
  }

  html << "</tbody></table></body></html>";
  html.close();
}
