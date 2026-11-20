#!/usr/bin/env python3
"""Compare Contrek and GDAL GeoJSON polygon outputs in memory.

Example:
  python compare_geojson_memory.py contrek.geojson gdal.geojson --tolerance 0.000001
  python compare_geojson_memory.py contrek.geojson gdal.geojson --tolerance 0
No external dependencies. Writes detailed .txt and .json reports.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import sys


def normalize_class(value, mode):
    """Convert Contrek opaque FF/R/G/B grayscale pixels to GDAL band values."""
    if mode == "raw":
        return value
    if isinstance(value, (int, float)) and not isinstance(value, bool) and float(value).is_integer():
        packed = int(value)
        if 0 <= packed <= 0xFFFFFFFF and (packed >> 24) == 0xFF:
            r, g, b = (packed >> 16) & 255, (packed >> 8) & 255, packed & 255
            if r == g == b:
                return r
    if mode == "rgba-gray":
        raise ValueError(f"expected opaque grayscale RGBA class, got {value!r}")
    return value

CLASS_KEYS = ('PixelVal', 'DN', 'class', 'Class', 'value', 'Value', 'raster_val', 'gridcode')


def min_rotation(seq):
    n = len(seq)
    if not n:
        return ()
    doubled = seq + seq
    i, j, k = 0, 1, 0
    while i < n and j < n and k < n:
        a, b = doubled[i+k], doubled[j+k]
        if a == b:
            k += 1
        else:
            if a > b:
                i += k+1
                if i <= j: i = j+1
            else:
                j += k+1
                if j <= i: j = i+1
            k = 0
    return tuple(doubled[min(i,j):min(i,j)+n])


def canonical_ring(ring, tolerance):
    if len(ring) < 4:
        raise ValueError('ring with fewer than four points')
    points = []
    for p in ring:
        if len(p) < 2:
            raise ValueError('invalid coordinate')
        if tolerance:
            points.append((round(float(p[0])/tolerance), round(float(p[1])/tolerance)))
        else:
            points.append((float(p[0]), float(p[1])))
    if points[0] != points[-1]:
        raise ValueError('ring not closed')
    points.pop()
    # Keep degenerate rings in the comparison; report them separately.
    return min(min_rotation(points), min_rotation(points[::-1]))


def signature(feature, key, tolerance, class_mode):
    props = feature.get('properties') or {}
    if key not in props:
        raise ValueError(f'missing class property {key!r}')
    cls = normalize_class(props[key], class_mode)
    if isinstance(cls, (int, float)) and not isinstance(cls, bool):
        cls = float(cls)
    geometry = feature.get('geometry') or {}
    typ = geometry.get('type')
    coords = geometry.get('coordinates')
    if typ == 'Polygon':
        polys = [coords]
    elif typ == 'MultiPolygon':
        polys = coords
    else:
        raise ValueError(f'unsupported geometry type: {typ}')
    if not polys:
        raise ValueError('empty geometry')
    normalized = []
    hole_count = 0
    degeneracies = []
    for poly in polys:
        if not poly:
            raise ValueError('polygon without rings')
        for ring_index, ring in enumerate(poly):
            original_unique = len({(float(pt[0]), float(pt[1])) for pt in ring[:-1]})
            if original_unique < 3:
                degeneracies.append({'ring': ring_index, 'reason': 'degenerate in original coordinates', 'unique_vertices': original_unique})
            elif tolerance and len({(round(float(pt[0])/tolerance), round(float(pt[1])/tolerance)) for pt in ring[:-1]}) < 3:
                degeneracies.append({'ring': ring_index, 'reason': 'collapsed by tolerance', 'unique_vertices': original_unique})
        ext = canonical_ring(poly[0], tolerance)
        holes = sorted(canonical_ring(r, tolerance) for r in poly[1:])
        hole_count += len(holes)
        normalized.append((ext, holes))
    normalized.sort()
    digest = hashlib.sha256(json.dumps([cls, normalized], separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
    bbox = [min(p[0] for poly in polys for ring in poly for p in ring),
            min(p[1] for poly in polys for ring in poly for p in ring),
            max(p[0] for poly in polys for ring in poly for p in ring),
            max(p[1] for poly in polys for ring in poly for p in ring)]
    return digest, str(cls), len(polys), hole_count, bbox, degeneracies


def detect_key(features, explicit):
    if explicit:
        return explicit
    if not features:
        raise ValueError('no features')
    props = features[0].get('properties') or {}
    for key in CLASS_KEYS:
        if key in props:
            return key
    raise ValueError(f'cannot detect class property among {list(props)}; specify --class-contrek/--class-gdal')


def process(path, key_override, tolerance, class_mode):
    print(f'Loading {path}...', flush=True)
    with open(path, encoding='utf-8') as f:
        doc = json.load(f)
    if doc.get('type') != 'FeatureCollection' or not isinstance(doc.get('features'), list):
        raise ValueError(f'{path}: expected complete GeoJSON FeatureCollection')
    features = doc['features']
    key = detect_key(features, key_override)
    counts, classes = Counter(), Counter()
    examples = {}
    parts = holes = 0
    degenerate_features = 0
    degenerate_examples = []
    for i, feat in enumerate(features, 1):
        try:
            digest, cls, n_parts, n_holes, bbox, degeneracies = signature(feat, key, tolerance, class_mode)
        except (ValueError, TypeError, IndexError, KeyError) as e:
            raise ValueError(f'{path}: feature #{i}: {e}') from e
        if degeneracies:
            degenerate_features += 1
            if len(degenerate_examples) < 20:
                degenerate_examples.append({'feature_index': i, 'class': cls, 'bbox': bbox, 'rings': degeneracies})
        counts[digest] += 1
        classes[cls] += 1
        parts += n_parts
        holes += n_holes
        if digest not in examples:
            examples[digest] = {'feature_index': i, 'class': cls, 'bbox': bbox,
                                'polygon_parts': n_parts, 'holes': n_holes, 'signature': digest}
        if i % 100000 == 0:
            print(f'  {i:,} features processed', flush=True)
    return {'file': str(path), 'class_property': key, 'class_normalization': class_mode, 'features': len(features),
            'polygon_parts': parts, 'holes': holes, 'classes': dict(classes),
            'degenerate_features': degenerate_features, 'degenerate_examples': degenerate_examples}, counts, examples


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('contrek')
    ap.add_argument('gdal')
    ap.add_argument('--tolerance', type=float, default=0.000001, help='Coordinate quantization in CRS units; 0 = exact numeric comparison')
    ap.add_argument('--class-contrek')
    ap.add_argument('--class-gdal')
    ap.add_argument('--contrek-class-format', choices=('auto', 'rgba-gray', 'raw'), default='auto',
                    help='auto converts opaque grayscale RGBA packed values to GDAL class values')
    ap.add_argument('--report', default='geojson_comparison', help='Report file prefix')
    ap.add_argument('--examples', type=int, default=20, help='Maximum mismatch examples per side')
    args = ap.parse_args()
    if args.tolerance < 0 or args.examples < 0:
        ap.error('tolerance and examples must be nonnegative')
    try:
        a, ac, ae = process(args.contrek, args.class_contrek, args.tolerance, args.contrek_class_format)
        b, bc, be = process(args.gdal, args.class_gdal, args.tolerance, "raw")
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2
    only_a, only_b = ac-bc, bc-ac
    matched = sum((ac & bc).values())
    unmatched_a, unmatched_b = sum(only_a.values()), sum(only_b.values())
    per_class = []
    for cls in sorted(set(a['classes']) | set(b['classes']), key=lambda x: (float(x) if x.replace('.','',1).lstrip('-').isdigit() else float('inf'), x)):
        per_class.append({'class': cls, 'contrek': a['classes'].get(cls,0), 'gdal': b['classes'].get(cls,0),
                          'delta': a['classes'].get(cls,0)-b['classes'].get(cls,0)})
    examples_a = [{**ae[d], 'unmatched_copies': n} for d,n in only_a.most_common(args.examples)]
    examples_b = [{**be[d], 'unmatched_copies': n} for d,n in only_b.most_common(args.examples)]
    passed = unmatched_a == unmatched_b == 0
    result = {'status': 'PASS' if passed else 'DIFFERENCES', 'tolerance_crs_units': args.tolerance,
              'comparison': 'class + canonical polygon rings; independent of feature order, ring direction, ring start, and hole order',
              'contrek': a, 'gdal': b,
              'summary': {'matched_features': matched, 'only_contrek': unmatched_a, 'only_gdal': unmatched_b,
                          'exteriors_or_parts_equal': a['polygon_parts']==b['polygon_parts'],
                          'holes_equal': a['holes']==b['holes'], 'class_counts_equal': a['classes']==b['classes']},
              'per_class': per_class,
              'examples_only_contrek': examples_a, 'examples_only_gdal': examples_b}
    lines = [
        'CONTREK vs GDAL — GEOJSON COMPARISON', '='*60,
        f'Coordinate tolerance (CRS units): {args.tolerance}',
        'Comparison: class + polygon rings, normalized for order/orientation/start', '',
        f'{"METRIC":<28} {"CONTREK":>16} {"GDAL":>16}', '-'*62,
        f'{"Features":<28} {a["features"]:>16,} {b["features"]:>16,}',
        f'{"Polygon parts":<28} {a["polygon_parts"]:>16,} {b["polygon_parts"]:>16,}',
        f'{"Holes":<28} {a["holes"]:>16,} {b["holes"]:>16,}',
        f'{"Degenerate-ring features":<28} {a["degenerate_features"]:>16,} {b["degenerate_features"]:>16,}',
        f'{"Distinct classes":<28} {len(a["classes"]):>16,} {len(b["classes"]):>16,}', '',
        f'Class fields: Contrek={a["class_property"]}, GDAL={b["class_property"]}',
        f'Contrek class normalization: {args.contrek_class_format}', '',
        'CLASS COUNTS', '-'*62,
        f'{"CLASS":<18} {"CONTREK":>14} {"GDAL":>14} {"DELTA":>12}'
    ]
    for row in per_class:
        lines.append(f'{row["class"]:<18} {row["contrek"]:>14,} {row["gdal"]:>14,} {row["delta"]:>+12,}')
    lines += ['', 'GEOMETRY COMPARISON', '-'*62,
              f'Matching features:      {matched:,}',
              f'Only in Contrek:        {unmatched_a:,}',
              f'Only in GDAL:           {unmatched_b:,}',
              f'Class counts identical: {a["classes"] == b["classes"]}',
              f'Polygon parts identical:{a["polygon_parts"] == b["polygon_parts"]}',
              f'Hole counts identical:  {a["holes"] == b["holes"]}']
    for title, samples in [('CONTREK-ONLY EXAMPLES',examples_a),('GDAL-ONLY EXAMPLES',examples_b)]:
        lines += ['', title, '-'*62]
        if not samples:
            lines.append('(none)')
        for s in samples:
            lines.append(f'feature #{s["feature_index"]}, class={s["class"]}, copies={s["unmatched_copies"]}, bbox={s["bbox"]}, holes={s["holes"]}')
            lines.append(f'  signature={s["signature"]}')
    for label, side in [('CONTREK DEGENERATE-RING EXAMPLES', a), ('GDAL DEGENERATE-RING EXAMPLES', b)]:
        lines += ['', label, '-'*62]
        if not side['degenerate_examples']:
            lines.append('(none)')
        for ex in side['degenerate_examples']:
            lines.append(f"feature #{ex['feature_index']}, class={ex['class']}, bbox={ex['bbox']}, rings={ex['rings']}")
    lines += ['', f'RESULT: {result["status"]}',
              'NOTE: Ring-coordinate equality is not a general GEOS topological equivalence test.',
              'NOTE: Quantization may hide differences smaller than the selected tolerance.']
    text = '\n'.join(lines)+'\n'
    prefix = Path(args.report)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    prefix.with_suffix('.txt').write_text(text, encoding='utf-8')
    prefix.with_suffix('.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    print('\n'+text)
    print(f'Reports: {prefix.with_suffix(".txt")} and {prefix.with_suffix(".json")}')
    return 0 if passed else 1

if __name__ == '__main__':
    sys.exit(main())