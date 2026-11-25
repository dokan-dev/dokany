// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace fstest::cli {

// Strictly parses RFC 8259 JSON into an ordered_json DOM.
// Rejects malformed JSON, trailing tokens, comments, trailing commas, and
// duplicate object keys anywhere in the document tree.
bool ParseJsonStrict(std::string_view text, nlohmann::ordered_json *out,
                     std::string *error_out);

// Serializes an ordered_json value deterministically with 2-space indentation,
// LF line endings, and a single trailing newline.
std::string SerializeJsonDeterministic(
    const nlohmann::ordered_json &value,
    nlohmann::ordered_json::error_handler_t error_handler =
        nlohmann::ordered_json::error_handler_t::strict);

} // namespace fstest::cli
