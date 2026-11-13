/*
 * p5d_sfx_stub.c: the P5D port's single-file release exe's self-extractor.
 *
 * dev/make_p5d_release.py builds this into a windowed program, appends the payload archive to it and a 24-byte footer
 * at the very end. The first run shows a window with a progress bar and unpacks the archive into
 * %LOCALAPPDATA%\P5D-Port\<version>; then it starts P5D Windows移植版启动器.exe from there with the environment the port needs:
 * ATIV_ROOT (the unpacked tree), P5D_PYTHON (python\python.exe), RECOMP_HOST_CC/CXX (mingw64\bin) and
 * ATIV_WIN_TOOLCHAIN (toolchain\, the SDL3/zlib/ffmpeg development files used to compile the game). Later runs find
 * the marker file and go straight to the launcher.
 *
 * Archive format (little endian): "P5DPAY01" u32 count, then per file: u16 name length, UTF-8 name with '/',
 * u64 raw size, u64 compressed size, then raw-deflate data. Footer (last 24 bytes of the exe): u64 payload offset,
 * u64 version, "P5DEND01".
 *
 * Build: gcc -O2 -mwindows dev/p5d_sfx_stub.c -I<zlib include> <zlib lib>/libz.a -lcomctl32 -o build/p5d_stub.exe
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <zlib.h>
#include <dwmapi.h>

#define PAY_MAGIC "P5DPAY01"
#define END_MAGIC "P5DEND01"
#define FOOTER_SIZE 24
#define WM_PROGRESS (WM_APP + 1)
#define WM_DONE     (WM_APP + 2)
#define WM_FAILED   (WM_APP + 3)

static HWND g_win, g_bar, g_text;
static HBRUSH g_bg;   /* the window's black ground (Persona 5's palette: black, white and red) */
static char g_last_name[256];   /* the file the progress line shows (copied: the worker's stack is gone by the time the UI reads it) */
static void dbg_bytes(const char *s, size_t n) {   /* the extractor's own log (P5D_SFX_LOG): UTF-8, so CJK paths show */
	const char *path = getenv("P5D_SFX_LOG");
	if (!path || !*path) return;
	FILE *f = fopen(path, "ab");
	if (!f) return;
	fwrite(s, 1, n, f);
	fputc('\n', f); fclose(f);
}
static void dbg(const char *fmt, ...) {   /* ASCII messages */
	const char *path = getenv("P5D_SFX_LOG");
	if (!path || !*path) return;
	char buf[2048];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	dbg_bytes(buf, strlen(buf));
}
static void dbgw(const char *tag, const wchar_t *w) {   /* a message and a path as UTF-8 */
	const char *path = getenv("P5D_SFX_LOG");
	if (!path || !*path) return;
	char buf[4096];
	int n = snprintf(buf, sizeof buf, "%s", tag);
	int m = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf + n, (int)sizeof buf - n - 1, NULL, NULL);
	if (m > 0) m--; else m = 0;
	dbg_bytes(buf, (size_t)n + (size_t)m);
}
static LONG WINAPI crash_log(EXCEPTION_POINTERS *ep) {
	dbg("CRASH code=%08lx addr=%p", (unsigned long)ep->ExceptionRecord->ExceptionCode, (void *)ep->ExceptionRecord->ExceptionAddress);
	return EXCEPTION_EXECUTE_HANDLER;
}
static wchar_t g_extract[MAX_PATH * 2];
static wchar_t g_failed[2048];

