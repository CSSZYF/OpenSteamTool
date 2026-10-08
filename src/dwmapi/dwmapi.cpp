// dwmapi.dll HiJack Project - Standardized Proxy Loader
#include <windows.h>
#include "../Common/ProxyBootstrap.hpp"

// Forwarded exports definition
#include "dwmapi.def"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
