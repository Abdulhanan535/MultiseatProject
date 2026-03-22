#pragma once
#include <windows.h>

// Patch termsrv.dll in memory to allow multiple concurrent local sessions.
// Must be called with SeDebugPrivilege (i.e. as SYSTEM or elevated admin).
BOOL TermsrvPatch_Apply(void);

// Restart TermService to revert all in-memory patches.
BOOL TermsrvPatch_Revert(void);
