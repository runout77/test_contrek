#!/usr/bin/env python3
"""Contrek Python concurrent progressive-streaming benchmark."""

import argparse
import hashlib
import sys
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import contrek


def memory_usage():
    rss_kib = peak_kib = None
    with open("/proc/self/status", encoding="utf-8") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                rss_kib = int(line.split()[1])
            elif line.startswith("VmHWM:"):
                peak_kib = int(line.split()[1])
    if rss_kib is None or peak_kib is None:
        raise RuntimeError("Unable to read VmRSS/VmHWM from /proc/self/status")
    return rss_kib / 1024.0, peak_kib / 1024.0


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def human_size(size):
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            return f"{value:.2f} {unit}"
        value /= 1024.0


def parse_args():
    p = argparse.ArgumentParser(description="Contrek Python concurrent progressive-streaming benchmark.")
    p.add_argument("--image", required=True, help="Input PNG image.")
    p.add_argument("--stripe-height", type=int, default=1000, help="Stripe height (default: 1000).")
    p.add_argument("--connectivity", type=int, choices=(4, 8), default=4, help="Pixel connectivity (default: 4).")
    p.add_argument("--threads", type=int, default=4, help="Concurrent workers (default: 4).")
    p.add_argument("--output", default="output/streaming_buffer.svg", help="Output SVG.")
    p.add_argument("--expected-sha256", help="Optional expected output SVG SHA-256.")
    return p.parse_args()


def run(args):
    if args.stripe_height <= 0 or args.threads <= 0:
        raise ValueError("stripe height and threads must be greater than zero")

    image_path = Path(args.image)
    output_path = Path(args.output)
    if not image_path.is_file():
        raise FileNotFoundError(f"Input image not found: {image_path}")

    source = contrek.PngSource(str(image_path))
    streamer = contrek.RasterStreamer(source, args.stripe_height)
    buffer_bitmap = contrek.RawBitmap(source.width, args.stripe_height)
    target_color = contrek.rgb_to_target_color(255, 255, 255, 255)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    print("Contrek Python Progressive Streaming Benchmark")
    print(f"IMAGE          {image_path}")
    print(f"RESOLUTION     {source.width}x{source.height}")
    print(f"PIXELS         {source.width * source.height:,}")
    print(f"STRIPE HEIGHT  {args.stripe_height}")
    print(f"CONNECTIVITY   {args.connectivity}")
    print(f"THREADS        {args.threads}")

    rss_start, _ = memory_usage()
    processed_rows = 0
    stripe_count = 0
    jobs = deque()
    started = time.perf_counter()

    def find_stripe(stripe, bitmap, buffer_rows, rows_read):
        options = {
            "processing_height": buffer_rows,
            "versus": "a",
            "bounds": True,
            "connectivity": args.connectivity,
            "compress": {"uniq": True, "linear": True},
        }
        tile = contrek.find_polygons_raw(
            bitmap, options, target_color, contrek.MatchMode.NOT_COLOR,
            None, 0, -1, 0
        )
        return stripe, rows_read, tile

    merger = contrek.SvgStreamingMerger(
        {"bounds": True, "unsafe_mode": True},
        str(output_path),
        source.width,
        source.height,
    )


    with ThreadPoolExecutor(max_workers=args.threads) as executor:
        def merge_oldest():
            nonlocal processed_rows
            stripe, rows_read, tile = jobs.popleft().result()
            processed_rows += rows_read
            merger.add_tile(tile, flush=processed_rows == source.height)
            del tile
            rss, peak = memory_usage()
            print(
                f"stripe={stripe:<3} processed={processed_rows}/{source.height} "
                f"rss={rss:.1f} MiB peak={peak:.1f} MiB"
            )

        def consume(bitmap, buffer_rows, buffer_size, rows_read):
            nonlocal stripe_count
            if len(jobs) >= args.threads:
                merge_oldest()
            stripe = stripe_count
            stripe_count += 1
            detached = bitmap.detach()
            jobs.append(executor.submit(
                find_stripe, stripe, detached, buffer_rows, rows_read
            ))

        streamer.each(buffer_bitmap, consume)
        while jobs:
            merge_oldest()

    result = merger.process_info()
    groups = result["groups"]
    del result
    del merger

    elapsed = time.perf_counter() - started
    rss_end, peak = memory_usage()
    digest = sha256_file(output_path)

    print("\n[Results]")
    print(f"Stripes processed:       {stripe_count}")
    print(f"Total polygons:          {groups:,}")
    print(f"Total elapsed time:      {elapsed:.3f} s")
    print(f"RSS start:               {rss_start:.1f} MiB")
    print(f"RSS end:                 {rss_end:.1f} MiB")
    print(f"Peak RSS:                {peak:.1f} MiB")
    print(f"SVG size:                {human_size(output_path.stat().st_size)}")
    print(f"SVG SHA-256:             {digest}")

    if args.expected_sha256 and digest.lower() != args.expected_sha256.lower():
        print(f"ERROR: expected SHA-256 {args.expected_sha256}", file=sys.stderr)
        return 1
    if args.expected_sha256:
        print("SHA-256 verification:    OK")
    return 0


def main():
    try:
        return run(parse_args())
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
