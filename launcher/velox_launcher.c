/* Velox Preview 0.1 コンソール非表示ネイティブ Win32 GUI ランチャー
 *
 * 自身の実行ファイル配置ディレクトリ（例: <VELOX_INSTALL_DIR>）を自動検出し、
 * 開発環境（<VELOX_DEV_ROOT>）や Windows 全体の PATH に依存せず、
 * 同梱の tools\python\pythonw.exe から app\velox\gui\__main__.py を起動する。
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <wchar.h>

static void show_error(const wchar_t* msg) {
    MessageBoxW(NULL, msg, L"Velox Preview 0.1 - 起動エラー", MB_OK | MB_ICONERROR);
}

static int file_exists(const wchar_t* path) {
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hInst;
    (void)hPrev;
    (void)nCmdShow;

    wchar_t exePath[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(NULL, exePath, (DWORD)(sizeof(exePath) / sizeof(exePath[0])));
    if (len == 0 || len >= (DWORD)(sizeof(exePath) / sizeof(exePath[0]))) {
        show_error(L"Velox.exe のパスを取得できませんでした。");
        return 1;
    }

    // 親ディレクトリを取得
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) {
        show_error(L"配置ディレクトリを特定できませんでした。");
        return 1;
    }
    *lastSlash = L'\0';

    wchar_t pyExe[MAX_PATH * 4];
    wchar_t pyHome[MAX_PATH * 4];
    wchar_t appDir[MAX_PATH * 4];
    wchar_t entryScript[MAX_PATH * 4];
    wchar_t tclLib[MAX_PATH * 4];
    wchar_t tkLib[MAX_PATH * 4];

    swprintf(pyHome, MAX_PATH * 4, L"%ls\\tools\\python", exePath);
    swprintf(pyExe, MAX_PATH * 4, L"%ls\\tools\\python\\pythonw.exe", exePath);
    swprintf(appDir, MAX_PATH * 4, L"%ls\\app", exePath);
    swprintf(entryScript, MAX_PATH * 4, L"%ls\\app\\velox\\gui\\__main__.py", exePath);
    swprintf(tclLib, MAX_PATH * 4, L"%ls\\tools\\python\\tcl\\tcl8.6", exePath);
    swprintf(tkLib, MAX_PATH * 4, L"%ls\\tools\\python\\tcl\\tk8.6", exePath);

    if (!file_exists(pyExe)) {
        wchar_t err[MAX_PATH * 4 + 128];
        swprintf(err, MAX_PATH * 4 + 128, L"同梱 Python ランタイムが見つかりません:\n%ls", pyExe);
        show_error(err);
        return 2;
    }
    if (!file_exists(entryScript)) {
        wchar_t err[MAX_PATH * 4 + 128];
        swprintf(err, MAX_PATH * 4 + 128, L"Velox GUI スクリプトが見つかりません:\n%ls", entryScript);
        show_error(err);
        return 3;
    }

    // 実用環境を完全に自己完結させる環境変数を子プロセスへ設定
    SetEnvironmentVariableW(L"VELOX_HOME", exePath);
    SetEnvironmentVariableW(L"PYTHONHOME", pyHome);
    SetEnvironmentVariableW(L"PYTHONPATH", appDir);
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"TCL_LIBRARY", tclLib);
    SetEnvironmentVariableW(L"TK_LIBRARY", tkLib);

    wchar_t cmdLine[MAX_PATH * 8];
    if (lpCmdLine && lpCmdLine[0] != L'\0') {
        swprintf(cmdLine, MAX_PATH * 8, L"\"%ls\" -s \"%ls\" %ls", pyExe, entryScript, lpCmdLine);
    } else {
        swprintf(cmdLine, MAX_PATH * 8, L"\"%ls\" -s \"%ls\"", pyExe, entryScript);
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(
            pyExe,
            cmdLine,
            NULL,
            NULL,
            FALSE,
            CREATE_NO_WINDOW,
            NULL,
            exePath,
            &si,
            &pi)) {
        wchar_t err[512];
        swprintf(err, 512, L"Velox GUI の起動に失敗しました (Win32 Error %lu)。", GetLastError());
        show_error(err);
        return 4;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
