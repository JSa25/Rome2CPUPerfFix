// A fresh, empty test folder %TEMP%\<prefix>_<pid>, deleted again when the test exits. A folder left by an older run
// with the same process id is emptied first: its files (a dev fgbridge_on.txt, say) must not change this run.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static void tmp_del_tree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH];
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) tmp_del_tree(p); else DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static wchar_t g_tmp_dir[MAX_PATH];
static void tmp_cleanup(void) { tmp_del_tree(g_tmp_dir); }

static void tmp_dir(wchar_t *dir, const wchar_t *prefix) {
    GetTempPathW(MAX_PATH, dir);
    swprintf(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"%ls_%lu", prefix, GetCurrentProcessId());
    tmp_del_tree(dir);
    CreateDirectoryW(dir, NULL);
    wcscpy(g_tmp_dir, dir);
    atexit(tmp_cleanup);
}
