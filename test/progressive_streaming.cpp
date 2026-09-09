#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sys/resource.h>
#include <fstream>
#include <spng.h>
#include <filesystem>
#include "ContrekApi.h"
#include "polygon/finder/concurrent/VerticalMerger.h"
#include "polygon/finder/concurrent/StreamingMerger.h"
#include "polygon/finder/concurrent/SvgStreamingMerger.h"
#include "polygon/bitmaps/streaming/PngSource.h"
#include "polygon/bitmaps/streaming/RasterStreamer.h"

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

void stream_progressive_png_image(const std::string& filepath, uint32_t stripe_height) {
  PngSource source(filepath);
  RasterStreamer streamer(source, stripe_height);

  // allocates stripe buffer
  RawBitmap stripe_bitmap(source.width(), stripe_height);
  RGBNotMatcher not_matcher(-1);

  // allocates streaming svg buffer
  std::string output_path = "streaming_buffer.svg";
  std::ofstream shared_stream(output_path, std::ios::out | std::ios::binary);
  if (!shared_stream) {
    std::cerr << "Error: Unable creating output streaming file!" << std::endl;
    return;
  }
  std::vector<char> buffer(4 * 1024 * 1024);  // Buffer (4MB)
  shared_stream.rdbuf()->pubsetbuf(buffer.data(), buffer.size());

  Options varguments = {
    {"bounds", true}
  };

  SvgStreamingMerger vmerger(0, varguments, &shared_stream, source.width(), source.height());

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
        {"compress", Options{
          {"uniq", true},
          {"linear", true},
        }},
        {"connectivity", 8}
      };

      PolygonFinder polygon_finder(&bitmap, &not_matcher, nullptr, finder_options);
      ProcessResult *result = polygon_finder.process_info();

      if (result) {
        std::cout << "stripe " << stripe_count << ": found polygons " << result->groups << std::endl;

        processed_rows += buffer_rows - (first ? 0 : RasterStreamer::OVERLAP);
        vmerger.add_tile(*result, processed_rows == source.height());

        delete result;
      }

      stripe_count++;
      first = false;
    });

    ProcessResult *merged_result = vmerger.process_info();
    std::cout << "total found polygons " << merged_result->groups << std::endl;
    delete merged_result;
  } catch (const std::exception& e) {
    std::cerr << "\n[ERROR] Processing exception: " << e.what() << std::endl;
    if (shared_stream.is_open()) shared_stream.close();
  }
}

int main(int argc, char* argv[]) {
  std::string image_path = "../../images/high_complexity_81920x81920.png";
  int stripe_height = 2000;
  bool generate_svg = false;
  bool generate_png = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--image" && i + 1 < argc) {
      std::string image_name = argv[++i];

      if (image_name.find('/') != std::string::npos ||
          image_name.find('\\') != std::string::npos) {
        image_path = image_name;
      } else {
        image_path = "../images/" + image_name;
      }
    } else if (arg == "--stripe-height" && i + 1 < argc) {
      stripe_height = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (arg == "--help" || arg == "-h") {
      std::cout
        << "Usage: " << argv[0]
        << " [--image FILE_OR_PATH] [--stripe-height N]\n"
        << "\n"
        << "Default image: ../images/high_complexity_81920x81920.png\n"
        << "Large test images must be downloaded first. See README.\n";
      return 0;
    }
  }

  if (!std::filesystem::exists(image_path)) {
    std::cerr
      << "Missing test image: " << image_path << "\n"
      << "Large test images must be downloaded first. See README.\n";
    return 1;
  }

  std::cout << "Image: " << image_path << std::endl;
  std::cout << "Initial memory usage: " << get_peak_rss() << " MB\n";
  std::cout << "Stripe height: " << stripe_height << std::endl;
  double start_time = now_ms();

  // image_path = "../images/test_1024x1024.png";

  stream_progressive_png_image(image_path, stripe_height);

  double end_time = now_ms();
  double total_time = end_time - start_time;
  std::cout << "Execution time: " << total_time << " ms " << std::endl;
  std::cout << "Memory usage peak: " << get_peak_rss() << " MB" << std::endl;
  return 0;
}
