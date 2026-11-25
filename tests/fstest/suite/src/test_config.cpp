// SPDX-License-Identifier: MIT

#include "fstest/suite/test_config.h"

#include <utility>

namespace fstest {
namespace {

TestConfig &MutableConfig() {
  static TestConfig config{};
  return config;
}

} // namespace

void ConfigureSuite(TestConfig config) { MutableConfig() = std::move(config); }

const TestConfig &GetSuiteConfig() { return MutableConfig(); }

void ResetSuiteConfig() { MutableConfig() = TestConfig{}; }

} // namespace fstest
