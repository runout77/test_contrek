#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sys/resource.h>
#include <fstream>
#include <arpa/inet.h>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>

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

EngineMetrics run_contrek_isolated(const std::string& path, const Contrek::Config& cfg) {
  int fd[2];
  pipe(fd);
  pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    EngineMetrics m{};
    double start = now_ms();
    auto result = Contrek::trace(path, cfg);
    m.time_ms = now_ms() - start;
    m.ram_mb = get_peak_rss();
    m.external = static_cast<int>(result->polygons.size());
    m.holes = 0;
    for (auto& x : result->polygons) {
      m.holes += static_cast<int>(x.inner.size());
    }
    write(fd[1], &m, sizeof(m));
    close(fd[1]);
    _exit(0);
  }
  close(fd[1]);
  EngineMetrics m{};
  ssize_t n = read(fd[0], &m, sizeof(m));
  close(fd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (n != sizeof(m)) {
    std::cerr << "[WARN] Contrek child produced no data (crashed/OOM-killed?)\n";
  }
  return m;
}

EngineMetrics run_opencv_isolated(const std::string& path, bool use_tree) {
  int fd[2];
  pipe(fd);
  pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    EngineMetrics m{};
    double start = now_ms();
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (img.empty()) {
      write(fd[1], &m, sizeof(m));
      close(fd[1]);
      _exit(1);
    }
    cv::Mat mask;
    cv::Scalar exclude_color = (img.channels() == 4)
      ? cv::Scalar(255, 255, 255, 255)
      : cv::Scalar(255, 255, 255);
    cv::inRange(img, exclude_color, exclude_color, mask);
    cv::bitwise_not(mask, mask);
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(mask, contours, hierarchy,
                      use_tree ? cv::RETR_TREE : cv::RETR_CCOMP,
                      cv::CHAIN_APPROX_SIMPLE);
    int external_count = 0;
    int hole_count = 0;
    for (size_t i = 0; i < contours.size(); i++) {
      if (hierarchy[i][3] == -1) {
        external_count++;
      } else {
        hole_count++;
      }
    }
    m.time_ms = now_ms() - start;
    m.ram_mb = get_peak_rss();
    m.external = external_count;
    m.holes = hole_count;
    write(fd[1], &m, sizeof(m));
    close(fd[1]);
    _exit(0);
  }
  close(fd[1]);
  EngineMetrics m{};
  ssize_t n = read(fd[0], &m, sizeof(m));
  close(fd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (n != sizeof(m)) {
    std::cerr << "[WARN] OpenCV child produced no data (crashed/OOM-killed?)\n";
  }
  return m;
}