static uint64_t rd_u64(const unsigned char *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint32_t rd_u32(const unsigned char *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint16_t rd_u16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void fail(const wchar_t *msg) {   /* a message for the window (the caller may append a path) */
	wcscpy_s(g_failed, 2048, msg);
	dbg("FAILED");
	PostMessageW(g_win, WM_FAILED, 0, 0);
}
static void fail_path(const wchar_t *msg, const wchar_t *path) {
	wcscpy_s(g_failed, 2048, msg);
	wcscat_s(g_failed, 2048, L": ");
	wcscat_s(g_failed, 2048, path);
	dbg("FAILED (path)");
	PostMessageW(g_win, WM_FAILED, 0, 0);
}
static int utf8_to_wide(const char *s, wchar_t *out, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n); }
static void mkdirs(const wchar_t *path) {   /* every directory on the way to path */
	wchar_t tmp[MAX_PATH * 4]; wcsncpy_s(tmp, MAX_PATH * 4, path, _TRUNCATE);
	for (size_t i = 1;; i++) {
		wchar_t c = tmp[i];
		if (c == L'\\' || c == L'/' || c == 0) {   /* every prefix, and the path itself (its last part has no separator) */
			tmp[i] = 0;
			if (!CreateDirectoryW(tmp, NULL)) { DWORD e = GetLastError(); if (e != ERROR_ALREADY_EXISTS) dbg("mkdir failed %lu", (unsigned long)e); }
			tmp[i] = c;
			if (c == 0) break;
		}
	}
}
static int read_all(HANDLE h, void *buf, DWORD n) { DWORD got = 0; return ReadFile(h, buf, n, &got, NULL) && got == n; }

/* ---- unpack the archive appended to the exe ---- */
static int extract_payload(const wchar_t *self) {
	HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) { fail(L"无法打开程序。"); return 0; }
	LARGE_INTEGER size; GetFileSizeEx(f, &size);
	if (size.QuadPart < FOOTER_SIZE) { fail(L"程序数据不完整。"); CloseHandle(f); return 0; }
	unsigned char footer[FOOTER_SIZE];
	LARGE_INTEGER at; at.QuadPart = size.QuadPart - FOOTER_SIZE;
	if (!SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !read_all(f, footer, FOOTER_SIZE) || memcmp(footer + 16, END_MAGIC, 8)) {
		fail(L"这个程序没有内置数据包，请重新下载完整的 exe。"); CloseHandle(f); return 0;
	}
	uint64_t payload_off = rd_u64(footer), version = rd_u64(footer + 8);
	uint64_t payload_len = (uint64_t)size.QuadPart - FOOTER_SIZE - payload_off;
	wchar_t base[MAX_PATH * 2];
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH * 2)) wcscpy_s(base, MAX_PATH * 2, L".");
	dbg("payload off=%llu version=%016llX len=%llu", (unsigned long long)payload_off, (unsigned long long)version, (unsigned long long)payload_len);
	swprintf_s(g_extract, MAX_PATH * 2, L"%s\\P5D-Port\\%016llX\\", base, (unsigned long long)version);
	wchar_t marker[MAX_PATH * 2]; swprintf_s(marker, MAX_PATH * 2, L"%s.p5d-ok", g_extract);
	if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) { CloseHandle(f); return 2; }   /* unpacked before */
	LARGE_INTEGER seek; seek.QuadPart = (LONGLONG)payload_off;
	if (!SetFilePointerEx(f, seek, NULL, FILE_BEGIN)) { fail(L"无法读取内置数据包。"); CloseHandle(f); return 0; }
	mkdirs(g_extract);
	unsigned char hdr[12];
	if (!read_all(f, hdr, 12) || memcmp(hdr, PAY_MAGIC, 8)) { fail(L"内置数据包已损坏（头部）。"); CloseHandle(f); return 0; }
	uint32_t count = rd_u32(hdr + 8);
	uint64_t done = 0;
	for (uint32_t i = 0; i < count; i++) {
		unsigned char lenb[2], szb[16];
		char name[4097];
		if (!read_all(f, lenb, 2)) { fail(L"内置数据包不完整。"); CloseHandle(f); return 0; }
		uint16_t nlen = rd_u16(lenb);
		if (!nlen || nlen > 4096) { fail(L"内置数据包里的文件名无效。"); CloseHandle(f); return 0; }
		if (!read_all(f, name, nlen)) { fail(L"内置数据包不完整。"); CloseHandle(f); return 0; }
		name[nlen] = 0;
		if (!read_all(f, szb, 16)) { fail(L"内置数据包不完整。"); CloseHandle(f); return 0; }
		uint64_t raw = rd_u64(szb), comp = rd_u64(szb + 8);
		if (comp > ((uint64_t)1 << 32) || raw > ((uint64_t)1 << 33)) { fail(L"内置数据包里的文件过大。"); CloseHandle(f); return 0; }
		if (strstr(name, "..") || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) { fail(L"内置数据包含有非法路径。"); CloseHandle(f); return 0; }
		dbg("file %u: %s raw=%llu comp=%llu", i, name, (unsigned long long)raw, (unsigned long long)comp);
		unsigned char *cbuf = malloc((size_t)comp), *rbuf = malloc((size_t)raw);
		if (!cbuf || !rbuf) { fail(L"内存不足，无法解压。"); CloseHandle(f); return 0; }
		if (!read_all(f, cbuf, (DWORD)comp)) { fail(L"内置数据包不完整。"); free(cbuf); free(rbuf); CloseHandle(f); return 0; }
		z_stream zs; memset(&zs, 0, sizeof zs);
		if (inflateInit2(&zs, -15) != Z_OK) { fail(L"无法启动解压器。"); CloseHandle(f); return 0; }
		zs.next_in = cbuf; zs.avail_in = (uInt)comp; zs.next_out = rbuf; zs.avail_out = (uInt)raw;
		int r = inflate(&zs, Z_FINISH);
		inflateEnd(&zs);
		if (r != Z_STREAM_END || zs.total_out != raw) { fail(L"内置数据包已损坏（文件）。"); free(cbuf); free(rbuf); CloseHandle(f); return 0; }
		wchar_t wname[MAX_PATH * 2], wpath[MAX_PATH * 4], dir[MAX_PATH * 4];
		utf8_to_wide(name, wname, MAX_PATH * 2);
		for (wchar_t *p = wname; *p; p++) if (*p == L'/') *p = L'\\';
		swprintf_s(wpath, MAX_PATH * 4, L"%s%s", g_extract, wname);
		wcscpy_s(dir, MAX_PATH * 4, wpath);
		wchar_t *slash = wcsrchr(dir, L'\\'); if (slash) { *slash = 0; mkdirs(dir); }
		dbgw("dirs ok, writing ", wpath);
		HANDLE out = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (out == INVALID_HANDLE_VALUE) { dbg("CreateFileW failed: error %lu", GetLastError()); fail_path(L"无法写入文件", wpath); free(cbuf); free(rbuf); CloseHandle(f); return 0; }
		DWORD wrote = 0;
		if (!WriteFile(out, rbuf, (DWORD)raw, &wrote, NULL) || wrote != (DWORD)raw) { fail_path(L"无法写入文件", wpath); CloseHandle(out); free(cbuf); free(rbuf); CloseHandle(f); return 0; }
		CloseHandle(out);
		free(cbuf); free(rbuf);
		dbgw("written ", wpath);
		done += comp + nlen + 18;
		{
			const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
			snprintf(g_last_name, sizeof g_last_name, "%s", base);
		}
		PostMessageW(g_win, WM_PROGRESS, (WPARAM)(payload_len ? (int)(100.0 * (double)done / (double)payload_len) : 100), 0);
	}
	CloseHandle(f);
	HANDLE mk = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (mk != INVALID_HANDLE_VALUE) CloseHandle(mk);
	return 1;
}

