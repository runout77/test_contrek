#!/usr/bin/env bash
set -e

# OpenCV Python vs Contrek Ruby Comparison
../build_ruby_env.sh
gem update contrek
ruby test_contrek.rb
python3 test_opencv.py

# OpenCV Python vs Contrek Python Comparison
../build_python_env.sh
python3 test_contrek.py
# python3 test_opencv.py

# OpenCV CPP vs Contrek CPP Comparison
./cpp_test.sh
(
  cd build
  ./contrek_opencv_benchmark
)

# GDAL Polygonize vs Contrek benchmark
(
  cd build
  ./gdal_test --image test_10000x10000.png
)

# Contrek Python — Concurrent Progressive Streaming Benchmark
python3 progressive_streaming.py --image ../images/test_20480x20480.png

# OpenCV Made Multithread with Contrek
(
  cd build
  ./test_opencv_multithread
)

# Simple Streaming Technique
(
  cd build
  ./streaming_benchmark --image '../../images/test_20480x20480.png'
)

# Progressive Streaming SVG Benchmark
(
  cd build
  ./progressive_streaming_benchmark --image '../../images/test_20480x20480.png'
)

echo
echo "DONE!"