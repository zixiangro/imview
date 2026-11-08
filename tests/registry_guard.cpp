// Regression tests for the ImView-owned HKCU path allowlist. The helpers live
// in shell_actions.cpp's anonymous namespace, so this test includes the source
// directly and compiles it with the Windows libraries it uses.

#include "shell_actions.cpp"

#include <cstdio>

namespace imview::shell {
namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL %s\n", what);
    ++gFailures;
}

} // namespace

int runRegistryGuardTests() {
    check(isSafeRegistryWritePath(HKEY_CURRENT_USER,
                                  L"Software\\Classes\\ImView.Image"),
          "image ProgID write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\ImView.Image\\DefaultIcon"),
          "image ProgID subkey write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\ImView.Video\\shell\\open\\command"),
          "video ProgID command write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\ImView\\Capabilities\\FileAssociations"),
          "capabilities write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\Applications\\imview.exe"),
          "legacy applications key write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\CLSID\\{A7F1E2C4-5B3D-4E6A-9C2F-1D8B7A6E5F40}\\InprocServer32"),
          "CLSID write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\SystemFileAssociations\\image\\shellex\\ContextMenuHandlers\\ImView"),
          "image handler write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\SystemFileAssociations\\video\\shellex\\ContextMenuHandlers\\ImView"),
          "video handler write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\ImView"),
          "directory handler write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\ImView"),
          "legacy handler cleanup write");
    check(isSafeRegistryWritePath(HKEY_CURRENT_USER,
                                  L"Software\\RegisteredApplications"),
          "registered applications value write");
    check(isSafeRegistryWritePath(HKEY_CURRENT_USER,
                                  L"Software\\Classes\\.jpg\\OpenWithProgids"),
          "image OpenWithProgids value write");
    check(isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\Applications\\imview.exe\\SupportedTypes"),
          "supported types write");
    check(isImViewProgId(L"ImView.Image"), "image ProgID value name");
    check(isImViewProgId(L"ImView.Video"), "video ProgID value name");
    check(!isImViewProgId(L"Other.Image"), "foreign ProgID value name rejected");

    check(!isSafeRegistryWritePath(HKEY_CURRENT_USER,
                                   L"Software\\Classes\\ImView.Evil"),
          "unrelated ProgID rejected");
    check(!isSafeRegistryWritePath(HKEY_CURRENT_USER,
                                   L"Software\\Classes\\ImView.ImageX"),
          "ProgID prefix collision rejected");
    check(!isSafeRegistryWritePath(HKEY_CURRENT_USER, L"Software"),
          "software root rejected");
    check(!isSafeRegistryWritePath(HKEY_CURRENT_USER, L"Software\\Classes"),
          "classes root rejected");
    check(!isSafeRegistryWritePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\ImView.Image\\..\\Evil"),
          "path traversal rejected");
    check(!isSafeRegistryWritePath(HKEY_LOCAL_MACHINE,
                                   L"Software\\Classes\\ImView.Image"),
          "non-HKCU rejected");

    check(isSafeRegistryDeleteKeyPath(HKEY_CURRENT_USER,
                                      L"Software\\Classes\\ImView.Image"),
          "ProgID key delete");
    check(isSafeRegistryDeleteKeyPath(HKEY_CURRENT_USER,
                                      L"Software\\ImView\\Capabilities"),
          "capabilities key delete");
    check(!isSafeRegistryDeleteKeyPath(HKEY_CURRENT_USER,
                                       L"Software\\RegisteredApplications"),
          "shared registered applications key not deleteable");
    check(!isSafeRegistryDeleteKeyPath(HKEY_CURRENT_USER,
                                       L"Software\\Classes\\.jpg"),
          "extension class key not deleteable");
    check(!isSafeRegistryDeleteKeyPath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\.jpg\\OpenWithProgids"),
          "OpenWithProgids key not deleteable");

    check(isSafeRegistryDeleteValuePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\.jpg\\OpenWithProgids"),
          "image OpenWithProgids value delete");
    check(isSafeRegistryDeleteValuePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\.mp4\\OpenWithProgids"),
          "video OpenWithProgids value delete");
    check(!isSafeRegistryDeleteValuePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\.exe\\OpenWithProgids"),
          "non-media OpenWithProgids value rejected");
    check(!isSafeRegistryDeleteValuePath(HKEY_CURRENT_USER,
                                         L"Software\\Classes\\.jpg"),
          "extension class key is not a value path");
    check(!isSafeRegistryDeleteValuePath(
              HKEY_CURRENT_USER,
              L"Software\\Classes\\.jpg\\OpenWithProgids\\Extra"),
          "extra OpenWithProgids segment rejected");
    check(isSafeRegistryDeleteValuePath(HKEY_CURRENT_USER,
                                        L"Software\\RegisteredApplications"),
          "registered applications value delete path");

    return gFailures;
}

} // namespace imview::shell

int main() {
    const int failures = imview::shell::runRegistryGuardTests();
    std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
