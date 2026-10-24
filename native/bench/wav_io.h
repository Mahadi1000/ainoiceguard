/**
 * Minimal WAV reader/writer for the offline benchmark.
 *
 * Reads:  RIFF/WAVE, PCM 16/24/32-bit or IEEE float 32-bit, including
 *         WAVE_FORMAT_EXTENSIBLE. Any channel count (caller decides how to
 *         handle non-mono input).
 * Writes: mono PCM 16-bit or IEEE float 32-bit.
 *
 * Dev tooling only -- not linked into the addon.
 */

#ifndef AINOICEGUARD_WAV_IO_H
#define AINOICEGUARD_WAV_IO_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ainoiceguard {
namespace wav {

struct Audio {
  int sampleRate = 0;
  int channels = 0;
  std::vector<float> samples;  /* interleaved, [-1, 1] */

  size_t frames() const { return channels ? samples.size() / channels : 0; }
};

enum class OutFormat { Float32, Pcm16 };

namespace detail {

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
inline void wr16(std::FILE* f, uint16_t v) {
  uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
  std::fwrite(b, 1, 2, f);
}
inline void wr32(std::FILE* f, uint32_t v) {
  uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

}  // namespace detail

/** Read a WAV file. Returns empty string on success, otherwise an error. */
inline std::string read(const std::string& path, Audio& out) {
  using namespace detail;

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "cannot open " + path;
  std::vector<uint8_t> buf;
  uint8_t chunk[65536];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    buf.insert(buf.end(), chunk, chunk + n);
  }
  std::fclose(f);

  if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 ||
      std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
    return "not a RIFF/WAVE file: " + path;
  }

  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t* data = nullptr;
  size_t dataSize = 0;

  size_t pos = 12;
  while (pos + 8 <= buf.size()) {
    const uint8_t* id = buf.data() + pos;
    uint32_t size = rd32(id + 4);
    const uint8_t* body = id + 8;
    size_t avail = buf.size() - (pos + 8);
    if (size > avail) size = uint32_t(avail); /* tolerate truncated data chunk */

    if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
      format = rd16(body);
      channels = rd16(body + 2);
      rate = rd32(body + 4);
      bits = rd16(body + 14);
      if (format == 0xFFFE && size >= 40) {
        format = rd16(body + 24); /* first two bytes of SubFormat GUID */
      }
    } else if (std::memcmp(id, "data", 4) == 0) {
      data = body;
      dataSize = size;
    }
    pos += 8 + size + (size & 1);
  }

  if (!data) return "no data chunk: " + path;
  if (channels == 0) return "no fmt chunk: " + path;

  const bool isFloat = (format == 3 && bits == 32);
  const bool isPcm = (format == 1 && (bits == 16 || bits == 24 || bits == 32));
  if (!isFloat && !isPcm) {
    return "unsupported format " + std::to_string(format) + "/" +
           std::to_string(bits) + "-bit: " + path;
  }

  const size_t bytesPer = bits / 8;
  const size_t count = dataSize / bytesPer;
  out.sampleRate = int(rate);
  out.channels = channels;
  out.samples.resize(count);

  for (size_t i = 0; i < count; i++) {
    const uint8_t* p = data + i * bytesPer;
    float v;
    if (isFloat) {
      uint32_t u = rd32(p);
      std::memcpy(&v, &u, 4);
    } else if (bits == 16) {
      v = float(int16_t(rd16(p))) / 32768.0f;
    } else if (bits == 24) {
      int32_t s = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 |
                          uint32_t(p[2]) << 24) >> 8;
      v = float(s) / 8388608.0f;
    } else {
      v = float(double(int32_t(rd32(p))) / 2147483648.0);
    }
    out.samples[i] = v;
  }
  return "";
}

/** Write a mono WAV file. Returns empty string on success. */
inline std::string writeMono(const std::string& path, int sampleRate,
                             const std::vector<float>& samples, OutFormat fmt) {
  using namespace detail;

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return "cannot create " + path;

  const uint16_t bits = (fmt == OutFormat::Float32) ? 32 : 16;
  const uint16_t tag = (fmt == OutFormat::Float32) ? 3 : 1;
  const uint32_t dataBytes = uint32_t(samples.size() * (bits / 8));

  std::fwrite("RIFF", 1, 4, f);
  wr32(f, 36 + dataBytes);
  std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f);
  wr32(f, 16);
  wr16(f, tag);
  wr16(f, 1);
  wr32(f, uint32_t(sampleRate));
  wr32(f, uint32_t(sampleRate) * (bits / 8));
  wr16(f, bits / 8);
  wr16(f, bits);
  std::fwrite("data", 1, 4, f);
  wr32(f, dataBytes);

  for (float s : samples) {
    if (fmt == OutFormat::Float32) {
      uint32_t u;
      std::memcpy(&u, &s, 4);
      wr32(f, u);
    } else {
      float c = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
      long q = long(c * 32767.0f + (c >= 0 ? 0.5f : -0.5f));
      wr16(f, uint16_t(int16_t(q)));
    }
  }

  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok ? "" : "write failed: " + path;
}

}  // namespace wav
}  // namespace ainoiceguard

#endif  // AINOICEGUARD_WAV_IO_H
