// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "json_util.h"

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fstest::cli {
namespace {

class DuplicateKeySaxChecker : public nlohmann::ordered_json::json_sax_t {
public:
  bool null() override {
    OnValueComplete();
    return true;
  }

  bool boolean(bool /*val*/) override {
    OnValueComplete();
    return true;
  }

  bool number_integer(number_integer_t /*val*/) override {
    OnValueComplete();
    return true;
  }

  bool number_unsigned(number_unsigned_t /*val*/) override {
    OnValueComplete();
    return true;
  }

  bool number_float(number_float_t /*val*/, const string_t & /*s*/) override {
    OnValueComplete();
    return true;
  }

  bool string(string_t & /*val*/) override {
    OnValueComplete();
    return true;
  }

  bool binary(binary_t & /*val*/) override {
    OnValueComplete();
    return true;
  }

  bool start_object(std::size_t /*elements*/) override {
    Frame frame;
    frame.is_object = true;
    frame.path = NextChildPath();
    frames_.push_back(std::move(frame));
    return true;
  }

  bool key(string_t &val) override {
    if (frames_.empty() || !frames_.back().is_object) {
      error_ = "Unexpected JSON key outside object context";
      return false;
    }
    Frame &top = frames_.back();
    if (!top.keys.insert(val).second) {
      error_ = "Duplicate JSON key \"" + val + "\" in object at \"" + top.path +
               "\"";
      return false;
    }
    top.current_key = val;
    return true;
  }

  bool end_object() override {
    if (!frames_.empty()) {
      frames_.pop_back();
    }
    OnValueComplete();
    return true;
  }

  bool start_array(std::size_t /*elements*/) override {
    Frame frame;
    frame.is_object = false;
    frame.path = NextChildPath();
    frames_.push_back(std::move(frame));
    return true;
  }

  bool end_array() override {
    if (!frames_.empty()) {
      frames_.pop_back();
    }
    OnValueComplete();
    return true;
  }

  bool parse_error(std::size_t /*position*/, const std::string & /*last_token*/,
                   const nlohmann::ordered_json::exception &ex) override {
    if (error_.empty()) {
      error_ = ex.what();
    }
    return false;
  }

  const std::string &error() const { return error_; }

private:
  struct Frame {
    bool is_object = true;
    std::string path;
    std::string current_key;
    std::size_t array_index = 0;
    std::unordered_set<std::string> keys;
  };

  std::string NextChildPath() const {
    if (frames_.empty()) {
      return "/";
    }
    const Frame &parent = frames_.back();
    const std::string base = (parent.path == "/") ? std::string{} : parent.path;
    if (parent.is_object) {
      return base + "/" + parent.current_key;
    }
    return base + "[" + std::to_string(parent.array_index) + "]";
  }

  void OnValueComplete() {
    if (!frames_.empty() && !frames_.back().is_object) {
      ++frames_.back().array_index;
    }
  }

  std::vector<Frame> frames_;
  std::string error_;
};

} // namespace

bool ParseJsonStrict(std::string_view text, nlohmann::ordered_json *out,
                     std::string *error_out) {
  if (out == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Internal error: null output parameter";
    }
    return false;
  }

  DuplicateKeySaxChecker checker;
  const bool sax_ok = nlohmann::ordered_json::sax_parse(text, &checker);
  if (!sax_ok) {
    if (error_out != nullptr) {
      *error_out =
          checker.error().empty() ? "Malformed JSON input" : checker.error();
    }
    return false;
  }

  try {
    *out = nlohmann::ordered_json::parse(text);
    return true;
  } catch (const std::exception &ex) {
    if (error_out != nullptr) {
      *error_out = ex.what();
    }
    return false;
  }
}

std::string SerializeJsonDeterministic(
    const nlohmann::ordered_json &value,
    nlohmann::ordered_json::error_handler_t error_handler) {
  std::string dumped = value.dump(
      /*indent=*/2, /*indent_char=*/' ', /*ensure_ascii=*/false, error_handler);
  dumped.push_back('\n');
  return dumped;
}

} // namespace fstest::cli
