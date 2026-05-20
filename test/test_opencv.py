import cv2
import numpy as np
import time
import os
import sys
import json
from datetime import datetime, timezone

DRAW = "--draw" in sys.argv
USE_TREE = "--tree" in sys.argv
DUMP_JSON = "--json" in sys.argv

if USE_TREE:
    contour_mode = cv2.RETR_TREE
    mode_label = "RETR_TREE"
else:
    contour_mode = cv2.RETR_CCOMP
    mode_label = "RETR_CCOMP"
print(f"Running benchmark using mode: {mode_label}")

images = [
  {'image': 'test_1024x1024', 'w': 1024, 'h': 1024},
  {'image': 'test_4096x4096', 'w': 4096, 'h': 4096},
  {'image': 'test_10000x10000', 'w': 10000, 'h': 10000},
  {'image': 'test_10240x10240', 'w': 10240, 'h': 10240},
  {'image': 'test_10240x10240_2', 'w': 10240, 'h': 10240},
  {'image': 'test_15360x15360', 'w': 15360, 'h': 15360},
  {'image': 'test_20480x20480', 'w': 20480, 'h': 20480},
]

def build_contrek_treemap(contours, hierarchy):
  """
  Builds a flat treemap compatible with Contrek's format:
    [parent_polygon_index, parent_inner_sequence_index]
 
  In OpenCV RETR_TREE, the hierarchy alternates levels:
    level 0 = outer polygons (even depth)
    level 1 = holes/inner sequences of their parent (odd depth)
    level 2 = polygons inside those holes (even depth)
    ...
 
  So for each even-depth contour (polygon), its parent is the
  even-depth ancestor above it, and the inner sequence index is
  determined by which hole (odd-depth node) it sits under.
 
  Returns a list of [parent_polygon_idx, parent_inner_seq_idx] entries,
  one per polygon (even-depth contour), in OpenCV contour order.
  Also returns a mapping from opencv contour index to polygon index.
  """
  if hierarchy is None or len(contours) == 0:
    return [], {}
 
  h = hierarchy[0]
  n = len(contours)

  depth = [0] * n
  for i in range(n):
    parent = h[i][3]
    if parent != -1:
      depth[i] = depth[parent] + 1
 
  polygon_indices = {} 
  poly_idx = 0
  for i in range(n):
    if depth[i] % 2 == 0:
      polygon_indices[i] = poly_idx
      poly_idx += 1

  hole_inner_seq = {}  # opencv_idx -> inner_seq_index
  from collections import defaultdict
  holes_by_parent = defaultdict(list)
  for i in range(n):
    if depth[i] % 2 == 1:
      parent_poly = h[i][3]  
      holes_by_parent[parent_poly].append(i)
 
  for parent_poly, hole_list in holes_by_parent.items():
    hole_list.sort()
    for seq_idx, hole_cv_idx in enumerate(hole_list):
      hole_inner_seq[hole_cv_idx] = seq_idx
  treemap = []
  opencv_to_poly = polygon_indices
 
  for i in range(n):
    if depth[i] % 2 != 0:
      continue
 
    parent_cv = h[i][3]  
    if parent_cv == -1:
      treemap.append([-1, -1])
    else:
      # parent_cv is a hole (odd depth)
      # the polygon containing this hole is the hole's parent
      grandparent_cv = h[parent_cv][3]
      parent_poly_idx = opencv_to_poly[grandparent_cv]
      inner_seq_idx = hole_inner_seq[parent_cv]
      treemap.append([parent_poly_idx, inner_seq_idx])
 
  return treemap, opencv_to_poly

