#include <catch2/catch_test_macros.hpp>

#include "mac_resource_fork.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using evnova::rez::ParseResourceFork;
using evnova::rez::ParseResourceForkOrAppleDouble;
using evnova::rez::ResourceFork;

void PushBe16(std::vector<std::byte> &out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value >> 8));
  out.push_back(static_cast<std::byte>(value & 0xff));
}

void PushBe32(std::vector<std::byte> &out, std::uint32_t value) {
  out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
  out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
  out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
  out.push_back(static_cast<std::byte>(value & 0xff));
}

struct TestResource {
  std::uint32_t type = 0;
  std::uint16_t id = 0;
  std::string name;
  std::uint8_t attributes = 0;
  std::vector<std::byte> payload;
};

// Builds a minimal but structurally valid classic resource fork. Types are
// emitted as given (the map's type order is not sorted, matching real forks).
[[nodiscard]] std::vector<std::byte>
BuildResourceFork(const std::vector<TestResource> &resources) {
  constexpr std::size_t data_offset = 256;
  std::vector<std::byte> data;
  for (const auto &resource : resources) {
    PushBe32(data, static_cast<std::uint32_t>(resource.payload.size()));
    data.insert(data.end(), resource.payload.begin(), resource.payload.end());
  }

  std::vector<std::byte> names;
  std::vector<std::uint16_t> name_offsets(resources.size(), 0xffff);
  for (std::size_t i = 0; i < resources.size(); ++i) {
    if (resources[i].name.empty()) {
      continue;
    }
    name_offsets[i] = static_cast<std::uint16_t>(names.size());
    names.push_back(static_cast<std::byte>(resources[i].name.size()));
    for (const char c : resources[i].name) {
      names.push_back(static_cast<std::byte>(c));
    }
  }

  std::vector<std::byte> map(28, std::byte{0});
  map[25] = std::byte{28}; // type list at offset 28
  std::vector<std::uint32_t> seen;
  for (const auto &resource : resources) {
    if (std::find(seen.begin(), seen.end(), resource.type) == seen.end()) {
      seen.push_back(resource.type);
    }
  }
  const auto num_types = seen.size();
  PushBe16(map, static_cast<std::uint16_t>(num_types - 1));
  const auto ref_lists_offset =
      2 + num_types * 8; // first ref list, relative to type list
  std::size_t running_ref_list = 0;
  std::vector<std::uint32_t> emitted_types;
  for (const auto &resource : resources) {
    if (std::find(emitted_types.begin(), emitted_types.end(), resource.type) !=
        emitted_types.end()) {
      continue;
    }
    emitted_types.push_back(resource.type);
    std::size_t count = 0;
    for (const auto &candidate : resources) {
      if (candidate.type == resource.type) {
        ++count;
      }
    }
    PushBe32(map, resource.type);
    PushBe16(map, static_cast<std::uint16_t>(count - 1));
    PushBe16(map,
             static_cast<std::uint16_t>(ref_lists_offset + running_ref_list));
    running_ref_list += count * 12;
  }

  std::uint32_t data_pos = 0;
  std::vector<std::vector<std::byte>> ref_lists;
  for (const auto &type : emitted_types) {
    std::vector<std::byte> refs;
    for (std::size_t i = 0; i < resources.size(); ++i) {
      const auto &resource = resources[i];
      if (resource.type != type) {
        continue;
      }
      PushBe16(refs, resource.id);
      PushBe16(refs, name_offsets[i]);
      refs.push_back(static_cast<std::byte>(resource.attributes));
      refs.push_back(static_cast<std::byte>((data_pos >> 16) & 0xff));
      refs.push_back(static_cast<std::byte>((data_pos >> 8) & 0xff));
      refs.push_back(static_cast<std::byte>(data_pos & 0xff));
      refs.insert(refs.end(), 4, std::byte{0}); // handle placeholder
      data_pos += static_cast<std::uint32_t>(resource.payload.size()) + 4;
    }
    ref_lists.push_back(std::move(refs));
  }
  for (const auto &refs : ref_lists) {
    map.insert(map.end(), refs.begin(), refs.end());
  }
  const auto name_list_offset = map.size();
  map[26] = static_cast<std::byte>((name_list_offset >> 8) & 0xff);
  map[27] = static_cast<std::byte>(name_list_offset & 0xff);
  map.insert(map.end(), names.begin(), names.end());

  std::vector<std::byte> fork;
  PushBe32(fork, static_cast<std::uint32_t>(data_offset));
  PushBe32(fork, static_cast<std::uint32_t>(data_offset + data.size()));
  PushBe32(fork, static_cast<std::uint32_t>(data.size()));
  PushBe32(fork, static_cast<std::uint32_t>(map.size()));
  fork.resize(data_offset, std::byte{0});
  fork.insert(fork.end(), data.begin(), data.end());
  fork.insert(fork.end(), map.begin(), map.end());
  return fork;
}

[[nodiscard]] std::vector<std::byte>
AppleDoubleWrap(const std::vector<std::byte> &fork) {
  constexpr std::uint32_t entry_offset = 26 + 12;
  std::vector<std::byte> out;
  PushBe32(out, 0x00051607); // AppleDouble
  PushBe32(out, 0x00020000);
  out.insert(out.end(), 16, std::byte{0}); // filler
  PushBe16(out, 1);                        // one entry
  PushBe32(out, 2);                        // resource-fork entry id
  PushBe32(out, entry_offset);
  PushBe32(out, static_cast<std::uint32_t>(fork.size()));
  out.insert(out.end(), fork.begin(), fork.end());
  return out;
}

