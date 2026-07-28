```bash
# Build the image using Docker Compose
sudo docker compose build test

# Run and enter the container shell
sudo docker compose run test
```

```Bash
cd test
./cpp_test.sh
cd build
./gdal_test 
```


```Bash
cd build
make -j
./gdal_test 
```

```Bash
./gdal_test  --help
./gdal_test  --image test_10000x10000.png
./gdal_test  --image test_81920x81920.png
./gdal_test  --image high_complexity_81920x81920.png
```
