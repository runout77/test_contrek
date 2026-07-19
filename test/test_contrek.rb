require 'contrek'
require 'rmagick'
require 'nokogiri'
require 'etc'
require 'optparse'
require 'json'

Magick.limit_resource(:memory, 8_000_000_000)
Magick.limit_resource(:map,    8_000_000_000)
Magick.limit_resource(:disk,   16_000_000_000)

n_cores = Etc.nprocessors
n_cores = 8 if n_cores > 8
options = {tiles: n_cores, threads: n_cores, treemap: nil}
OptionParser.new do |opts|
  opts.banner = "Usage: test_contrek.rb [opts]"
  opts.on("-d", "--draw", "Draws into a png image founded polygons.") do |v|
    options[:draw] = v
  end
  opts.on("-t", "--treemap", "Build hierarchy map of founded polygons") do |f|
    options[:treemap] = f
  end
  opts.on("--tiles number_of_tiles", "Number of tiles to divide image (default: #{options[:tiles]})") do |f|
    options[:tiles] = f.to_i
  end
  opts.on("--threads number_of_threads", "Number of threads involved (default: #{options[:threads]})") do |f|
    options[:threads] = f.to_i
  end
  opts.on("-j","--json", "Stores result as json structure") do |f|
    options[:json] = f
  end
end.parse!
puts "Options = #{options.inspect}"
images = [
  {'image': 'test_1024x1024', 'w': 1024, 'h': 1024},
  {'image': 'test_4096x4096', 'w': 4096, 'h': 4096},
  {'image': 'test_10000x10000', 'w': 10000, 'h': 10000},
  {'image': 'test_10240x10240', 'w': 10240, 'h': 10240},
  {'image': 'test_10240x10240_2', 'w': 10240, 'h': 10240},
  {'image': 'test_15360x15360', 'w': 15360, 'h': 15360},
  {'image': 'test_20480x20480', 'w': 20480, 'h': 20480},
]

images.each do |image|
  image_path = "images/#{image[:image]}.png"
  puts "Processing #{image_path} .... "
  
  exclude_color = { r: 255, g: 255, b: 255, a: 255 }
  start_time = Time.now

  result = Contrek.contour!(
    png_file_path: image_path,
    options: {
      number_of_threads: options[:threads],
      class: "value_not_matcher",
      color: exclude_color,
      finder: {
        connectivity: 8,
        number_of_tiles: options[:tiles],
        compress: { uniq: true },
        treemap: options[:treemap]
      }.compact
    }
  )
  polygons = result.polygons
  end_time = Time.now
  puts result.metadata[:benchmarks]
  image[:outer] = polygons.size
  image[:inner] = polygons.sum{|e| e[:inner].count }
  scan_ms = (end_time - start_time)
  image[:time] = scan_ms


### IMAGE
  if options[:draw]
    canvas = Magick::Image.new(image[:w], image[:h]) do |options|
      options.background_color = 'white' 
    end
    gc = Magick::Draw.new
    polygons.each do |obj|
      outer_points = obj[:outer].flatten
      outer_points << outer_points[0] # x1
      outer_points << outer_points[1] # y1
      gc.stroke('red').fill('none').stroke_width(1)
      gc.polyline(*outer_points)
      gc.stroke('green')
      obj[:inner].each do |hole_coords|
        inner_points = hole_coords.flatten
        if inner_points.length >= 2
          inner_points << inner_points[0]
          inner_points << inner_points[1]
          gc.polyline(*inner_points)
        end
      end
    end
    gc.draw(canvas)
    canvas.write("output/#{image[:image]}_contrek.png")
    canvas.destroy!
  end

## JSON output
  if options[:json]
    output = result.polygons.each_with_index.map do |polygon, i|
      { outer: polygon[:outer].each_slice(2).map { |x, y| { x: x, y: y } },
        inner: polygon[:inner].map{|inner| inner.each_slice(2).map { |x, y| { x: x, y: y } }},
        treemap: result.metadata[:treemap][i]
      }
    end
    File.write("output/#{image[:image]}_contrek.json", JSON.pretty_generate(output))
  end
end


### REPORT
file_path = "report.html"
file_ori_path = "report_ori.html"
now = Time.now.utc
display_time = now.strftime("%Y-%m-%d %H:%M")
if File.exist?(file_path)
  source_to_read = file_path
elsif File.exist?(file_ori_path)
  source_to_read = file_ori_path
else
  "Error: #{file_ori_path} not found!"
  exit
end
doc = Nokogiri::HTML(File.read(source_to_read))
tbody = doc.at_css('#report-body')
if tbody
  current_count = (doc.css('tr[count]').map { |tr| tr['count'].to_i }).max 
  current_count = 0 if current_count.nil?

  images.each do |entry|
    image_id = entry[:image]
    target_cell = doc.at_xpath("//tr[@count='#{current_count}']/td[@type='ruby' and contains(@class,'pending') and contains(@class,'#{image_id}')]")
    
    if target_cell.nil?
      row = Nokogiri::XML::Node.new('tr', doc)
      row['count'] = current_count + 1
      row.inner_html = <<-HTML
        <td>#{current_count + 1}</td>
        <td>#{display_time}</td>
        <td>#{entry[:image]}</td>
        <td>#{entry[:w]}x#{entry[:h]}</td>
        <td type="python" class="pending #{image_id}">Pending...</td>
        <td type="ruby" class="pending #{image_id}">Pending...</td>
      HTML
      tbody.prepend_child(row)
    end

    ruby_cell = doc.at_css("tr[count='#{target_cell.nil? ? (current_count + 1) : current_count}'] td[type='ruby'].pending.#{image_id}")
    if ruby_cell
      formatted_time = sprintf("%.6f s", entry[:time])
      ruby_content = "#{formatted_time} (polylines outer=#{entry[:outer]}, inner=#{entry[:inner]}, threads/tiles=#{options[:threads]}/#{options[:tiles]})"
      ruby_cell.content = ruby_content
      ruby_cell['class'] = image_id
    end
  end
  
  File.write(file_path, doc.to_html)
end
