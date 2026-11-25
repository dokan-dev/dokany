// SPDX-License-Identifier: MIT

#include "fstest/suite/test_metadata.h"

namespace fstest {

const std::vector<DomainInfo> &GetAllDomains() {
  static const std::vector<DomainInfo> kDomains = {
      {"create_open", "CreateOpenTest"},
      {"delete", "DeleteTest"},
      {"dir_enum", "DirEnumTest"},
      {"rename_move", "RenameMoveTest"},
      {"data_io", "DataIoTest"},
      {"locks_oplocks", "LocksOplocksTest"},
      {"timestamps_attrs", "TimestampsAttrsTest"},
      {"streams", "StreamsTest"},
      {"security_acl", "SecurityAclTest"},
      {"links_reparse", "LinksReparseTest"},
      {"naming_paths", "NamingPathsTest"},
      {"info_queries", "InfoQueriesTest"},
  };
  return kDomains;
}

std::string SuiteNameToDomainId(std::string_view suite_name) {
  for (const auto &domain : GetAllDomains()) {
    if (domain.suite_name == suite_name) {
      return std::string(domain.domain_id);
    }
  }
  return std::string(suite_name);
}

bool IsKnownSuiteName(std::string_view suite_name) {
  for (const auto &domain : GetAllDomains()) {
    if (domain.suite_name == suite_name) {
      return true;
    }
  }
  return false;
}

} // namespace fstest