for image in images:
  image_path = f"images/{image['image']}.png" 
  print(f"Processing {image_path} ....")
  start = time.time()
  img = cv2.imread(image_path, cv2.IMREAD_UNCHANGED)
  t_img = time.time()

  if img is None:
    print("Error: Image not found.")
    exit()

  channels = img.shape[2] if len(img.shape) > 2 else 1
  if channels == 4:
    exclude_color = np.array([255, 255, 255, 255], dtype=np.uint8)
  else:
    exclude_color = np.array([255, 255, 255], dtype=np.uint8)

  mask = cv2.inRange(img, exclude_color, exclude_color)
  mask = cv2.bitwise_not(mask) 
  contours, hierarchy = cv2.findContours(mask, contour_mode, cv2.CHAIN_APPROX_SIMPLE)

  external_count = 0
  internal_count = 0
  if hierarchy is not None:
    for i in range(len(contours)):
      parent = hierarchy[0][i][3]
      if parent == -1:
        external_count += 1
      else:
        internal_count += 1
  image['inner'] = internal_count
  image['outer'] = external_count

  t_proc = time.time()
  image['time'] = t_proc - start

  if DUMP_JSON and USE_TREE:
    treemap, opencv_to_poly = build_contrek_treemap(contours, hierarchy)
    os.makedirs("output", exist_ok=True)
    h_d = hierarchy[0]
    n_c = len(contours)
    depth = [0] * n_c
    for i in range(n_c):
      par = h_d[i][3]
      if par != -1:
        depth[i] = depth[par] + 1
    output = []
    poly_idx = 0
    for i in range(n_c):
      if depth[i] % 2 != 0:
        continue
      first = contours[i][0][0]
      output.append({
        "first_point": {"x": int(first[0]), "y": int(first[1])},
        "treemap": treemap[poly_idx]
      })
      poly_idx += 1
    json_path = f"output/{image['image']}_opencv.json"
    with open(json_path, 'w') as f:
      json.dump(output, f, indent=2)
    roots = sum(1 for e in output if e["treemap"][0] == -1)
    print(f"  -> Treemap JSON saved: {json_path} ({len(output)} polygons, {roots} roots)")
  elif DUMP_JSON and not USE_TREE:
    print("  -> --json requires --tree to produce a meaningful hierarchy, skipping.")

  if DRAW:
    result = np.full(img.shape[:2] + (3,), 255, dtype=np.uint8)
    if hierarchy is not None:
      h_data = hierarchy[0]
      num_contours = len(contours)
      levels = np.zeros(num_contours, dtype=int)
      for i in range(num_contours):
          parent = h_data[i][3]
          if parent != -1:
              levels[i] = levels[parent] + 1
      pari = [contours[i] for i in range(num_contours) if levels[i] % 2 == 0]
      dispari = [contours[i] for i in range(num_contours) if levels[i] % 2 != 0]

      if pari:
          cv2.drawContours(result, pari, -1, (0, 0, 255), 1)  
      if dispari:
          cv2.drawContours(result, dispari, -1, (0, 128, 0), 1)
    end = time.time()
    cv2.imwrite(f"output/{image['image']}_opencv.png", result)



import os
import datetime
from bs4 import BeautifulSoup

file_path = "report.html"
original_template = "report_ori.html"
now_utc = datetime.datetime.now(datetime.timezone.utc)
display_time = now_utc.strftime("%Y-%m-%d %H:%M")
if os.path.exists(file_path):
    source_to_read = file_path
    print(f"Updating existing report: {file_path}")
elif os.path.exists(original_template):
    source_to_read = original_template
    print(f"Report not found. Using default template: {original_template}")
else:
    print(f"Errore: Nor {file_path} or {original_template} found!")
    exit()
with open(source_to_read, 'r', encoding='utf-8') as f:
    doc = BeautifulSoup(f, 'lxml')
tbody = doc.select_one('#report-body')
if tbody:
    rows = doc.select('tr[count]')
    counts = [int(r['count']) for r in rows if r.has_attr('count')]
    current_count = max(counts) if counts else 0
    for entry in images:
        image_id = entry['image']
        target_cell = doc.select_one(f"tr[count='{current_count}'] td[type='python'].pending.{image_id}")
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
            new_row = BeautifulSoup(new_row_html, 'html.parser').tr
            tbody.insert(0, new_row)          
        python_cell = doc.select_one(f"tr[count='{target_row_count}'] td[type='python'].pending.{image_id}")     
        if python_cell:
            formatted_time = f"{entry['time']:.6f} s"
            python_content = f"{formatted_time} (polylines outer={entry['outer']}, inner={entry['inner']})"          
            python_cell.string = python_content
            python_cell['class'] = [image_id]
    with open(file_path, 'w', encoding='utf-8') as f:
        f.write(str(doc))
