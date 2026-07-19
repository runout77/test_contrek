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
#include "polygon/finder/concurrent/GeoJsonStreamingMerger.h"

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
    std::vector<ProcessResult*> result_clones;
    std::vector<std::string> varguments = {"--bounds"};
    // opens image to stream
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) {
      std::cerr << "Unable open file: " << filepath << std::endl;
      return;
    }

    // exams image
    spng_ctx *ctx = spng_ctx_new(0);
    spng_set_png_file(ctx, fp);
    struct spng_ihdr ihdr;
    if (spng_get_ihdr(ctx, &ihdr)) {
      fclose(fp);
      spng_ctx_free(ctx);
      return;
    }
    uint32_t total_width = ihdr.width;
    uint32_t total_height = ihdr.height;

    // allocates stripe buffer
    RawBitmap stripe_bitmap;
    stripe_bitmap.define(total_width, stripe_height, 4, true);
    RGBNotMatcher not_matcher(-1);
    if (spng_decode_image(ctx, NULL, 0, SPNG_FMT_RGBA8, SPNG_DECODE_PROGRESSIVE)) {
      fclose(fp);
      spng_ctx_free(ctx);
      return;
    }

    // allocates streaming svg buffer
    std::string output_path = "streaming_buffer.svg";
    std::ofstream shared_stream(output_path, std::ios::out | std::ios::binary);
    if (!shared_stream) {
      std::cerr << "Error: Unable creating output streaming file!" << std::endl;
    }
    std::vector<char> buffer(4 * 1024 * 1024);  // Buffer (4MB)
    shared_stream.rdbuf()->pubsetbuf(buffer.data(), buffer.size());

    SvgStreamingMerger vmerger(0, &varguments, &shared_stream, total_width, total_height);
    try {
      size_t row_size = static_cast<size_t>(total_width) * 4;
      int stripe_count = 0;
      // main stripes loop
      for (uint32_t current_y_offset = 0; current_y_offset < total_height; current_y_offset += stripe_height) {
        int uncovered_height = total_height - current_y_offset;

        // copy previous last line to the next new one (each contigue stripe must share one pixel scanline)
        if (current_y_offset > 0) {
          unsigned char* last_row_prev = const_cast<unsigned char*>(stripe_bitmap.get_row_ptr(stripe_height - 1));
          unsigned char* first_row_curr = const_cast<unsigned char*>(stripe_bitmap.get_row_ptr(0));
          std::memcpy(first_row_curr, last_row_prev, row_size);
        }
        // clears the part of the stripe wont be overwritten by png data
        if (uncovered_height < stripe_height)
        { stripe_bitmap.draw_filled_rectangle(0, 1, total_width, stripe_height - 1, 255, 255, 255);
        }
        // decoding data directly in the stripe buffer
        uint32_t lines_to_read = std::min(stripe_height, total_height - current_y_offset);
        for (uint32_t y = (current_y_offset == 0 ? 0 : 1); y < lines_to_read; y++) {
          unsigned char* row_ptr = const_cast<unsigned char*>(stripe_bitmap.get_row_ptr(y));
          int ret = spng_decode_row(ctx, row_ptr, row_size);
          if (ret != 0 && ret != SPNG_EOI) break;
        }
        // stripe contour tracing
        std::vector<std::string> finder_arguments = {
          "--versus=a",
          "--bounds",
          "--connectivity=8",
          "--compress_uniq",
          "--compress_linear"
        };

        PolygonFinder polygon_finder(&stripe_bitmap, &not_matcher, nullptr, &finder_arguments);
        ProcessResult *result = polygon_finder.process_info();
        if (result) {
          std::cout << "stripe " << stripe_count << ": found polygons " << result->groups << std::endl;
          vmerger.add_tile(*result, !(current_y_offset + stripe_height < total_height));
          delete result;
        }
        stripe_count++;
      }
      ProcessResult *merged_result = vmerger.process_info();
      std::cout << "total found polygons " << merged_result->groups << std::endl;
      delete merged_result;
    } catch (const std::exception& e) {
      std::cerr << "\n[ERROR] Processing exception: " << e.what() << std::endl;
      if (shared_stream.is_open()) shared_stream.close();
    }
    spng_ctx_free(ctx);
    fclose(fp);
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
