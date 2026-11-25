// SPDX-License-Identifier: MIT

#include "fstest/suite/register_suites.h"

namespace fstest {

void LinkAllSuites() {
  ForceLink_CreateOpen();
  ForceLink_CreateOpenSecurity();
  ForceLink_Delete();
  ForceLink_DirEnum();
  ForceLink_RenameMove();
  ForceLink_DataIo();
  ForceLink_DataIoSparseMmap();
  ForceLink_LocksOplocks();
  ForceLink_TimestampsAttrs();
  ForceLink_Streams();
  ForceLink_StreamsOps();
  ForceLink_SecurityAcl();
  ForceLink_LinksReparse();
  ForceLink_NamingPaths();
  ForceLink_InfoQueries();
  ForceLink_InfoQueriesNotifyEa();
}

} // namespace fstest
