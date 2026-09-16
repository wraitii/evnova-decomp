FILES = ARGV

ACTIVE_MISSION_BASE = 0x295e
ACTIVE_MISSION_SIZE = 0x8e6
ACTIVE_MISSION_NAME_RANGES = 16.times.flat_map do |slot|
  record = ACTIVE_MISSION_BASE + slot * ACTIVE_MISSION_SIZE
  [record + 0x6d...record + 0xad, record + 0xad...record + 0xed]
end.freeze

def u32le(data, offset)
  data.byteslice(offset, 4).unpack1("V")
end

def pilot_name_from_path(path)
  File.basename(path, File.extname(path))
end

def pilot_nickname(data)
  field = data.byteslice(0x5d98, 0x40) || ""
  length = field.getbyte(0)
  if length && length.between?(1, 0x3f) && field.bytesize > length
    raw = field.byteslice(1, length)
  else
    raw = field.split("\0", 2).first
  end
  raw.force_encoding("macRoman").encode("UTF-8", invalid: :replace,
                                         undef: :replace, replace: "?")
end
def decode_block(data, force: false)
  bytes = data.bytes
  return data if bytes.length < 2
  first = bytes[0] | (bytes[1] << 8)
  return data if !force && first < 0x800
  key = 0xb36a210f
  offset = 0
  while offset < bytes.length
    4.times do |byte|
      break if offset >= bytes.length
      bytes[offset] ^= (key >> (24 - byte * 8)) & 0xff
      offset += 1
    end
    key = ((key + 0xdeadbeef) & 0xffffffff) ^ 0xdeadbeef
  end
  bytes.pack("C*")
end
def ascii_runs(data, minimum = 4)
  results = []
  start = nil
  data.bytes.each_with_index do |byte, offset|
    if byte == 0x20 || byte == 0x27 || byte == 0x2d ||
       byte.between?(0x41, 0x5a) || byte.between?(0x61, 0x7a)
      start ||= offset
    elsif start
      text = data.byteslice(start, offset - start)
      results << [start, text] if text.length >= minimum && text.match?(/[A-Za-z]{4}/)
      start = nil
    end
  end
  results
end
def macroman_c_strings(data, minimum = 4)
  results = []
  start = 0
  data.bytes.each_with_index do |byte, offset|
    next unless byte.zero?
    raw = data.byteslice(start, offset - start)
    if raw.length.between?(minimum, 80) && raw.bytes.all? { |b| b >= 0x20 && b != 0x7f }
      text = raw.force_encoding("macRoman").encode("UTF-8", invalid: :replace,
                                                    undef: :replace, replace: "?")
      letters = text.scan(/[A-Za-zÀ-ÖØ-öø-ÿ]/).length
      results << [start, text] if letters >= minimum && letters * 2 >= text.length
    end
    start = offset + 1
  end
  results
end
def utf16_latin_runs(data, endian, minimum = 4)
  results = []
  [0, 1].each do |alignment|
    start = nil
    offset = alignment
    while offset + 1 < data.bytesize
      pair = data.byteslice(offset, 2).bytes
      code = endian == :le ? pair[0] | (pair[1] << 8) : (pair[0] << 8) | pair[1]
      latin = code.between?(0x20, 0x7e) || code.between?(0x00c0, 0x024f)
      if latin
        start ||= offset
      elsif start
        bytes = data.byteslice(start, offset - start)
        text = bytes.force_encoding(endian == :le ? "UTF-16LE" : "UTF-16BE").encode("UTF-8")
        results << [start, text] if text.length >= minimum && text.match?(/[A-Za-zÀ-ÖØ-öø-ÿ]/)
        start = nil
      end
      offset += 2
    end
  end
  results.uniq
end
def report(label, data, excluded_ranges: [])
  puts "-- #{label} --"
  scanned = data.dup
  excluded_ranges.each do |range|
    range.each { |offset| scanned.setbyte(offset, 0) if offset < scanned.bytesize }
  end
  {"ASCII" => ascii_runs(scanned),
   "MacRoman NUL strings" => macroman_c_strings(scanned),
   "UTF-16LE Latin" => utf16_latin_runs(scanned, :le),
   "UTF-16BE Latin" => utf16_latin_runs(scanned, :be)}.each do |kind, rows|
    puts kind
    rows.uniq.each { |offset, value| puts format("  0x%05x  %p", offset, value) }
  end
end
FILES.each do |path|
  raw = File.binread(path)
  size1 = u32le(raw, 0)
  size2_at = 4 + size1
  size2 = u32le(raw, size2_at)
  block1 = raw.byteslice(4, size1)
  block2 = raw.byteslice(size2_at + 4, size2)
  trailer = raw.byteslice(size2_at + 4 + size2, raw.bytesize) || ""
  decoded2 = decode_block(block2)
  big_endian = decoded2.byteslice(0, 2).unpack1("n") == 300
  decoded1 = decode_block(block1, force: big_endian)
  puts "=== #{File.basename(path)} ==="
  puts format("Pilot name (from filename): %p", pilot_name_from_path(path))
  puts format("Pilot nickname (block 2 +0x5d98): %p", pilot_nickname(decoded2))
  puts format("Player ship name (trailer): %p", trailer.delete_suffix("\0"))
  # These buffers hold generated active-mission names. They are unrelated to
  # the player ship-name trailer and must not be treated as recovery candidates.
  report("decoded block 1", decoded1,
         excluded_ranges: ACTIVE_MISSION_NAME_RANGES)
  report("decoded block 2", decoded2)
  report("trailer", trailer)
end