/* ---- start the launcher from the extract folder ---- */
static int launch_launcher(void) {
	wchar_t exe[MAX_PATH * 4];
	swprintf_s(exe, MAX_PATH * 4, L"%sP5D Windows移植版启动器.exe", g_extract);
	if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) { fail_path(L"解压后的启动器缺失", exe); return 0; }
	LPWCH envs = GetEnvironmentStringsW();
	size_t n = 0; if (envs) for (wchar_t *p = envs; *p; p += wcslen(p) + 1) n++;
	const size_t extra = 8;
	size_t cap = (n + extra) * (MAX_PATH * 4 + 64) + 16;
	wchar_t *block = malloc(cap * sizeof(wchar_t));
	if (!block) { fail(L"内存不足。"); return 0; }
	wchar_t *w = block; size_t left = cap;
	if (envs) {
		for (wchar_t *p = envs; *p; p += wcslen(p) + 1) {
			size_t l = wcslen(p) + 1;
			if (l >= left) break;
			wmemcpy(w, p, l); w += l; left -= l;
		}
		FreeEnvironmentStringsW(envs);
	}
	struct { const wchar_t *k; wchar_t v[MAX_PATH * 4]; } vars[6];
	vars[0].k = L"ATIV_ROOT";      wcscpy_s(vars[0].v, MAX_PATH * 4, g_extract);
	vars[1].k = L"P5D_PYTHON";     swprintf_s(vars[1].v, MAX_PATH * 4, L"%spython\\python.exe", g_extract);
	vars[2].k = L"RECOMP_HOST_CC"; swprintf_s(vars[2].v, MAX_PATH * 4, L"%smingw64\\bin\\gcc.exe", g_extract);
	vars[3].k = L"RECOMP_HOST_CXX"; swprintf_s(vars[3].v, MAX_PATH * 4, L"%smingw64\\bin\\g++.exe", g_extract);
	vars[4].k = L"ATIV_WIN_TOOLCHAIN"; swprintf_s(vars[4].v, MAX_PATH * 4, L"%stoolchain", g_extract);
	vars[5].k = L"P5D_LAUNCHER"; wcscpy_s(vars[5].v, MAX_PATH * 4, L"1");
	/* a variable already set from outside wins (a development bat can point the launcher at the source tree and the
	 * library the player's other bat uses: then the game this launcher starts is the very same one, reported 2026-11-07) */
	for (int i = 0; i < 6; i++) if (GetEnvironmentVariableW(vars[i].k, NULL, 0) > 1) vars[i].k = L"";
	for (int i = 0; i < 6; i++) {
		if (!vars[i].k[0]) continue;
		size_t l = wcslen(vars[i].k) + 1 + wcslen(vars[i].v) + 1;
		if (l >= left) break;
		swprintf_s(w, left, L"%s=%s", vars[i].k, vars[i].v);
		w += l; left -= l;
	}
	*w = 0;
	STARTUPINFOW si = { sizeof si };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(NULL, exe, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, block, g_extract, &si, &pi)) {
		fail(L"无法启动启动器。");
		free(block);
		return 0;
	}
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	free(block);
	return 1;
}

