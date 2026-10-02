#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../tools/microexec_evnova/host/reflect.hpp"

namespace {

template <typename P> void CheckStringParameter(std::string text) {
  auto state = std::make_unique<game::GameState>();
  microexec::Args args;
  args["text"].bytes = text;
  std::vector<std::string_view> used;
  microexec::Param<P> original(*state, args, "text", used);
  // Match Run's movement into its parameter tuple. Views must refer to the
  // final owner's storage, including strings using the short-string buffer.
  auto params = std::tuple{std::move(original)};
  CHECK(std::string_view(std::get<0>(params).Get()) == text);
  CHECK(used == std::vector<std::string_view>{"text"});
}

} // namespace

TEST_CASE("Microexec string parameters retain their bytes after movement",
          "[microexec]") {
  for (const auto &text : {std::string{},
                           std::string("short"),
                           std::string(512, 'x'),
                           std::string("a\0\xff\n", 4)}) {
    CheckStringParameter<std::string>(text);
    CheckStringParameter<const std::string &>(text);
    CheckStringParameter<std::string_view>(text);
    CheckStringParameter<const std::string_view &>(text);
  }
  CHECK(microexec::KindOf<std::string>() == "string");
  CHECK(microexec::KindOf<std::string_view>() == "string");
  CHECK(microexec::ParamProblem<std::string &>() == "string out-parameter");
  CHECK(microexec::ParamProblem<std::string_view &>() ==
        "string out-parameter");
}

TEST_CASE("Microexec byte arguments cannot become numeric fields",
          "[microexec]") {
  microexec::Value bytes;
  bytes.bytes = "1";
  CHECK_THROWS_AS(microexec::Convert<bool>(bytes), microexec::CaseError);
  CHECK_THROWS_AS(microexec::Convert<int>(bytes), microexec::CaseError);
  CHECK_THROWS_AS(microexec::Convert<double>(bytes), microexec::CaseError);
}
