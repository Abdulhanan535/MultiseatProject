#pragma once
#include <windows.h>

void   DllInjector_Init(LPCWSTR dllPath);
ULONG  DllInjector_InjectSession(ULONG sessionId);
HANDLE DllInjector_WatchSession(ULONG sessionId);
BOOL   DllInjector_SetupIFEO(LPCWSTR exeName);
BOOL   DllInjector_RemoveIFEO(LPCWSTR exeName);
