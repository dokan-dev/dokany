// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "json_util.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {
namespace {

TEST(JsonUtilTest, ParsesValidJsonPreservingKeyOrder) {
  const std::string input = R"({
    "z": 1,
    "a": true,
    "m": [null, -42, 3.5, "hello\n\"world\"", "\u00E9"],
    "nested": {
      "second": 2,
      "first": 1
    }
  })";

  nlohmann::ordered_json parsed;
  std::string err;
  ASSERT_TRUE(ParseJsonStrict(input, &parsed, &err)) << err;
  ASSERT_TRUE(parsed.is_object());

  // Verify insertion order is preserved by ordered_json.
  std::vector<std::string> keys;
  for (const auto &[k, _] : parsed.items()) {
    keys.push_back(k);
  }
  ASSERT_EQ(keys.size(), 4u);
  EXPECT_EQ(keys[0], "z");
  EXPECT_EQ(keys[1], "a");
  EXPECT_EQ(keys[2], "m");
  EXPECT_EQ(keys[3], "nested");

  EXPECT_EQ(parsed["z"].get<int>(), 1);
  EXPECT_TRUE(parsed["a"].get<bool>());
  EXPECT_EQ(parsed["m"].size(), 5u);
  EXPECT_TRUE(parsed["m"][0].is_null());
  EXPECT_EQ(parsed["m"][1].get<int>(), -42);
  EXPECT_EQ(parsed["m"][3].get<std::string>(), "hello\n\"world\"");
  EXPECT_EQ(parsed["m"][4].get<std::string>(), "\xC3\xA9");
}

TEST(JsonUtilTest, RejectsDuplicateKeysAtRoot) {
  const std::string input = R"({"a": 1, "b": 2, "a": 3})";
  nlohmann::ordered_json parsed;
  std::string err;
  EXPECT_FALSE(ParseJsonStrict(input, &parsed, &err));
  EXPECT_NE(err.find("Duplicate JSON key"), std::string::npos) << err;
  EXPECT_NE(err.find("\"a\""), std::string::npos) << err;
}

TEST(JsonUtilTest, RejectsDuplicateKeysInNestedObject) {
  const std::string input = R"({
    "targets": {
      "mirror_base": {
        "xfails": {
          "Suite.TestA": {"reason": "first"},
          "Suite.TestB": {"reason": "second"},
          "Suite.TestA": {"reason": "duplicate"}
        }
      }
    }
  })";

  nlohmann::ordered_json parsed;
  std::string err;
  EXPECT_FALSE(ParseJsonStrict(input, &parsed, &err));
  EXPECT_NE(err.find("Duplicate JSON key"), std::string::npos) << err;
  EXPECT_NE(err.find("Suite.TestA"), std::string::npos) << err;
  EXPECT_NE(err.find("/targets/mirror_base/xfails"), std::string::npos) << err;
}

TEST(JsonUtilTest, RejectsDuplicateKeysInsideArrayElementObject) {
  const std::string input = R"({
    "items": [
      {"id": 1},
      {"id": 2, "id": 3}
    ]
  })";

  nlohmann::ordered_json parsed;
  std::string err;
  EXPECT_FALSE(ParseJsonStrict(input, &parsed, &err));
  EXPECT_NE(err.find("Duplicate JSON key"), std::string::npos) << err;
  EXPECT_NE(err.find("/items[1]"), std::string::npos) << err;
}

TEST(JsonUtilTest, AllowsSameKeyInSiblingAndNestedScopes) {
  const std::string input = R"({
    "reason": "outer",
    "a": {"reason": "sibling1", "inner": {"reason": "nested"}},
    "b": {"reason": "sibling2"},
    "arr": [{"reason": "elem0"}, {"reason": "elem1"}]
  })";

  nlohmann::ordered_json parsed;
  std::string err;
  EXPECT_TRUE(ParseJsonStrict(input, &parsed, &err)) << err;
}

TEST(JsonUtilTest, RejectsMalformedJsonInputs) {
  const std::vector<std::string> bad_inputs = {
      "",
      "   ",
      "{",
      R"({"a": 1,})",
      R"({"a": [1, 2,]})",
      R"({"a": 1} trailing)",
      R"({/* comment */ "a": 1})",
      R"({"a": 'single'})",
      R"({"a": undefined})",
      R"({"unterminated": "abc)",
  };

  for (const std::string &bad : bad_inputs) {
    nlohmann::ordered_json parsed;
    std::string err;
    EXPECT_FALSE(ParseJsonStrict(bad, &parsed, &err))
        << "Expected failure for input: " << bad;
    EXPECT_FALSE(err.empty()) << "Expected non-empty error for input: " << bad;
  }
}

TEST(JsonUtilTest, DeterministicSerializationUsesLfAndTrailingNewline) {
  nlohmann::ordered_json obj = nlohmann::ordered_json::object();
  obj["schema_version"] = 1;
  obj["description"] = "demo";
  obj["empty_obj"] = nlohmann::ordered_json::object();
  obj["empty_arr"] = nlohmann::ordered_json::array();

  const std::string out1 = SerializeJsonDeterministic(obj);
  const std::string out2 = SerializeJsonDeterministic(obj);
  EXPECT_EQ(out1, out2);
  ASSERT_FALSE(out1.empty());
  EXPECT_EQ(out1.back(), '\n');
  EXPECT_EQ(out1.find('\r'), std::string::npos);

  const std::string expected = "{\n"
                               "  \"schema_version\": 1,\n"
                               "  \"description\": \"demo\",\n"
                               "  \"empty_obj\": {},\n"
                               "  \"empty_arr\": []\n"
                               "}\n";
  EXPECT_EQ(out1, expected);

  // Round-trip through strict parser.
  nlohmann::ordered_json roundtrip;
  std::string err;
  ASSERT_TRUE(ParseJsonStrict(out1, &roundtrip, &err)) << err;
  EXPECT_EQ(SerializeJsonDeterministic(roundtrip), out1);
}

} // namespace
} // namespace fstest::cli
