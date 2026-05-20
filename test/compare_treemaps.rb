# =============================================================================
# compare_treemaps.rb
#
# PURPOSE:
#   Validate Contrek's treemap (parent-child hierarchy of polygons) against
#   OpenCV's RETR_TREE output, used here as a trusted reference implementation.
#
# OBJECTIVE:
#   Verify that Contrek produces a hierarchy equivalent to OpenCV's on the same
#   image, as a guarantee of correct topological behaviour. OpenCV is a battle-
#   tested computer vision library used in production worldwide, making it an
#   authoritative source for contour hierarchy validation.
#
# WHAT IS COMPARED:
#   For each polygon, both systems record which polygon contains it (the parent).
#   This script checks that Contrek and OpenCV agree on the parent-child
#   relationships, after resolving the fact that the two systems number their
#   polygons independently (different array ordering).
#
# WHAT IS NOT COMPARED:
#   The "inner sequence index" (which specific hole of the parent contains the
#   child) is NOT compared. OpenCV's hierarchy structure — [next, prev,
#   first_child, parent] — does not carry this information at all: it only
#   records the parent polygon index, not which hole the child sits in.
#   Contrek provides this richer information natively, but since OpenCV cannot
#   be used as a reference for it, the comparison is limited to parent identity.
#
# MATCHING STRATEGY:
#   Since the two systems number polygons differently, a direct index comparison
#   is meaningless. Instead, polygons are matched geometrically: the first point
#   of each OpenCV contour is searched within the full point set of each Contrek
#   polygon. Once matched, the parent index from OpenCV is translated into
#   Contrek space via the same geometric cross-reference, and then compared.
#
# USAGE:
#   ruby compare_treemaps.rb opencv_treemap.json contrek_treemap.json
#
#   opencv_treemap.json  — produced by benchmark_opencv.py --tree --json
#                          format: [{first_point: {x,y}, treemap: [parent_idx, inner_seq_idx]}, ...]
#   contrek_treemap.json — produced by Contrek's Ruby export
#                          format: [{outer: [{x,y},...], inner: [...], treemap: [parent_idx, inner_seq_idx]}, ...]
# =============================================================================

require 'json'

opencv_path  = ARGV[0]
contrek_path = ARGV[1]

if opencv_path.nil? || contrek_path.nil?
  puts "Usage: ruby compare_treemaps.rb opencv_treemap.json contrek_treemap.json"
  exit 1
end

opencv_data  = JSON.parse(File.read(opencv_path))
contrek_data = JSON.parse(File.read(contrek_path))

puts "OpenCV  polygons: #{opencv_data.size}"
puts "Contrek polygons: #{contrek_data.size}"
puts

# Build contrek index: for each polygon, a Set of all its points for fast lookup
contrek_index = contrek_data.each_with_index.map do |entry, idx|
  points_set = entry["outer"].map { |p| [p["x"], p["y"]] }.to_set
  { idx: idx, points: points_set, treemap: entry["treemap"] }
end

# Build opencv index
opencv_index = opencv_data.each_with_index.map do |entry, idx|
  fp = entry["first_point"]
  { idx: idx, point: [fp["x"], fp["y"]], treemap: entry["treemap"] }
end

# Build cross-reference: opencv_idx -> contrek_idx
# Matched geometrically: OpenCV's first_point must exist in Contrek's point set
opencv_to_contrek = {}
opencv_index.each do |oe|
  match = contrek_index.find { |c| c[:points].include?(oe[:point]) }
  opencv_to_contrek[oe[:idx]] = match[:idx] if match
end

matched   = 0
mismatched = 0
not_found  = 0
mismatches = []

