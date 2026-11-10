#pragma once
#include "plugin.h"
#include <Windows.h>

using namespace plugin;

unsigned int GfxCoreAddress(unsigned int addr);
void InstallGfxCoreHook(void (*callback)());
void SetGfxCoreLibrary(HANDLE handle);
