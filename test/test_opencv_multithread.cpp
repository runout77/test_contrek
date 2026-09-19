#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <sys/resource.h>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <future>
#include <deque>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <opencv2/opencv.hpp>
#include "polygon/finder/concurrent/VerticalMerger.h"
#include "polygon/bitmaps/streaming/PngSource.h"
#include "polygon/bitmaps/streaming/RasterStreamer.h"
#include "polygon/shared/OpencvConverter.h"


double get_peak_rss() {
  struct rusage r_usage;
  getrusage(RUSAGE_SELF, &r_usage);
#ifdef __APPLE__
  return r_usage.ru_maxrss / (1024.0 * 1024.0);
#else
  return r_usage.ru_maxrss / 1024.0;
#endif
}

struct StripeJob {
  std::future<ProcessResult*> future;
};

struct EngineMetrics {
  double time_ms;
  double ram_mb;
};

double now_ms() {
  return std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now().time_since_epoch()
  ).count();
}

static RectBounds opencvrect_to_contrekbounds(const cv::Rect& rect) {
  RectBounds bounds;
  bounds.min_x = rect.x;
  bounds.max_x = rect.x + rect.width;
  bounds.min_y = rect.y;
  bounds.max_y = rect.y + rect.height;
  return bounds;
}


static ProcessResult* make_opencv_result(const std::vector<std::vector<cv::Point>>& contours,
                                         const std::vector<cv::Vec4i>& hierarchy,
                                         int width, int height,
                                         const Options& options) {

  auto* result = new ProcessResult();

  result->width = width;
  result->height = height;
  result->versus = Node::A;
  result->options = options;

  for (std::size_t i = 0; i < contours.size(); ++i) {
    if (hierarchy[i][3] != -1) continue;

    cv::Rect rect = cv::boundingRect(contours[i]);
    Polygon poly;

    RectBounds bounds = opencvrect_to_contrekbounds(rect);
    poly.outer = OpencvConverter::contour_to_cell_boundary(contours[i], bounds);
    poly.bounds = bounds;

    for (int child = hierarchy[i][2]; child != -1; child = hierarchy[child][0]) {
      cv::Rect child_rect = cv::boundingRect(contours[child]);
      RectBounds child_bounds = opencvrect_to_contrekbounds(child_rect);
      auto hole = OpencvConverter::contour_to_cell_boundary(contours[child], child_bounds);
      poly.inner.push_back(std::move(hole));
    }

    result->polygons.push_back(std::move(poly));
  }

  result->sort_polygons();
  result->groups = static_cast<int>(result->polygons.size());
  return result;
}


static ProcessResult* process_opencv_stripe(std::unique_ptr<RawBitmap> bitmap,
                                            uint32_t buffer_rows,
                                            const Options& options) {
  cv::Mat rgba(
    static_cast<int>(buffer_rows), bitmap->w(), CV_8UC4,
    const_cast<unsigned char*>(bitmap->get_row_ptr(0)),
    static_cast<size_t>(bitmap->w()) * bitmap->get_bytes_per_pixel()
  );

  cv::Mat gray;
  cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);

  cv::Mat binary;
  cv::threshold(gray, binary, 127, 255, cv::THRESH_BINARY_INV);

  std::vector<std::vector<cv::Point>> contours;
  std::vector<cv::Vec4i> hierarchy;

  cv::findContours(
    binary,
    contours,
    hierarchy,
    cv::RETR_CCOMP,
    cv::CHAIN_APPROX_NONE
  );

  return make_opencv_result(
    contours,
    hierarchy,
    bitmap->w(),
    buffer_rows,
    options
  );
}