int main(int argc, char* argv[]) {
  bool use_tree = false;
  std::string image_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--image" && i + 1 < argc) {
      image_path = argv[++i];
    } else if (arg == "--tree") {
      use_tree = true;
    } else if (arg == "--info") {
      std::cout << cv::getBuildInformation();
      std::cout << "OpenCV version=" << CV_VERSION << std::endl;
      return 0;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [--image FILE_OR_PATH] [--tree] [--info]\n";
      return 0;
    }
  }

  std::vector<ImageInfo> images = {
      {"test_1024x1024.png", 1024, 1024},
      {"test_4096x4096.png", 4096, 4096},
      {"test_10000x10000.png", 10000, 10000},
      {"test_10240x10240.png", 10240, 10240},
      {"test_10240x10240_2.png", 10240, 10240},
      {"test_15360x15360.png", 15360, 15360},
      {"test_20480x20480.png", 20480, 20480}
  };

  if (!image_path.empty()) {
    std::cout << "Reading... " << image_path << std::endl;
    ImageInfo user_info;
    if (get_png_dimensions("../../images/" + image_path, user_info.w, user_info.h)) {
      std::cout << "Given image dimension: " << user_info.w << "x" << user_info.h << " px\n";
      user_info.name = image_path;
      images.clear();
      images.push_back(user_info);
    }
  }

  std::vector<BenchResult> results;

  Contrek::Config cfg;
  cfg.threads = 8;
  cfg.tiles = 8;
  cfg.compress_unique = true;
  cfg.connectivity_mode = Contrek::Connectivity::OMNIDIRECTIONAL;
  cfg.treemap = use_tree;

  for (auto& imgInfo : images) {
    std::string path = "../../images/" + imgInfo.name;
    std::cout << "=== Processing " << path << " ===" << std::endl;

    // --- CONTREK  ---
    auto contrek = run_contrek_isolated(path, cfg);

    // --- OPENCV ---
    auto opencv = run_opencv_isolated(path, use_tree);

    // --- VERIFY ---
    if (contrek.external != opencv.external || contrek.holes != opencv.holes) {
      std::cerr << "[WARN] Mismatch on " << imgInfo.name
                << " -> contrek(ext=" << contrek.external << ", hole=" << contrek.holes << ")"
                << " vs opencv(ext=" << opencv.external << ", hole=" << opencv.holes << ")\n";
    }

    results.push_back({
      imgInfo.name,
      contrek.time_ms, contrek.ram_mb, contrek.external, contrek.holes,
      opencv.time_ms, opencv.ram_mb, opencv.external, opencv.holes,
      imgInfo.w, imgInfo.h
    });
  }

  // --- REPORT HTML ---
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
       << ".winner-cv { background: #ffeeba; color: #856404; box-shadow: 0 0 6px 3px rgba(255, 0, 0, 0.5);}"
       << ".check { display:inline-block; color:#2ecc71; font-weight:bold; font-size:18px; text-align:center; margin-left:4px; }"
       << ".ratio-pill { padding: 4px 8px; border-radius: 12px; font-size: 11px; font-weight: bold; display: inline-block; min-width: 40px; }"
       << ".good { background: #2ecc71; color: white; }"
       << ".bad { background: #e67e22; color: white; }"
       << "</style></head><body>";

  html << "<h1 style='color: #232f3e;'>🚀 Contrek vs OpenCV: Native Benchmark Report</h1>";
  html << "<p>Mode: <b>" << (use_tree ? "RETR_TREE / treemap=true" : "RETR_CCOMP / treemap=false") << "</b></p>";
  html << "<p><i>Each engine runs in an isolated child process (fork), so peak-RSS measurements are not contaminated across engines or across images.</i></p>";
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
    double mp = (static_cast<double>(r.w) * r.h) / 1000000.0;
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
    html << "<td class='contrek-col'>" << (ct_speed_win ? "<span class='winner-label winner-ct check'>\u2713</span>" : "-") << "</td>";
    html << "<td class='contrek-col'>" << (int)r.c_ram << "MB</td>";
    html << "<td class='contrek-col'>" << (ct_ram_win ? "<span class='winner-label winner-ct check'>\u2713</span>" : "-") << "</td>";

    // --- OPENCV STATS ---
    html << "<td class='opencv-col'>" << r.o_external << "</td>";
    html << "<td class='opencv-col'>" << r.o_holes << "</td>";
    html << "<td class='opencv-col'>" << (int)r.o_time << "ms</td>";
    html << "<td class='opencv-col'>" << (!ct_speed_win ? "<span class='winner-label winner-cv check'>\u2713</span>" : "-") << "</td>";
    html << "<td class='opencv-col'>" << (int)r.o_ram << "MB</td>";
    html << "<td class='opencv-col'>" << (!ct_ram_win ? "<span class='winner-label winner-cv check'>\u2713</span>" : "-") << "</td>";

    // --- RATIOS ---
    html << "<td><span class='ratio-pill " << (speed_ratio < 1.0 ? "good" : "bad") << "'>"
         << std::fixed << std::setprecision(2) << speed_ratio << "x</span></td>";
    html << "<td><span class='ratio-pill " << (ram_ratio < 1.0 ? "good" : "bad") << "'>"
         << std::fixed << std::setprecision(2) << ram_ratio << "x</span></td>";
    html << "</tr>";
  }

  html << "</tbody></table></body></html>";
  html.close();

  std::cout << "\n[OK] Benchmark done! Report -> cpp_benchmark_results.html" << std::endl;
  return 0;
}