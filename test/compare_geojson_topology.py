#!/usr/bin/env python3
"""Compare Contrek/GDAL GeoJSON using raster-grid coordinates and GEOS topology.

Requires: pip install shapely
Optional for --raster: pip install rasterio

Example:
 python3 compare_geojson_topology.py contrek.geojson gdal.geojson \
   --raster ESA_WorldCover_10m_2021_v200_S03W060_Map.tif --report comparison

If rasterio is unavailable, use --grid X_ORIGIN Y_ORIGIN PIXEL_WIDTH PIXEL_HEIGHT.
The raster must have a north-up, unrotated affine transform.
"""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import sys

from shapely.geometry import Polygon

KEYS = ('PixelVal', 'DN', 'class', 'Class', 'value', 'Value', 'raster_val', 'gridcode')


def cls_value(value, packed):
    if packed and isinstance(value, (int, float)) and float(value).is_integer():
        v = int(value)
        if 0 <= v <= 0xffffffff and v >> 24 == 255 and (v >> 16 & 255) == (v >> 8 & 255) == (v & 255):
            return v & 255
    return value


def read(path, field, packed):
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    if data.get('type') != 'FeatureCollection':
        raise ValueError(f'{path}: expected FeatureCollection')
    features = data.get('features', [])
    if not features:
        raise ValueError(f'{path}: empty FeatureCollection')
    if field is None:
        props = features[0].get('properties') or {}
        field = next((k for k in KEYS if k in props), None)
        if field is None:
            raise ValueError(f'{path}: class field not found')
    return features, field


def grid_point(pt, grid):
    x0, y0, dx, dy = grid
    fx = (float(pt[0]) - x0) / dx
    fy = (float(pt[1]) - y0) / dy
    ix, iy = round(fx), round(fy)
    if abs(fx-ix) > 0.01 or abs(fy-iy) > 0.01:
        raise ValueError(f'coordinate {pt[:2]} is not on raster grid; pixel coords {(fx, fy)}')
    return (ix, iy)


def clean_ring(coords, grid):
    pts = [grid_point(pt, grid) for pt in coords]
    if not pts or pts[0] != pts[-1]:
        raise ValueError('ring not closed on raster grid')
    pts = [p for i, p in enumerate(pts[:-1]) if i == 0 or p != pts[i-1]]
    if len(set(pts)) < 3:
        raise ValueError('degenerate ring on raster grid')
    # Drop redundant collinear vertices, including across ring closure.
    while True:
        out = []
        for i, p in enumerate(pts):
            a, b = pts[i-1], pts[(i+1) % len(pts)]
            cross = (p[0]-a[0])*(b[1]-p[1])-(p[1]-a[1])*(b[0]-p[0])
            between = min(a[0],b[0]) <= p[0] <= max(a[0],b[0]) and min(a[1],b[1]) <= p[1] <= max(a[1],b[1])
            if cross != 0 or not between:
                out.append(p)
        if len(out) == len(pts) or len(out) < 3:
            break
        pts = out
    return pts + [pts[0]]


def polygons(feature, grid):
    g = feature.get('geometry') or {}
    if g.get('type') == 'Polygon':
        parts = [g['coordinates']]
    elif g.get('type') == 'MultiPolygon':
        parts = g['coordinates']
    else:
        raise ValueError(f"unsupported geometry type {g.get('type')}")
    return [Polygon(clean_ring(part[0], grid), [clean_ring(h, grid) for h in part[1:]]) for part in parts]


def bbox_key(geoms):
    return tuple(int(v) for v in (min(g.bounds[0] for g in geoms), min(g.bounds[1] for g in geoms),
                                  max(g.bounds[2] for g in geoms), max(g.bounds[3] for g in geoms)))