static DWORD WINAPI worker(LPVOID unused) {
	(void)unused;
	wchar_t self[MAX_PATH * 4];
	if (!GetModuleFileNameW(NULL, self, MAX_PATH * 4)) { fail(L"找不到本程序。"); return 0; }
	int r = extract_payload(self);
	if (r == 0) return 0;
	PostMessageW(g_win, WM_DONE, (WPARAM)r, 0);
	return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM p, LPARAM l) {
	switch (m) {
	case WM_PROGRESS: {
		SendMessageW(g_bar, PBM_SETPOS, p, 0);
		wchar_t wname[256] = L"";
		utf8_to_wide(g_last_name, wname, 256);
		wchar_t text[512]; swprintf_s(text, 512, L"正在解压 %d%%  %s", (int)p, wname);
		SetWindowTextW(g_text, text);
		return 0;
	}
	case WM_DONE:   /* p == 1: just unpacked; 2: was unpacked before -- both start the launcher */
		SetWindowTextW(g_text, L"正在启动…");
		if (!launch_launcher()) return 0;
		PostMessageW(h, WM_CLOSE, 0, 0);
		return 0;
	case WM_FAILED:
		MessageBoxW(h, g_failed, L"P5D Windows移植版启动器", MB_ICONERROR | MB_OK);
		PostMessageW(h, WM_CLOSE, 0, 0);
		return 0;
	case WM_CTLCOLORSTATIC: {
		HDC dc = (HDC)p;
		SetTextColor(dc, RGB(245, 245, 245));
		SetBkColor(dc, RGB(10, 10, 12));
		return (LRESULT)g_bg;
	}
	case WM_ERASEBKGND: {
		RECT r; GetClientRect(h, &r); FillRect((HDC)p, &r, g_bg);
		return 1;
	}
	case WM_CLOSE: DestroyWindow(h); return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcW(h, m, p, l);
}

int main(void) {
	SetUnhandledExceptionFilter(crash_log);
	InitCommonControls();
	WNDCLASSEXW wc = { sizeof wc };
	wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"P5D_SFX";
	g_bg = CreateSolidBrush(RGB(10, 10, 12));
	wc.hbrBackground = g_bg;
	RegisterClassExW(&wc);
	g_win = CreateWindowExW(0, L"P5D_SFX", L"P5D Windows移植版启动器 — 首次运行", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, 540, 160, NULL, NULL, wc.hInstance, NULL);
	{ BOOL dark = TRUE; DwmSetWindowAttribute(g_win, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark); }
	g_text = CreateWindowExW(0, L"STATIC", L"正在准备运行环境…", WS_CHILD | WS_VISIBLE | SS_LEFT,
		20, 20, 490, 40, g_win, NULL, wc.hInstance, NULL);
	g_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE,
		20, 75, 490, 26, g_win, NULL, wc.hInstance, NULL);
	SendMessageW(g_bar, PBM_SETRANGE32, 0, 100);
	SendMessageW(g_bar, PBM_SETBKCOLOR, 0, RGB(38, 38, 42));   /* the bar's track: dark grey */
	SendMessageW(g_bar, PBM_SETBARCOLOR, 0, RGB(230, 0, 18));  /* its fill: the game's red */
	CreateThread(NULL, 0, worker, NULL, 0, NULL);
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
	return 0;
}
