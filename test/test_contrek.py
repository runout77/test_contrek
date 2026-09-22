import argparse
import datetime
import json
import os
import sys
import time
from bs4 import BeautifulSoup
import cv2
import numpy as np
from PIL import Image
from pathlib import Path
import contrek

Image.MAX_IMAGE_PIXELS = None

# input arguments
parser = argparse.ArgumentParser(description="Test and benchmark Contrek Python module")
parser.add_argument("-d", "--draw", action="store_true", help="Draw found polygons into PNG images")
parser.add_argument("-t", "--treemap", action="store_true", help="Build hierarchy map of found polygons")
parser.add_argument("--tiles", type=int, default=os.cpu_count() or 8, help="Number of tiles to divide image")
parser.add_argument("--threads", type=int, default=os.cpu_count() or 8, help="Number of threads involved")
parser.add_argument("-j", "--json", action="store_true", help="Stores result as JSON structure")
parser.add_argument("-i", "--image", help="Use different image")

args = parser.parse_args()

print(f"Options = {vars(args)}")

images = [
    {'image': 'test_1024x1024.png', 'w': 1024, 'h': 1024},
    {'image': 'test_4096x4096.png', 'w': 4096, 'h': 4096},
    {'image': 'test_10000x10000.png', 'w': 10000, 'h': 10000},
    {'image': 'test_10240x10240.png', 'w': 10240, 'h': 10240},
    {'image': 'test_10240x10240_2.png', 'w': 10240, 'h': 10240},
    {'image': 'test_15360x15360.png', 'w': 15360, 'h': 15360},
    {'image': 'test_20480x20480.png', 'w': 20480, 'h': 20480},
]

if args.image:
  with Image.open(f"../images/{args.image}") as img:
    w, h = img.size
    images = []
    images.append({'image': args.image, 'w': w, 'h': h})

exclude_color = {"r": 255, "g": 255, "b": 255, "a": 255}

# contrek-python image processing
for image in images:
    image_path = f"../images/{image['image']}"
    print(f"Processing {image_path} ....")

    start_time = time.time()

    finder_opts = {
        "connectivity": 8,
        "number_of_tiles": args.tiles,
        "compress": {"uniq": True},
    }
    if args.treemap:
        finder_opts["treemap"] = True

    bitmap = contrek.FastPngBitmap(image_path)
    target = contrek.rgb_to_target_color(
        exclude_color["r"], exclude_color["g"], exclude_color["b"], exclude_color["a"]
    )

    result = contrek.find_polygons(
        bitmap,
        options=finder_opts,
        target_color=target,
        mode=contrek.MatchMode.NOT_COLOR,
        number_of_threads=args.threads
    )

    end_time = time.time()
    scan_ms = end_time - start_time

    polygons = result.polygons if hasattr(result, "polygons") else result.get("polygons", [])
    metadata = result.metadata if hasattr(result, "metadata") else result.get("metadata", {})

    image['outer'] = len(polygons)
    image['inner'] = sum(len(poly.get('inner', [])) for poly in polygons)
    image['time'] = scan_ms

    if "benchmarks" in metadata:
        print(f"  Benchmarks: {metadata['benchmarks']}")

    # image result draw (if -d / --draw)
    if args.draw:
        os.makedirs("output", exist_ok=True)
        canvas = np.full((image['h'], image['w'], 3), 255, dtype=np.uint8)

        for poly in polygons:
            # outer points (red)
            outer_flat = poly.get('outer', [])
            if len(outer_flat) > 0:
                pts_outer = np.array(outer_flat, dtype=np.int32).reshape((-1, 1, 2))
                cv2.polylines(canvas, [pts_outer], isClosed=True, color=(0, 0, 255), thickness=1)

            # inner points (green)
            for inner_flat in poly.get('inner', []):
                if len(inner_flat) >= 4:
                    pts_inner = np.array(inner_flat, dtype=np.int32).reshape((-1, 1, 2))
                    cv2.polylines(canvas, [pts_inner], isClosed=True, color=(0, 255, 0), thickness=1)

        cv2.imwrite(f"output/{image['image']}_contrek.png", canvas)

    # result json dump (if -j / --json)
    if args.json:
        os.makedirs("output", exist_ok=True)
        output_data = []
        treemap_data = metadata.get("treemap", [])

        for i, polygon in enumerate(polygons):
            outer_pts = polygon.get('outer', [])
            
            # NumPy conversion
            if len(outer_pts) >= 2:
                pts_array = np.array(outer_pts, dtype=np.int32).reshape(-1, 2)
                outer_formatted = [{"x": int(pt[0]), "y": int(pt[1])} for pt in pts_array]
            else:
                outer_formatted = []

            inner_formatted = []
            for inner in polygon.get('inner', []):
                if len(inner) >= 2:
                    inner_array = np.array(inner, dtype=np.int32).reshape(-1, 2)
                    inner_pts = [{"x": int(pt[0]), "y": int(pt[1])} for pt in inner_array]
                    inner_formatted.append(inner_pts)

            item = {
                "outer": outer_formatted,
                "inner": inner_formatted,
                "treemap": treemap_data[i] if i < len(treemap_data) else None
            }
            output_data.append(item)

        json_path = f"output/{image['image']}_contrek.json"
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(output_data, f, indent=2)

# html report update
file_path = "report.html"
file_ori_path = "report_ori.html"
now_utc = datetime.datetime.now(datetime.timezone.utc)
display_time = now_utc.strftime("%Y-%m-%d %H:%M")

if os.path.exists(file_path):
    source_to_read = file_path
elif os.path.exists(file_ori_path):
    source_to_read = file_ori_path
else:
    print(f"Error: Neither {file_path} nor {file_ori_path} found!")
    sys.exit(1)

with open(source_to_read, "r", encoding="utf-8") as f:
    doc = BeautifulSoup(f, "lxml")

tbody = doc.select_one("#report-body")
if tbody:
    rows = doc.select("tr[count]")
    counts = [int(r["count"]) for r in rows if r.has_attr("count")]
    current_count = max(counts) if counts else 0

    for entry in images:
        image_id = Path(entry["image"]).stem
        target_cell = doc.select_one(f"tr[count='{current_count}'] td[type='ruby'].pending.{image_id}")
        target_row_count = current_count

        if target_cell is None:
            target_row_count = current_count + 1
            new_row_html = f"""
            <tr count="{target_row_count}">
                <td>{target_row_count}</td>
                <td>{display_time}</td>
                <td>{image_id}</td>
                <td>{entry['w']}x{entry['h']}</td>
                <td type="python" class="pending {image_id}">Pending...</td>
                <td type="ruby" class="pending {image_id}">Pending...</td>
            </tr>
            """
            new_row = BeautifulSoup(new_row_html, "html.parser").tr
            tbody.insert(0, new_row)

        contrek_cell = doc.select_one(f"tr[count='{target_row_count}'] td[type='ruby'].pending.{image_id}")
        if contrek_cell:
            formatted_time = f"{entry['time']:.6f} s"
            contrek_content = f"{formatted_time} (polylines outer={entry['outer']}, inner={entry['inner']}, threads/tiles={args.threads}/{args.tiles})"
            contrek_cell.string = contrek_content
            contrek_cell["class"] = [image_id]

    with open(file_path, "w", encoding="utf-8") as f:
        f.write(str(doc))