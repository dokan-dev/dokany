// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_env.h"

#include <iostream>
#include <string>

#include <gtest/gtest.h>

int main(int argc, char **argv) {
  std::string err;
  if (!dokan_np_integration::ParseCliFlags(&argc, argv, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 2;
  }

  if (dokan_np_integration::GetTestConfig().probe_mode) {
    return dokan_np_integration::RunProbeMode();
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
