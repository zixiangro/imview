#pragma once

// Shared between the ImView executable (which registers the handler) and the
// Explorer context-menu DLL. Changing this GUID orphans existing registration.
#include <guiddef.h>

inline constexpr CLSID kImViewContextMenuClsid =
    {0xa7f1e2c4, 0x5b3d, 0x4e6a,
     {0x9c, 0x2f, 0x1d, 0x8b, 0x7a, 0x6e, 0x5f, 0x40}};