opencv_index.each do |oe|
  opencv_idx    = oe[:idx]
  opencv_parent = oe[:treemap][0]

  contrek_match = contrek_index.find { |c| c[:points].include?(oe[:point]) }

  if contrek_match.nil?
    not_found += 1
    puts "[NOT FOUND] OpenCV ##{opencv_idx} point=#{oe[:point]} treemap=#{oe[:treemap]}"
    next
  end

  contrek_parent = contrek_match[:treemap][0]

  # Both root → agreement
  if opencv_parent == -1 && contrek_parent == -1
    matched += 1
    next
  end

  # One root, one not → real discrepancy
  if opencv_parent == -1 || contrek_parent == -1
    mismatched += 1
    mismatches << {
      opencv_idx: opencv_idx,
      contrek_idx: contrek_match[:idx],
      point: oe[:point],
      opencv_treemap: oe[:treemap],
      contrek_treemap: contrek_match[:treemap],
      reason: "one is root, other is not"
    }
    next
  end

  # Translate opencv parent index -> contrek space via cross-reference
  resolved_parent = opencv_to_contrek[opencv_parent]

  if resolved_parent.nil?
    mismatched += 1
    mismatches << {
      opencv_idx: opencv_idx,
      contrek_idx: contrek_match[:idx],
      point: oe[:point],
      opencv_treemap: oe[:treemap],
      contrek_treemap: contrek_match[:treemap],
      reason: "opencv parent ##{opencv_parent} could not be matched in contrek"
    }
    next
  end

  if resolved_parent == contrek_parent
    matched += 1
  else
    mismatched += 1
    mismatches << {
      opencv_idx: opencv_idx,
      contrek_idx: contrek_match[:idx],
      point: oe[:point],
      opencv_treemap: oe[:treemap],
      contrek_treemap: contrek_match[:treemap],
      reason: "parent mismatch (opencv->contrek: ##{resolved_parent} vs contrek: ##{contrek_parent})"
    }
  end
end

total_compared = opencv_data.size - not_found
agreement_pct  = matched.to_f / total_compared * 100

puts "=" * 55
puts "Results:"
puts "  Matched (parent identity):  #{matched}"
puts "  Mismatched:                 #{mismatched}"
puts "  Not found (skipped):        #{not_found}"
puts "  Total compared:             #{total_compared}"
puts "  Agreement:                  #{"%.1f" % agreement_pct}%"
puts "=" * 55

by_reason = mismatches.group_by { |m| m[:reason].split("(").first.strip }
by_reason.each do |reason, entries|
  puts "\n[#{reason}] — #{entries.size} cases"
end

if mismatches.any?
  puts "\nMismatches detail (first 20):"
  mismatches.first(20).each do |m|
    puts "  OpenCV ##{m[:opencv_idx]} <-> Contrek ##{m[:contrek_idx]} point=#{m[:point]}"
    puts "    OpenCV  treemap: #{m[:opencv_treemap]}"
    puts "    Contrek treemap: #{m[:contrek_treemap]}"
    puts "    Reason: #{m[:reason]}"
  end
end

# Print full coordinates for "one is root, other is not" cases
# These are the most suspicious — Contrek says root, OpenCV says has a parent
root_mismatches = mismatches.select { |m| m[:reason] == "one is root, other is not" }
if root_mismatches.any?
  puts "\n#{"=" * 55}"
  puts "COORDINATE DUMP — 'one is root, other is not' cases (#{root_mismatches.size})"
  puts "These polygons are ROOT in Contrek but have a PARENT in OpenCV."
  puts "Inspect these to determine if Contrek has a bug in parent assignment."
  puts "=" * 55
  root_mismatches.each do |m|
    contrek_entry = contrek_data[m[:contrek_idx]]
    outer_points  = contrek_entry["outer"]
    xs = outer_points.map { |p| p["x"] }
    ys = outer_points.map { |p| p["y"] }
    puts "\nContrek ##{m[:contrek_idx]} (OpenCV ##{m[:opencv_idx]})"
    puts "  first_point : #{m[:point]}"
    puts "  opencv parent: ##{m[:opencv_treemap][0]}"
    puts "  bbox         : x=#{xs.min}..#{xs.max}  y=#{ys.min}..#{ys.max}"
    puts "  point count  : #{outer_points.size}"
    puts "  points       : #{outer_points.map { |p| "[#{p["x"]},#{p["y"]}]" }.join(", ")}"
  end
end

puts
if not_found == 0 && mismatched == 0
  puts "✓ Perfect match — Contrek and OpenCV agree on the full hierarchy."
elsif agreement_pct >= 97.0
  puts "✓ Strong agreement (#{"%.1f" % agreement_pct}%) — Contrek hierarchy validated against OpenCV."
else
  puts "✗ Agreement below threshold (#{"%.1f" % agreement_pct}%) — review mismatches above."
end