const std::vector<TestResource> kSample = {
    {.type = 0x73689570,
     .id = 128,
     .name = "Alpha",
     .attributes = 0,
     .payload = {std::byte{1}, std::byte{2}, std::byte{3}}},
    {.type = 0x73689570, .id = 129, .name = "", .attributes = 0, .payload = {}},
    {.type = 0x6f9f7466,
     .id = 157,
     .name = "Beta",
     .attributes = evnova::rez::kResourceAttributeCompressed,
     .payload = {std::byte{0xaa}, std::byte{0xbb}}},
};

} // namespace

TEST_CASE("ParseResourceFork reads types, ids, names and payloads") {
  const auto fork = ParseResourceFork(BuildResourceFork(kSample));
  REQUIRE(fork.has_value());
  REQUIRE(fork->resources.size() == 3);

  const auto &alpha = fork->resources[0];
  REQUIRE(alpha.type_code == 0x73689570);
  REQUIRE(alpha.resource_id == 128);
  REQUIRE(alpha.name == "Alpha");
  REQUIRE(alpha.size == 3);
  REQUIRE(std::to_integer<int>(fork->bytes[alpha.offset]) == 1);
  REQUIRE(std::to_integer<int>(fork->bytes[alpha.offset + 2]) == 3);

  const auto &unnamed = fork->resources[1];
  REQUIRE(unnamed.resource_id == 129);
  REQUIRE(unnamed.name.empty());
  REQUIRE(unnamed.size == 0);

  const auto &compressed = fork->resources[2];
  REQUIRE(compressed.type_code == 0x6f9f7466);
  REQUIRE(compressed.resource_id == 157);
  REQUIRE(compressed.name == "Beta");
  REQUIRE((compressed.attributes & evnova::rez::kResourceAttributeCompressed) !=
          0);
}

TEST_CASE("ParseResourceFork rejects malformed headers") {
  const std::vector<std::byte> empty;
  REQUIRE_FALSE(ParseResourceFork(empty).has_value());
  std::vector<std::byte> short_header(32, std::byte{0});
  REQUIRE_FALSE(ParseResourceFork(short_header).has_value());

  auto fork = BuildResourceFork(kSample);
  // Zero the data offset: no longer a plausible fork.
  fork[0] = fork[1] = fork[2] = fork[3] = std::byte{0};
  REQUIRE_FALSE(ParseResourceFork(fork).has_value());

  // Move the map past EOF.
  auto truncated = BuildResourceFork(kSample);
  truncated.resize(truncated.size() - 4);
  REQUIRE_FALSE(ParseResourceFork(truncated).has_value());
}

TEST_CASE("ParseResourceForkOrAppleDouble unwraps AppleDouble images") {
  const auto fork = BuildResourceFork(kSample);
  const auto wrapped = AppleDoubleWrap(fork);
  const auto parsed = ParseResourceForkOrAppleDouble(wrapped);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->resources.size() == 3);
  REQUIRE(parsed->resources[2].name == "Beta");

  // A plain fork passes straight through.
  const auto direct = ParseResourceForkOrAppleDouble(fork);
  REQUIRE(direct.has_value());
  REQUIRE(direct->resources.size() == 3);

  // An unrelated blob is rejected.
  const std::vector<std::byte> junk(64, std::byte{0x5a});
  REQUIRE_FALSE(ParseResourceForkOrAppleDouble(junk).has_value());
}

TEST_CASE("container pre-filters reject non-archives") {
  const auto fork = BuildResourceFork(kSample);
  REQUIRE(evnova::rez::IsResourceForkHeader(fork, fork.size()));
  const std::vector<std::byte> junk(64, std::byte{0x5a});
  REQUIRE_FALSE(evnova::rez::IsResourceForkHeader(junk, junk.size()));
  REQUIRE_FALSE(evnova::rez::IsResourceForkHeader({}, 0));
  // A header that claims a map past EOF must not pass the sniff.
  REQUIRE_FALSE(evnova::rez::IsResourceForkHeader(fork, 64));

  const auto wrapped = AppleDoubleWrap(fork);
  REQUIRE(evnova::rez::IsAppleSingleOrDoubleHeader(wrapped));
  REQUIRE_FALSE(evnova::rez::IsAppleSingleOrDoubleHeader(fork));
  REQUIRE_FALSE(evnova::rez::IsAppleSingleOrDoubleHeader(junk));
}

TEST_CASE("LoadResourceFork reads an AppleDouble sidecar") {
  const auto directory =
      std::filesystem::temp_directory_path() / "evnova_resource_fork_test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto data_file = directory / "Plugin Name";
  {
    std::ofstream empty{data_file, std::ios::binary};
  }
  const auto sidecar = directory / "._Plugin Name";
  {
    const auto wrapped = AppleDoubleWrap(BuildResourceFork(kSample));
    std::ofstream out{sidecar, std::ios::binary};
    out.write(reinterpret_cast<const char *>(wrapped.data()),
              static_cast<std::streamsize>(wrapped.size()));
  }

  const auto fork = evnova::rez::LoadResourceFork(data_file);
  REQUIRE(fork.has_value());
  REQUIRE(fork->resources.size() == 3);
  REQUIRE(fork->resources[0].name == "Alpha");

  std::filesystem::remove_all(directory);
}
