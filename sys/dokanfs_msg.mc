;#ifndef _DOKANFS_MC_
;#define _DOKANFS_MC_
;
;//
;//  Status values are 32 bit values layed out as follows:
;//
;//   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
;//   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
;//  +---+-+-------------------------+-------------------------------+
;//  |Sev|C|       Facility          |               Code            |
;//  +---+-+-------------------------+-------------------------------+
;//
;//  where
;//
;//      Sev - is the severity code
;//
;//          00 - Success
;//          01 - Informational
;//          10 - Warning
;//          11 - Error
;//
;//      C - is the Customer code flag
;//
;//      Facility - is the facility code
;//
;//      Code - is the facility's status code
;//
;
MessageIdTypedef=NTSTATUS

SeverityNames=(Success=0x0:STATUS_SEVERITY_SUCCESS
               Informational=0x1:STATUS_SEVERITY_INFORMATIONAL
               Warning=0x2:STATUS_SEVERITY_WARNING
               Error=0x3:STATUS_SEVERITY_ERROR
              )

FacilityNames=(System=0x0
               RpcRuntime=0x2:FACILITY_RPC_RUNTIME
               RpcStubs=0x3:FACILITY_RPC_STUBS
               Io=0x4:FACILITY_IO_ERROR_CODE
               Dokanfs=0x6:FACILITY_DOKANFS_ERROR_CODE
              )

;// Typical drivers have a separate message defined here for every logging call
;// site. However, that approach would cause forced truncation when injecting
;// variable-length parameters into messages, and it would also tightly couple
;// us to the Event Viewer, which we are not sure we want to use long term. So
;// having a message ID per log level with no static content allows us to use
;// printf-style logging that's portable to other mechanisms.

LanguageNames = (English=0x409:MSG00409)

MessageId=0x0001 Facility=Dokanfs Severity=Informational SymbolicName=DOKANFS_INFO_MSG
Language=English
%2
.

MessageId=0x0002 Facility=Dokanfs Severity=Error SymbolicName=DOKANFS_ERROR_MSG
Language=English
%2
.

;#endif