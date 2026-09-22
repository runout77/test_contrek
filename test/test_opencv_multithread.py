"""
Experimental OpenCV + Contrek multithreaded pipeline.

This example is functional but not yet optimized. OpenCV contours currently
cross the Python/C++ boundary individually, adding significant overhead.
A future batch conversion API should move this step entirely to C++.
"""

import sys
import time
import resource
from collections import deque
from concurrent.futures import ThreadPoolExecutor

import cv2
import numpy as np
import contrek


image_path = sys.argv[1] if len(sys.argv) > 1 else "../images/test_1024x1024.png"
stripe_height = int(sys.argv[2]) if len(sys.argv) > 2 else 300
number_of_threads = int(sys.argv[3]) if len(sys.argv) > 3 else 4

print("image:", image_path)
print("stripe height:", stripe_height)
print("threads:", number_of_threads)

source = contrek.PngSource(image_path)
streamer = contrek.RasterStreamer(source, stripe_height)
bitmap = contrek.RawBitmap(source.width, stripe_height)
merger = contrek.VerticalMerger()

pending = deque()
stripe_index = 0


def process_stripe(detached, buffer_rows, index):
    rgba = np.asarray(detached)[:buffer_rows]

    gray = cv2.cvtColor(rgba, cv2.COLOR_RGBA2GRAY)
    _, binary = cv2.threshold(gray, 127, 255, cv2.THRESH_BINARY_INV)

    contours, hierarchy = cv2.findContours(
        binary,
        cv2.RETR_CCOMP,
        cv2.CHAIN_APPROX_NONE
    )

    polygons = []

    if hierarchy is not None:
        hierarchy = hierarchy[0]

        for i, contour in enumerate(contours):
            if hierarchy[i][3] != -1:
                continue

            x, y, w, h = cv2.boundingRect(contour)

            bounds = {
                "min_x": x,
                "max_x": x + w,
                "min_y": y,
                "max_y": y + h,
            }

            outer = contrek.opencv_contour_to_cell_boundary(
                contour[:, 0, :],
                bounds
            )

            inner = []
            child = hierarchy[i][2]

            while child != -1:
                child_contour = contours[child]
                cx, cy, cw, ch = cv2.boundingRect(child_contour)

                child_bounds = {
                    "min_x": cx,
                    "max_x": cx + cw,
                    "min_y": cy,
                    "max_y": cy + ch,
                }

                hole = contrek.opencv_contour_to_cell_boundary(
                    child_contour[:, 0, :],
                    child_bounds
                )

                inner.append(hole)
                child = hierarchy[child][0]

            polygons.append({
                "outer": outer,
                "inner": inner,
                "bounds": bounds,
            })

    polygons.sort(key=lambda p: (
        p["bounds"]["min_y"],
        p["bounds"]["min_x"]
    ))

    result = contrek.make_result_from_polygons(
        polygons,
        rgba.shape[1],
        buffer_rows,
        contrek.Versus.ANTICLOCKWISE
    )

    print(
        "stripe:", index,
        "rows:", buffer_rows,
        "contours:", len(contours),
        "polygons:", len(polygons)
    )

    return result


def merge_oldest():
    future = pending.popleft()
    result = future.result()
    merger.add_tile(result)


def submit_stripe(bitmap, buffer_rows, buffer_size, rows_read):
    global stripe_index

    detached = bitmap.detach()

    future = executor.submit(
        process_stripe,
        detached,
        buffer_rows,
        stripe_index
    )

    pending.append(future)
    stripe_index += 1

    if len(pending) >= number_of_threads:
        merge_oldest()


start = time.perf_counter()

with ThreadPoolExecutor(max_workers=number_of_threads) as executor:
    streamer.each(bitmap, submit_stripe)

    while pending:
        merge_oldest()

merged = merger.process_info()

elapsed = time.perf_counter() - start
peak_memory_mb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0

print()
print("merged!")
print("total polygons:", merged["groups"])
print(f"execution time: {elapsed:.3f} s")
print(f"peak memory: {peak_memory_mb:.2f} MB")