void stream_png_image(const std::string& filepath, uint32_t stripe_height, int max_threads, bool save_svg=false) {
  std::vector<ProcessResult*> result_clones;

  PngSource source(filepath);
  RasterStreamer streamer(source, stripe_height);

  Options options = {
    //{"unsafe_mode", true}
  };

  VerticalMerger vmerger(0, options);
  RawBitmap stripe_bitmap(source.width(), stripe_height);

  std::deque<StripeJob> jobs;
  int stripe_count = 0;

  auto collect_first = [&]() {
    StripeJob& job = jobs.front();

    ProcessResult* result = job.future.get();

    if (result) {
      result_clones.push_back(result);
      vmerger.add_tile(*result);
    }

    jobs.pop_front();
  };

  streamer.each(stripe_bitmap, [&](Bitmap& bitmap, uint32_t buffer_rows, std::size_t, std::size_t) {
    std::cout << "stripe " << stripe_count
              << " width=" << bitmap.w()
              << " rows=" << buffer_rows
              << " bpp=" << bitmap.get_bytes_per_pixel()
              << std::endl;

    RawBitmap& raw = static_cast<RawBitmap&>(bitmap);
    auto current_bitmap = raw.detach();

    const int current_stripe = stripe_count++;

    jobs.push_back({
      std::async(std::launch::async,
        [bitmap = std::move(current_bitmap), buffer_rows, current_stripe, options]() mutable {
          return process_opencv_stripe(std::move(bitmap), buffer_rows, options);
        })
    });

    if (static_cast<int>(jobs.size()) >= max_threads)
      collect_first();
  });

  while (!jobs.empty())
    collect_first();

  ProcessResult* merged_result = vmerger.process_info();

  if (merged_result && save_svg) {
    merged_result->save_svg("whole.svg");
    std::cout << "Svg saved!" << std::endl;
  }

  delete merged_result;

  for (auto result : result_clones)
    delete result;
}

EngineMetrics run_isolated(const std::string& image_path,
                           uint32_t stripe_height,
                           int max_threads,
                           bool save_svg) {
  int fd[2];
  if (pipe(fd) == -1)
    throw std::runtime_error("pipe() failed");

  pid_t pid = fork();

  if (pid == -1) {
    close(fd[0]);
    close(fd[1]);
    throw std::runtime_error("fork() failed");
  }

  if (pid == 0) {
    close(fd[0]);

    EngineMetrics m{};

    double start = now_ms();
    stream_png_image(image_path, stripe_height, max_threads, save_svg);
    m.time_ms = now_ms() - start;
    m.ram_mb = get_peak_rss();

    write(fd[1], &m, sizeof(m));
    close(fd[1]);
    _exit(0);
  }

  close(fd[1]);

  EngineMetrics m{};
  ssize_t bytes = read(fd[0], &m, sizeof(m));
  close(fd[0]);

  int status;
  waitpid(pid, &status, 0);

  if (bytes != sizeof(m) || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("Benchmark process failed");

  return m;
}

int main(int argc, char* argv[]) {
  int max_threads = 1;
  bool save_svg = false;
  std::string image_path = "../../images/test_1024x1024.png";

  uint32_t stripe_height = 300; 

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--image" && i + 1 < argc) {
      std::string image_name = argv[++i];
      if (image_name.find('/') != std::string::npos ||
          image_name.find('\\') != std::string::npos) {
        image_path = image_name;
      } else {
        image_path = "../../images/" + image_name;
      }
    } else if (arg == "--stripe-height" && i + 1 < argc) {
      stripe_height = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (arg == "--threads" && i + 1 < argc) {
      max_threads = std::stoi(argv[++i]);
    } else if (arg == "--svg") {
      save_svg = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout
        << "Usage: " << argv[0]
        << " [--image FILE_OR_PATH] [--stripe-height N] [--threads N] [--svg] [--help]\n";
      return 0;
    }
  }

  if (!std::filesystem::exists(image_path)) {
    std::cerr << "Missing test image: " << image_path << std::endl;
    return 1;
  }

  std::cout << "Image: " << image_path << std::endl;
  std::cout << "Threads: " << max_threads << std::endl;
  std::cout << "Stripe height: " << stripe_height << std::endl;

  EngineMetrics metrics = run_isolated(
    image_path,
    stripe_height,
    max_threads,
    save_svg
  );

  std::cout << "Execution time: " << metrics.time_ms << " ms" << std::endl;
  std::cout << "Peak memory: " << metrics.ram_mb << " MB" << std::endl;

  return 0;
}
