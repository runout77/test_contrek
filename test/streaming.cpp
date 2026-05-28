#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sys/resource.h>
#include <fstream>
#include <spng.h>
#include "ContrekApi.h"
#include "polygon/finder/concurrent/VerticalMerger.h"


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
    std::vector<std::string> varguments = {};
    VerticalMerger vmerger(0, &varguments);

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

    size_t row_size = static_cast<size_t>(total_width) * 4;
    int stripe_count = 0;
    // main strpes loop
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
        "--strict_bounds"  // <- this option is strictly needed when working with vertical merger
      };
      PolygonFinder polygon_finder(&stripe_bitmap, &not_matcher, nullptr, &finder_arguments);
      ProcessResult *result = polygon_finder.process_info();
      if (result) {
        std::cout << "stripe " << stripe_count << ": found polygons " << result->groups << std::endl;
        ProcessResult* safe_result = result->clone();
        result_clones.push_back(safe_result);
        vmerger.add_tile(*safe_result);
        delete result;
      }
      stripe_count++;
    }

    std::cout << "Merging polygons ..." << std::endl;
    ProcessResult *merged_result = vmerger.process_info();

    if (merged_result) {
      std::cout << "Found total polygons: " << merged_result->groups << std::endl;
      //merged_result->print_info();
      if (generate_png) {
        RawBitmap full_bitmap;
        full_bitmap.define(total_width, total_height, 4, true);
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
    spng_ctx_free(ctx);
    fclose(fp);
}

int main(int argc, char* argv[]) {
  bool generate_svg = false;
  bool generate_png = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--svg") {
      generate_svg = true;
    } else if (arg == "--png") {
      generate_png = true;
    }
  }

  std::cout << "Initial memory usage: " << get_peak_rss() << " MB\n";
  double start_time = now_ms();

  stream_png_image("../images/test_40960x40960.png", 2000, generate_svg, generate_png);
  //stream_png_image("../images/test_1024x1024.png", 300);

  double end_time = now_ms();
  double total_time = end_time - start_time;
  std::cout << "Execution time: " << total_time << " ms " << std::endl;
  std::cout << "Memory usage peak: " << get_peak_rss() << " MB" << std::endl;
  return 0;
}