def equivalent(a, b):
    if len(a) != len(b):
        return False
    remaining = list(b)
    for p in a:
        match = next((i for i, q in enumerate(remaining) if p.equals(q)), None)
        if match is None:
            return False
        remaining.pop(match)
    return not remaining


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('contrek')
    ap.add_argument('gdal')
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument('--raster', help='Source raster, used to recover integer pixel coordinates')
    group.add_argument('--grid', nargs=4, type=float, metavar=('X0','Y0','DX','DY'), help='Raster origin and pixel size')
    ap.add_argument('--class-contrek')
    ap.add_argument('--class-gdal')
    ap.add_argument('--raw-contrek-class', action='store_true', help='Disable automatic opaque grayscale RGBA decoding')
    ap.add_argument('--report', default='comparison')
    ap.add_argument('--examples', type=int, default=20)
    args = ap.parse_args()
    if args.examples < 0:
        ap.error('--examples must be >= 0')
    try:
        if args.raster:
            try:
                import rasterio
            except ImportError as e:
                raise ValueError('rasterio required for --raster; install it or use --grid') from e
            with rasterio.open(args.raster) as ds:
                t = ds.transform
                if abs(t.b) > 1e-12 or abs(t.d) > 1e-12:
                    raise ValueError('rotated raster not supported; use a north-up raster')
                grid = (t.c, t.f, t.a, t.e)
        else:
            grid = tuple(args.grid)
        if grid[2] == 0 or grid[3] == 0:
            raise ValueError('pixel size cannot be zero')
        af, ak = read(args.contrek, args.class_contrek, not args.raw_contrek_class)
        bf, bk = read(args.gdal, args.class_gdal, False)
        # Bucket by normalized class and exact raster-pixel bounding box.
        # This avoids an O(N^2) global spatial comparison and prevents cross-class matches.
        buckets = defaultdict(lambda: [[], []])
        stats = [dict(features=0, parts=0, holes=0, invalid=0, classes=Counter()),
                 dict(features=0, parts=0, holes=0, invalid=0, classes=Counter())]
        for side, (features, key) in enumerate(((af, ak), (bf, bk))):
            print(f'Processing {"Contrek" if side == 0 else "GDAL"}: {len(features):,} features', flush=True)
            for i, feature in enumerate(features, 1):
                try:
                    cls = cls_value((feature.get('properties') or {})[key], side == 0 and not args.raw_contrek_class)
                    geoms = polygons(feature, grid)
                    bucket_key = (str(cls), bbox_key(geoms))
                except (KeyError, ValueError, TypeError, IndexError) as e:
                    raise ValueError(f'{"Contrek" if side == 0 else "GDAL"} feature #{i}: {e}') from e
                s = stats[side]
                s['features'] += 1
                s['parts'] += len(geoms)
                s['holes'] += sum(len(p.interiors) for p in geoms)
                s['invalid'] += any(not p.is_valid for p in geoms)
                s['classes'][str(cls)] += 1
                buckets[bucket_key][side].append((i, geoms))
                if i % 100000 == 0:
                    print(f'  {i:,} processed', flush=True)
        matched = 0
        mismatches = []
        missing_c = missing_g = 0
        for (cls, bbox), (ca, gb) in buckets.items():
            # Cheap path: exact topology of pixel-grid polygons; GEOS handles
            # rotated ring starts, winding, and redundant collinear vertices.
            available = list(gb)
            for ci, cg in ca:
                hit = next((j for j, (_, gg) in enumerate(available) if equivalent(cg, gg)), None)
                if hit is None:
                    missing_c += 1
                    if len(mismatches) < args.examples:
                        mismatches.append({'class': cls, 'pixel_bbox': bbox, 'contrek_feature': ci,
                                           'gdal_candidates': [j for j, _ in available[:5]],
                                           'contrek_valid': all(p.is_valid for p in cg),
                                           'contrek_holes': sum(len(p.interiors) for p in cg)})
                else:
                    matched += 1
                    available.pop(hit)
            missing_g += len(available)
        summary = {'matched': matched, 'only_contrek': missing_c, 'only_gdal': missing_g,
                   'matching_percent': round(100 * matched / len(af), 5),
                   'same_class_counts': stats[0]['classes'] == stats[1]['classes'],
                   'same_holes': stats[0]['holes'] == stats[1]['holes']}
        status = 'PASS' if missing_c == missing_g == 0 else 'DIFFERENCES'
        result = {'status': status, 'method': 'integer raster-grid coordinates + GEOS topological equals',
                  'grid': {'origin_x': grid[0], 'origin_y': grid[1], 'pixel_width': grid[2], 'pixel_height': grid[3]},
                  'contrek': {**stats[0], 'classes': dict(stats[0]['classes'])},
                  'gdal': {**stats[1], 'classes': dict(stats[1]['classes'])},
                  'summary': summary, 'mismatch_examples': mismatches}
        prefix = Path(args.report)
        prefix.parent.mkdir(parents=True, exist_ok=True)
        prefix.with_suffix('.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        lines = ['CONTREK vs GDAL — RASTER GRID TOPOLOGICAL COMPARISON', '='*60,
                 f'Status: {status}', f'Grid: {grid}',
                 f'Features: {len(af):,} / {len(bf):,}',
                 f'Polygons: {stats[0]["parts"]:,} / {stats[1]["parts"]:,}',
                 f'Holes: {stats[0]["holes"]:,} / {stats[1]["holes"]:,}',
                 f'Invalid polygons: {stats[0]["invalid"]:,} / {stats[1]["invalid"]:,}',
                 f'Matching: {matched:,} ({summary["matching_percent"]}%)',
                 f'Only Contrek: {missing_c:,}', f'Only GDAL: {missing_g:,}',
                 f'Class counts identical: {summary["same_class_counts"]}', '',
                 'UNMATCHED EXAMPLES', '-'*60]
        for ex in mismatches:
            lines.append(f'Contrek #{ex["contrek_feature"]}, class={ex["class"]}, bbox(px)={ex["pixel_bbox"]}, GDAL candidates={ex["gdal_candidates"]}, valid={ex["contrek_valid"]}')
        lines += ['', 'NOTE: Equals checks coordinate-set equality on raster-grid polygons, including invalid geometries where GEOS can evaluate them.',
                  'NOTE: Different polygon partitioning may require a separate union-based comparison.']
        prefix.with_suffix('.txt').write_text('\n'.join(lines)+'\n', encoding='utf-8')
        print('\n'.join(lines))
        print(f'Reports: {prefix.with_suffix(".txt")}, {prefix.with_suffix(".json")}')
        return 0 if status == 'PASS' else 1
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
