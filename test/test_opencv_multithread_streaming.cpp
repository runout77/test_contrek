#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <sys/resource.h>
#include <filesystem>
#include <future>
#include <deque>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <opencv2/opencv.hpp>

#include "polygon/finder/concurrent/SvgStreamingMerger.h"
#include "polygon/bitmaps/streaming/PngSource.h"
#include "polygon/bitmaps/streaming/RasterStreamer.h"
#include "polygon/shared/OpencvConverter.h"

struct EngineMetrics {
  double time_ms;
  double ram_mb;
};

struct StripeJob {
  int stripe;
  uint32_t rows_read;
  std::future<ProcessResult*> future;
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
                                         int width, int height, const Options& options) {
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
                                            int stripe,
                                            const Options& options) {
  cv::Mat rgba(static_cast<int>(buffer_rows), bitmap->w(), CV_8UC4,
               const_cast<unsigned char*>(bitmap->get_row_ptr(0)),
               static_cast<size_t>(bitmap->w()) * bitmap->get_bytes_per_pixel());

  cv::Mat gray;
  cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);

  cv::Mat binary;
  cv::threshold(gray, binary, 127, 255, cv::THRESH_BINARY_INV);

  std::vector<std::vector<cv::Point>> contours;
  std::vector<cv::Vec4i> hierarchy;

  cv::findContours(binary, contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_NONE);

  return make_opencv_result(contours, hierarchy, bitmap->w(), buffer_rows, options);
}

void stream_png_image(const std::string& filepath, uint32_t stripe_height, int max_threads) {
  PngSource source(filepath);
  RasterStreamer streamer(source, stripe_height);
  RawBitmap stripe_bitmap(source.width(), stripe_height);

  std::string output_path = "opencv_streaming.svg";
  std::ofstream shared_stream(output_path, std::ios::out | std::ios::binary);
  if (!shared_stream) throw std::runtime_error("Unable to create output streaming file");

  std::vector<char> output_buffer(4 * 1024 * 1024);
  shared_stream.rdbuf()->pubsetbuf(output_buffer.data(), output_buffer.size());

  Options options = {
    {"bounds", true}
  };

  SvgStreamingMerger vmerger(0, options, &shared_stream, source.width(), source.height());

  std::deque<StripeJob> jobs;
  int stripe_count = 0;
  uint32_t processed_rows = 0;

  auto collect_first = [&]() {
    StripeJob& job = jobs.front();
    ProcessResult* result = job.future.get();

    if (result) {
      processed_rows += job.rows_read;
      const bool last = processed_rows == source.height();

      std::cout << "merge stripe " << job.stripe
                << " polygons=" << result->groups
                << " processed_rows=" << processed_rows
                << (last ? " [last]" : "") << std::endl;

      vmerger.add_tile(*result, last);
      delete result;
    }

    jobs.pop_front();
  };

  streamer.each(stripe_bitmap, [&](Bitmap& bitmap, uint32_t buffer_rows, std::size_t, std::size_t rows_read) {
    std::cout << "stripe " << stripe_count
              << " width=" << bitmap.w()
              << " rows=" << buffer_rows
              << " bpp=" << bitmap.get_bytes_per_pixel()
              << std::endl;

    RawBitmap& raw = static_cast<RawBitmap&>(bitmap);
    auto current_bitmap = raw.detach();
    const int current_stripe = stripe_count++;

    jobs.push_back({
      current_stripe,
      static_cast<uint32_t>(rows_read),
      std::async(std::launch::async,
        [bitmap = std::move(current_bitmap), buffer_rows, current_stripe, options]() mutable {
          return process_opencv_stripe(std::move(bitmap), buffer_rows, current_stripe, options);
        })
    });

    if (static_cast<int>(jobs.size()) >= max_threads) collect_first();
  });

  while (!jobs.empty()) collect_first();

  ProcessResult* merged_result = vmerger.process_info();

  if (merged_result) {
    std::cout << "total found polygons " << merged_result->groups << std::endl;
    delete merged_result;
  }

  shared_stream.close();
  std::cout << "SVG saved to " << output_path << std::endl;
}

EngineMetrics run_isolated(const std::string& image_path, uint32_t stripe_height, int max_threads) {
  int fd[2];
  if (pipe(fd) == -1) throw std::runtime_error("pipe() failed");

  pid_t pid = fork();

  if (pid == -1) {
    close(fd[0]);
    close(fd[1]);
    throw std::runtime_error("fork() failed");
  }

  if (pid == 0) {
    close(fd[0]);
    EngineMetrics metrics{};

    try {
      double start = now_ms();
      stream_png_image(image_path, stripe_height, max_threads);
      metrics.time_ms = now_ms() - start;
      metrics.ram_mb = get_peak_rss();

      write(fd[1], &metrics, sizeof(metrics));
      close(fd[1]);
      _exit(0);
    } catch (const std::exception& e) {
      std::cerr << "[ERROR] " << e.what() << std::endl;
      close(fd[1]);
      _exit(1);
    }
  }

  close(fd[1]);

  EngineMetrics metrics{};
  ssize_t bytes = read(fd[0], &metrics, sizeof(metrics));
  close(fd[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (bytes != sizeof(metrics) || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("Benchmark process failed");

  return metrics;
}

int main(int argc, char* argv[]) {
  std::string image_path = "../../images/test_1024x1024.png";
  uint32_t stripe_height = 1000;
  int max_threads = 8;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--image" && i + 1 < argc) {
      std::string image_name = argv[++i];

      if (image_name.find('/') != std::string::npos || image_name.find('\\') != std::string::npos)
        image_path = image_name;
      else
        image_path = "../../images/" + image_name;

    } else if (arg == "--stripe-height" && i + 1 < argc) {
      stripe_height = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (arg == "--threads" && i + 1 < argc) {
      max_threads = std::stoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0]
                << " [--image FILE_OR_PATH] [--stripe-height N] [--threads N]\n";
      return 0;
    }
  }

  if (!std::filesystem::exists(image_path)) {
    std::cerr << "Missing test image: " << image_path << std::endl;
    return 1;
  }

  if (stripe_height == 0) {
    std::cerr << "Stripe height must be greater than 0" << std::endl;
    return 1;
  }

  if (max_threads < 1) {
    std::cerr << "Threads must be greater than 0" << std::endl;
    return 1;
  }

  std::cout << "Image: " << image_path << std::endl;
  std::cout << "Threads: " << max_threads << std::endl;
  std::cout << "Stripe height: " << stripe_height << std::endl;

  try {
    EngineMetrics metrics = run_isolated(image_path, stripe_height, max_threads);

    std::cout << "Execution time: " << metrics.time_ms << " ms" << std::endl;
    std::cout << "Peak memory: " << metrics.ram_mb << " MB" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "[ERROR] " << e.what() << std::endl;
    return 1;
  }

  return 0;
}