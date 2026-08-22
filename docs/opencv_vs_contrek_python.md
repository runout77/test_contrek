# OpenCV Python vs Contrek Python Comparison

## Setup and Execution

### Build and Launch via Docker
The entire environment is fully containerized to ensure cross-platform compatibility and reproducible results. Build the system and launch the interactive testing shell using:

```bash
# Build the image using Docker Compose
sudo docker compose build test

# Build again ignoring cache
sudo docker compose build --no-cache test

# Run and enter the container shell
sudo docker compose run test
```

### Internal Configuration
Once inside the container shell, run the setup script to install Python dependencies:

```bash
./build_python_env.sh
```

### Executing High-Level Tests
Navigate to the test directory and run the benchmarks:

```Bash
cd test
python3 test_contrek.py
python3 test_opencv.py
```
Results will be aggregated into a **test/report.html** file.

See other options by
```Bash
python3 test_contrek.py --help
python3 test_opencv.py --help
```

Both scripts support the --treemap option which uses cv2.RETR_TREE in place of cv2.RETR_CCOMP and pass to the Contrek's core the `treemap: true` flag).

### Visual Validation:
To verify the precision of the results graphically, add the --draw flag:

```Bash
ruby test_contrek.rb --draw
python3 test_opencv.py --draw
```
The resulting images will be saved in the **test/output** directory.

### Very large dataset
You can try massive images using the --image option (image are read from /images root directory)

```bash
OPENCV_IO_MAX_IMAGE_PIXELS=2147483647 python3 test_opencv.py --image test_40960x40960.png
python3 test_contrek.py --image test_40960x40960.png
```

## Benchmark Results

👉 [Benchmarks table of results](https://runout77.github.io/test_contrek/benchmarks/opencv_python_vs_contrek_python.html)