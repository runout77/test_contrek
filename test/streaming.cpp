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

void stream_png_image(const std::string& filepath, uint32_t stripe_height, bool generate_svg, bool generate_png) {
    std::vector<ProcessResult*> result_clones;
    Options options = {};
    VerticalMerger vmerger(0, options);

    // opens image to stream
    PngSource source(filepath);
    RasterStreamer streamer(source, stripe_height);

    // allocates stripe buffer
    RawBitmap stripe_bitmap(source.width(), stripe_height);
    RGBNotMatcher not_matcher(-1);

    int stripe_count = 0;

    // main stripes loop
    streamer.each(stripe_bitmap, [&](Bitmap& bitmap, uint32_t buffer_rows, std::size_t, std::size_t) {
      // stripe contour tracing
      Options finder_options = {
        {"processing_height", static_cast<int64_t>(buffer_rows)},
        {"versus", Identifier{"a"}},
      };
      PolygonFinder polygon_finder(&bitmap, &not_matcher, nullptr, finder_options);
      ProcessResult *result = polygon_finder.process_info();
      if (result) {
        std::cout << "stripe " << stripe_count << ": found polygons " << result->groups << std::endl;
        result_clones.push_back(result);
        vmerger.add_tile(*result);
      }
      stripe_count++;
    });

    std::cout << "Merging polygons ..." << std::endl;
    ProcessResult *merged_result = vmerger.process_info();

    if (merged_result) {
      std::cout << "Found total polygons: " << merged_result->groups << std::endl;
      //merged_result->print_info();
      if (generate_png) {
        RawBitmap full_bitmap(source.width(), source.height());
        full_bitmap.fill(255, 255, 255);
        merged_result->draw_on_bitmap(full_bitmap);
        std::cout << "Saving whole png ..." << std::endl;
        if (full_bitmap.save_to_png("whole.png")) {
          std::cout << "Png saved!" << std::endl;
        }
      }
      if (generate_svg) {
        merged_result->save_svg("whole.svg");
        std::cout << "Svg saved!" << std::endl;
      }
    }
    delete merged_result;

    // frees memory
    for (auto c : result_clones) {
      delete c;
    }
}

int main(int argc, char* argv[]) {
  std::string image_path = "../../images/test_40960x40960.png";
  int stripe_height = 2000;
  bool generate_svg = false;
  bool generate_png = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--svg") {
      generate_svg = true;
    } else if (arg == "--png") {
      generate_png = true;
    } else if (arg == "--image" && i + 1 < argc) {
      std::string image_name = argv[++i];

      if (image_name.find('/') != std::string::npos ||
          image_name.find('\\') != std::string::npos) {
        image_path = image_name;
      } else {
        image_path = "../../images/" + image_name;
      }
    } else if (arg == "--stripe-height" && i + 1 < argc) {
      stripe_height = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (arg == "--help" || arg == "-h") {
      std::cout
        << "Usage: " << argv[0]
        << " [--image FILE_OR_PATH] [--stripe-height N] [--svg] [--png]\n"
        << "\n"
        << "Default image: ../../images/test_40960x40960.png\n"
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

  stream_png_image(image_path, stripe_height, generate_svg, generate_png);

  double end_time = now_ms();
  double total_time = end_time - start_time;
  std::cout << "Execution time: " << total_time << " ms " << std::endl;
  std::cout << "Memory usage peak: " << get_peak_rss() << " MB" << std::endl;
  return 0;
}
