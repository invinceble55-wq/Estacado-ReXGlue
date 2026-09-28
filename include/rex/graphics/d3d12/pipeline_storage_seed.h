/**
 * @file graphics/d3d12/pipeline_storage_seed.h
 * @brief Imports a packaged shader/pipeline storage seed into the persistent
 *        D3D12 storage before PipelineCache loads it.
 *
 * The persistent storage ("shaders/shareable") holds only guest shader
 * microcode, content-addressed by XXH3, and pipeline descriptions, each
 * guarded by an XXH3 of its bytes. At load every shader is retranslated and
 * every pipeline recreated by the current code, so the records carry no
 * build-specific artifacts; format changes are rejected by the file headers
 * (magic, API and version). A seed record is appended only if it validates
 * exactly as the loader validates it and the store does not already hold it.
 * Nothing is removed from a store: a store whose header no longer matches is
 * replaced, which is what the loader itself does with such a file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/hash.h>

namespace rex::graphics::d3d12::pipeline_storage_seed {

// On-disk constants mirrored from PipelineCache; pipeline_cache.cpp
// static_asserts that the sizes and versions match its private definitions.
inline constexpr uint32_t kShaderMagic = 0x48534558;     // 'XESH'
inline constexpr uint32_t kPipelineMagic = 0x53504558;   // 'XEPS'
inline constexpr uint32_t kPipelineApiRtv = 0x54525844;  // 'DXRT'
inline constexpr uint32_t kPipelineApiRov = 0x4F525844;  // 'DXRO'
inline constexpr size_t kShaderRecordHeaderSize = 12;
inline constexpr size_t kPipelineRecordSize = 72;

inline constexpr uint32_t ByteSwap32(uint32_t value) {
  return (value >> 24) | ((value >> 8) & 0x0000FF00u) |
         ((value << 8) & 0x00FF0000u) | (value << 24);
}

struct Record {
  size_t offset = 0;
  size_t size = 0;
  uint64_t hash = 0;
};

inline void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
  uint8_t bytes[4];
  std::memcpy(bytes, &value, sizeof(bytes));
  out.insert(out.end(), bytes, bytes + sizeof(bytes));
}

inline std::vector<uint8_t> ShaderHeader(uint32_t version) {
  std::vector<uint8_t> header;
  AppendU32(header, kShaderMagic);
  AppendU32(header, ByteSwap32(version));
  return header;
}

inline std::vector<uint8_t> PipelineHeader(uint32_t api, uint32_t version) {
  std::vector<uint8_t> header;
  AppendU32(header, kPipelineMagic);
  AppendU32(header, api);
  AppendU32(header, ByteSwap32(version));
  return header;
}

// Returns the byte length of the valid prefix (header plus every record up to
// the first one the loader would reject), or 0 if the header does not match.
inline size_t ParseShaders(const std::vector<uint8_t>& data, uint32_t version,
                           std::vector<Record>& records) {
  records.clear();
  const std::vector<uint8_t> header = ShaderHeader(version);
  if (data.size() < header.size() ||
      std::memcmp(data.data(), header.data(), header.size())) {
    return 0;
  }
  size_t offset = header.size();
  while (data.size() - offset >= kShaderRecordHeaderSize) {
    uint64_t hash;
    uint32_t bits;
    std::memcpy(&hash, data.data() + offset, sizeof(hash));
    std::memcpy(&bits, data.data() + offset + sizeof(hash), sizeof(bits));
    const size_t ucode_bytes = size_t(bits & 0x7FFFFFFFu) * sizeof(uint32_t);
    if (data.size() - offset - kShaderRecordHeaderSize < ucode_bytes ||
        XXH3_64bits(data.data() + offset + kShaderRecordHeaderSize,
                    ucode_bytes) != hash) {
      break;
    }
    records.push_back({offset, kShaderRecordHeaderSize + ucode_bytes, hash});
    offset += kShaderRecordHeaderSize + ucode_bytes;
  }
  return offset;
}

inline size_t ParsePipelines(const std::vector<uint8_t>& data, uint32_t api,
                             uint32_t version, std::vector<Record>& records) {
  records.clear();
  const std::vector<uint8_t> header = PipelineHeader(api, version);
  if (data.size() < header.size() ||
      std::memcmp(data.data(), header.data(), header.size())) {
    return 0;
  }
  size_t offset = header.size();
  while (data.size() - offset >= kPipelineRecordSize) {
    uint64_t hash;
    std::memcpy(&hash, data.data() + offset, sizeof(hash));
    if (XXH3_64bits(data.data() + offset + sizeof(hash),
                    kPipelineRecordSize - sizeof(hash)) != hash) {
      break;
    }
    records.push_back({offset, kPipelineRecordSize, hash});
    offset += kPipelineRecordSize;
  }
  return offset;
}

inline bool ReadWholeFile(const std::filesystem::path& path,
                          std::vector<uint8_t>& out) {
  out.clear();
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return false;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  out.assign(std::istreambuf_iterator<char>(stream),
             std::istreambuf_iterator<char>());
  return !stream.bad();
}

// Replaces the file through a sibling temporary so an interrupted write never
// leaves a partially written store behind.
inline bool WriteWholeFile(const std::filesystem::path& path,
                           const std::vector<uint8_t>& data) {
  std::filesystem::path temporary = path;
  temporary += ".seed-tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(data.data()),
                 std::streamsize(data.size()));
    stream.flush();
    if (!stream) return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

struct FileResult {
  bool seed_valid = false;    // seed present with a matching header
  size_t seed_records = 0;    // valid records in the seed
  size_t store_records = 0;   // valid records already in the store
  size_t added = 0;           // seed records appended to the store
  bool write_failed = false;
};

// Shaders are keyed by their microcode hash (the loader's own key); pipeline
// descriptions by their full bytes.
template <typename Parse>
FileResult MergeFile(const std::filesystem::path& seed_path,
                     const std::filesystem::path& store_path,
                     const std::vector<uint8_t>& header, Parse parse,
                     bool key_by_hash) {
  FileResult result;
  std::vector<uint8_t> seed;
  std::vector<Record> seed_records;
  if (!ReadWholeFile(seed_path, seed) || !parse(seed, seed_records)) {
    return result;
  }
  result.seed_valid = true;
  result.seed_records = seed_records.size();

  std::vector<uint8_t> store;
  std::vector<Record> store_records;
  const size_t store_valid =
      ReadWholeFile(store_path, store) ? parse(store, store_records) : 0;
  result.store_records = store_records.size();

  const auto key_of = [key_by_hash](const std::vector<uint8_t>& data,
                                    const Record& record) {
    if (key_by_hash) {
      return std::string(reinterpret_cast<const char*>(&record.hash),
                         sizeof(record.hash));
    }
    return std::string(reinterpret_cast<const char*>(data.data() + record.offset),
                       record.size);
  };
  std::unordered_set<std::string> present;
  for (const Record& record : store_records) present.insert(key_of(store, record));

  std::vector<uint8_t> merged;
  if (store_valid) {
    merged.assign(store.begin(), store.begin() + ptrdiff_t(store_valid));
  } else {
    merged = header;
  }
  for (const Record& record : seed_records) {
    if (!present.insert(key_of(seed, record)).second) continue;
    merged.insert(merged.end(), seed.begin() + ptrdiff_t(record.offset),
                  seed.begin() + ptrdiff_t(record.offset + record.size));
    ++result.added;
  }
  if (result.added && !WriteWholeFile(store_path, merged)) {
    result.write_failed = true;
    result.added = 0;
  }
  return result;
}

inline FileResult MergeShaders(const std::filesystem::path& seed_path,
                               const std::filesystem::path& store_path,
                               uint32_t version) {
  return MergeFile(
      seed_path, store_path, ShaderHeader(version),
      [version](const std::vector<uint8_t>& data, std::vector<Record>& records) {
        return ParseShaders(data, version, records);
      },
      true);
}

inline FileResult MergePipelines(const std::filesystem::path& seed_path,
                                 const std::filesystem::path& store_path,
                                 uint32_t api, uint32_t version) {
  return MergeFile(
      seed_path, store_path, PipelineHeader(api, version),
      [api, version](const std::vector<uint8_t>& data,
                     std::vector<Record>& records) {
        return ParsePipelines(data, api, version, records);
      },
      false);
}

// Shader index (<title>.xshi): which guest shaders the pipeline descriptions
// need, without their microcode, so a package can carry it. Per shader: the
// microcode hash (the storage key), its dword count and type, and the XXH3 of
// its first kIndexPrefixBytes bytes, which lets the pipeline cache recognise
// the shader in guest memory as soon as the game has created it, before its
// first draw (PipelineCache prewarm). Little-endian, like the other stores.
inline constexpr uint32_t kShaderIndexMagic = 0x49534558;  // 'XESI'
inline constexpr size_t kShaderIndexRecordSize = 24;
inline constexpr size_t kIndexPrefixBytes = 32;

struct ShaderIndexEntry {
  uint64_t ucode_hash = 0;
  uint32_t dword_count = 0;
  uint32_t type = 0;  // xenos::ShaderType: 0 vertex, 1 pixel
  uint64_t prefix_hash = 0;
};

inline uint64_t ShaderIndexPrefixHash(const void* ucode, size_t ucode_bytes) {
  return XXH3_64bits(ucode, ucode_bytes < kIndexPrefixBytes ? ucode_bytes : kIndexPrefixBytes);
}

// The index of every shader in a shader store (.xsh) with at least
// kIndexPrefixBytes of microcode (shorter ones are too ambiguous to find).
inline std::vector<uint8_t> BuildShaderIndex(const std::vector<uint8_t>& shader_store,
                                             uint32_t shader_version) {
  std::vector<Record> records;
  if (!ParseShaders(shader_store, shader_version, records)) return {};
  std::vector<uint8_t> out;
  AppendU32(out, kShaderIndexMagic);
  AppendU32(out, ByteSwap32(shader_version));
  for (const Record& record : records) {
    uint32_t bits;
    std::memcpy(&bits, shader_store.data() + record.offset + 8, sizeof(bits));
    const size_t ucode_bytes = size_t(bits & 0x7FFFFFFFu) * sizeof(uint32_t);
    if (ucode_bytes < kIndexPrefixBytes) continue;
    const uint8_t* ucode = shader_store.data() + record.offset + kShaderRecordHeaderSize;
    const uint64_t prefix = ShaderIndexPrefixHash(ucode, ucode_bytes);
    const size_t at = out.size();
    out.resize(at + kShaderIndexRecordSize);
    std::memcpy(out.data() + at, &record.hash, 8);
    const uint32_t count = bits & 0x7FFFFFFFu;
    const uint32_t type = bits >> 31;
    std::memcpy(out.data() + at + 8, &count, 4);
    std::memcpy(out.data() + at + 12, &type, 4);
    std::memcpy(out.data() + at + 16, &prefix, 8);
  }
  return out;
}

inline bool ParseShaderIndex(const std::vector<uint8_t>& data, uint32_t shader_version,
                             std::vector<ShaderIndexEntry>& entries) {
  entries.clear();
  if (data.size() < 8) return false;
  uint32_t magic, version;
  std::memcpy(&magic, data.data(), 4);
  std::memcpy(&version, data.data() + 4, 4);
  if (magic != kShaderIndexMagic || ByteSwap32(version) != shader_version) return false;
  for (size_t at = 8; at + kShaderIndexRecordSize <= data.size(); at += kShaderIndexRecordSize) {
    ShaderIndexEntry entry;
    std::memcpy(&entry.ucode_hash, data.data() + at, 8);
    std::memcpy(&entry.dword_count, data.data() + at + 8, 4);
    std::memcpy(&entry.type, data.data() + at + 12, 4);
    std::memcpy(&entry.prefix_hash, data.data() + at + 16, 8);
    if (entry.dword_count * sizeof(uint32_t) < kIndexPrefixBytes || entry.type > 1 ||
        entry.dword_count > 0xFFFFu) {
      continue;
    }
    entries.push_back(entry);
  }
  return true;
}

// Shader rebuild list (<title>.xshp, scripts/program-cache-patches.py): the
// title keeps its compiled shaders in System/Xenon/ProgramCache.xpc, and the
// console's Direct3D patches the vertex-fetch instructions of each vertex
// shader for the vertex layout it is bound with. Per shader the game draws
// with: the microcode hash of its ProgramCache source and the changed dwords
// as XOR masks (vertex-fetch formats, strides, offsets and slots), so the
// player's own ProgramCache rebuilds it before its first use. Carries no
// microcode; a rebuilt shader is used only if its hash matches.
// Little-endian: 'XESP', shader version (byte-swapped), entry count, then per
// entry: runtime hash u64, source hash u64, dword count u32, type u32, patch
// count u32 and that many (dword index u32, xor u32).
inline constexpr uint32_t kShaderPatchMagic = 0x50534558;  // 'XESP'

struct ShaderPatchEntry {
  uint64_t runtime_hash = 0;
  uint64_t source_hash = 0;
  uint32_t dword_count = 0;
  uint32_t type = 0;  // xenos::ShaderType: 0 vertex, 1 pixel
  std::vector<std::pair<uint32_t, uint32_t>> patches;  // dword index, xor
};

inline bool ParseShaderPatches(const std::vector<uint8_t>& data, uint32_t shader_version,
                               std::vector<ShaderPatchEntry>& entries) {
  entries.clear();
  if (data.size() < 12) return false;
  uint32_t magic, version, count;
  std::memcpy(&magic, data.data(), 4);
  std::memcpy(&version, data.data() + 4, 4);
  std::memcpy(&count, data.data() + 8, 4);
  if (magic != kShaderPatchMagic || ByteSwap32(version) != shader_version) return false;
  size_t at = 12;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < 28) return false;
    ShaderPatchEntry entry;
    uint32_t patch_count;
    std::memcpy(&entry.runtime_hash, data.data() + at, 8);
    std::memcpy(&entry.source_hash, data.data() + at + 8, 8);
    std::memcpy(&entry.dword_count, data.data() + at + 16, 4);
    std::memcpy(&entry.type, data.data() + at + 20, 4);
    std::memcpy(&patch_count, data.data() + at + 24, 4);
    at += 28;
    if (entry.type > 1 || !entry.dword_count || entry.dword_count > 0xFFFFu ||
        patch_count > entry.dword_count || (data.size() - at) / 8 < patch_count) {
      return false;
    }
    entry.patches.resize(patch_count);
    for (auto& patch : entry.patches) {
      std::memcpy(&patch.first, data.data() + at, 4);
      std::memcpy(&patch.second, data.data() + at + 4, 4);
      at += 8;
      if (patch.first >= entry.dword_count) return false;
    }
    entries.push_back(std::move(entry));
  }
  return at == data.size();
}

// The shader containers of a title's ProgramCache.xpc: big-endian headers
// with flags 0x102A11xx (bit 0 = vertex), virtual and physical sizes, and the
// microcode at virtual size + its physical offset (as in
// tools/xenos_shader_catalog.cpp). Returns microcode spans within data.
struct ProgramCacheShader {
  size_t offset = 0;  // of the microcode in data
  uint32_t bytes = 0;
  uint32_t type = 0;  // 0 vertex, 1 pixel
};

inline uint32_t ReadBigEndian32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
         (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]);
}

inline std::vector<ProgramCacheShader> ParseProgramCache(const std::vector<uint8_t>& data) {
  std::vector<ProgramCacheShader> shaders;
  for (size_t offset = 0; offset + 36 <= data.size(); ++offset) {
    const uint32_t flags = ReadBigEndian32(data, offset);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u) continue;
    const uint32_t virtual_size = ReadBigEndian32(data, offset + 4);
    const uint32_t physical_size = ReadBigEndian32(data, offset + 8);
    const uint32_t constant_table_offset = ReadBigEndian32(data, offset + 16);
    const uint32_t shader_offset = ReadBigEndian32(data, offset + 24);
    const uint64_t container_size = uint64_t(virtual_size) + physical_size;
    if (container_size < 36 || container_size > data.size() - offset ||
        ReadBigEndian32(data, offset + 28) || ReadBigEndian32(data, offset + 32) ||
        !constant_table_offset || constant_table_offset >= virtual_size || !shader_offset ||
        uint64_t(shader_offset) + 24 > virtual_size) {
      continue;
    }
    const uint32_t ucode_physical_offset = ReadBigEndian32(data, offset + shader_offset);
    const uint32_t ucode_bytes = ReadBigEndian32(data, offset + shader_offset + 4);
    const uint64_t ucode_relative = uint64_t(virtual_size) + ucode_physical_offset;
    if ((ucode_bytes & 3u) || !ucode_bytes || ucode_relative > container_size ||
        ucode_bytes > container_size - ucode_relative) {
      continue;
    }
    shaders.push_back({offset + size_t(ucode_relative), ucode_bytes, (flags & 1u) ? 0u : 1u});
  }
  return shaders;
}

}  // namespace rex::graphics::d3d12::pipeline_storage_seed
