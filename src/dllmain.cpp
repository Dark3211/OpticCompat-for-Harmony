#include "common.hpp"

HMODULE g_opticcompat_module = nullptr;

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if(reason == DLL_PROCESS_ATTACH) {
        g_opticcompat_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
