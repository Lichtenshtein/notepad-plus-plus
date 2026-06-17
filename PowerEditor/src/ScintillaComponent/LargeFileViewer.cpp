// This file is part of Notepad++ project
// Copyright (C)2026 Don HO <don.h@free.fr>

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// -----------------------------------------------------------------------------
// LargeFileViewer - windowed read-only viewer whose peak memory is FIXED for any
// file size (a 10 GB file costs the same as a 10 MB one).
//
// Two design rules make that true:
//   1. The file is never memory-mapped; only a small WIN_BYTES slice is ever held.
//   2. ALL reads use FILE_FLAG_NO_BUFFERING, so bytes go straight into our own
//      buffers and never populate the Windows file cache. Without this, buffered
//      ReadFile would fill the system cache with gigabytes while scanning/scrolling
//      and choke a low-RAM machine even though our process buffers stay small.
//
// NO_BUFFERING requires sector-aligned offset/length/buffer; readAligned() hides
// that: it reads an aligned span into a sector-aligned scratch buffer, then copies
// out the requested logical range.
// -----------------------------------------------------------------------------

#include "LargeFileViewer.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>

#include <psapi.h>
#pragma comment(lib, "Psapi.lib")

#include <Sci_Position.h>
#include <Scintilla.h>

#define WM_LFV_INDEX_DONE  (WM_APP + 1)
#define WM_LFV_SEARCH_DONE (WM_APP + 2) // lParam = match byte, or -1 not found, -2 cancelled

namespace
{
	constexpr int64_t SECTOR = 4096; // alignment for NO_BUFFERING (covers 512 and 4K disks)

	// ---- file ----
	HANDLE         g_hFile = INVALID_HANDLE_VALUE; // UI-thread handle (display + byte->line)
	std::wstring   g_filePath;
	const wchar_t* g_fileNamePtr = L"";
	int64_t        g_size = 0;

	// ---- Scintilla / window ----
	SciFnDirect   g_fn = nullptr;
	sptr_t        g_ptr = 0;
	HWND          g_hSci = nullptr;
	HWND          g_hEdit = nullptr;

	std::vector<char> g_winBuf;        // logical slice handed to Scintilla
	char*         g_dispScratch = nullptr; // sector-aligned scratch for display reads
	char*         g_idxScratch = nullptr;  // sector-aligned scratch for byte->line reads
	int64_t       g_winStart = 0;
	int64_t       g_winEnd = 0;
	int64_t       g_winStartLine = -1;
	bool          g_shifting = false;

	// ---- whole-file scrollbar ----
	constexpr int SCROLL_RANGE = 100000;
	constexpr int SCROLL_PAGE  = SCROLL_RANGE / 50;
	int           g_lastTrackPos = 0;
	unsigned long g_lastLoadTick = 0;

	// ---- sparse line index (background) ----
	std::vector<int64_t> g_lineOffsets;
	int64_t              g_totalLines = 0;
	std::atomic<bool>    g_indexReady{ false };
	std::atomic<bool>    g_abortIndex{ false };
	constexpr int64_t    INDEX_STRIDE = 2000;

	// ---- search (background) ----
	std::string       g_needle;
	int               g_searchLen = 0;
	int64_t           g_lastMatchByte = -1;
	std::atomic<bool> g_searching{ false };
	std::atomic<bool> g_abortSearch{ false };
	std::thread       g_indexThread;   // joined on shutdown so it can't outlive globals
	std::thread       g_searchThread;
	std::atomic<bool> g_viewerActive{ false }; // one viewer at a time (global state is shared)

	// ---- tuning ----
	constexpr int64_t WIN_BYTES        = 8 * 1024 * 1024;
	constexpr int64_t SHIFT_BYTES      = 4 * 1024 * 1024;
	constexpr int64_t BUFFER_LINES     = 800;
	constexpr int64_t SCAN_CHUNK       = 8 * 1024 * 1024;
	constexpr int64_t DISP_SCRATCH_CAP = WIN_BYTES + 2 * SECTOR;
	constexpr int64_t IDX_SCRATCH_CAP  = 65536 + 2 * SECTOR;
	constexpr int64_t SCAN_SCRATCH_CAP = SCAN_CHUNK + 2 * SECTOR;
	constexpr int     EDIT_H           = 26;
	constexpr int64_t HUGE_THRESHOLD   = 2LL * 1024 * 1024 * 1024; // offer the viewer at >= 2 GB

	inline sptr_t sci(unsigned int msg, uptr_t w = 0, sptr_t l = 0)
	{
		return g_fn(g_ptr, msg, w, l);
	}

	HANDLE openUnbuffered()
	{
		return ::CreateFileW(g_filePath.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_FLAG_NO_BUFFERING, nullptr);
	}

	// Read logical [offset, offset+len) into dst via a NO_BUFFERING handle, using a
	// sector-aligned scratch buffer. Returns logical bytes copied.
	int64_t readAligned(HANDLE hf, int64_t offset, char* dst, int64_t len, char* scratch, int64_t scratchCap)
	{
		if (len <= 0 || offset < 0 || offset >= g_size) return 0;
		const int64_t aStart = offset & ~(SECTOR - 1);
		const int frontPad = static_cast<int>(offset - aStart);
		int64_t aLen = ((frontPad + len + SECTOR - 1) / SECTOR) * SECTOR;
		if (aLen > scratchCap) aLen = (scratchCap / SECTOR) * SECTOR;

		LARGE_INTEGER li{}; li.QuadPart = aStart;
		if (!::SetFilePointerEx(hf, li, nullptr, FILE_BEGIN)) return 0;

		int64_t got = 0;
		while (got < aLen)
		{
			const DWORD want = static_cast<DWORD>(std::min<int64_t>(aLen - got, 16 * 1024 * 1024));
			DWORD r = 0;
			if (!::ReadFile(hf, scratch + got, want, &r, nullptr)) break;
			got += r;
			if (r < want) break; // reached EOF
		}
		int64_t avail = got - frontPad;
		if (avail < 0) avail = 0;
		if (avail > len) avail = len;
		if (avail > 0) std::memcpy(dst, scratch + frontPad, static_cast<size_t>(avail));
		return avail;
	}

	int64_t byteToLine(int64_t byte)
	{
		if (byte <= 0 || !g_indexReady.load()) return 0;
		auto it = std::upper_bound(g_lineOffsets.begin(), g_lineOffsets.end(), byte);
		int64_t ck = (it - g_lineOffsets.begin()) - 1;
		if (ck < 0) ck = 0;
		int64_t line = ck * INDEX_STRIDE;
		int64_t pos = g_lineOffsets[static_cast<size_t>(ck)];
		char tmp[65536];
		while (pos < byte)
		{
			const int64_t want = std::min<int64_t>(byte - pos, static_cast<int64_t>(sizeof(tmp)));
			const int64_t got = readAligned(g_hFile, pos, tmp, want, g_idxScratch, IDX_SCRATCH_CAP);
			if (got <= 0) break;
			for (int64_t i = 0; i < got; ++i) if (tmp[i] == '\n') ++line;
			pos += got;
		}
		return line;
	}

	void buildIndex(HWND hFrame)
	{
		HANDLE hf = openUnbuffered();
		if (hf == INVALID_HANDLE_VALUE) return;
		char* scratch = static_cast<char*>(::VirtualAlloc(nullptr, static_cast<size_t>(SCAN_SCRATCH_CAP), MEM_COMMIT, PAGE_READWRITE));
		std::vector<char> buf(static_cast<size_t>(SCAN_CHUNK));
		if (!scratch) { ::CloseHandle(hf); return; }

		std::vector<int64_t> offs;
		offs.push_back(0);
		int64_t filePos = 0;
		int64_t line = 0;
		char lastCh = '\n';
		while (filePos < g_size && !g_abortIndex.load())
		{
			const int64_t got = readAligned(hf, filePos, buf.data(), SCAN_CHUNK, scratch, SCAN_SCRATCH_CAP);
			if (got <= 0) break;
			for (int64_t i = 0; i < got; ++i)
			{
				if (buf[static_cast<size_t>(i)] == '\n')
				{
					++line;
					if ((line % INDEX_STRIDE) == 0)
						offs.push_back(filePos + i + 1);
				}
			}
			lastCh = buf[static_cast<size_t>(got - 1)];
			filePos += got;
		}
		::VirtualFree(scratch, 0, MEM_RELEASE);
		::CloseHandle(hf);
		if (g_abortIndex.load()) return;

		g_lineOffsets = std::move(offs);
		g_totalLines = (lastCh != '\n') ? line + 1 : line;
		g_indexReady.store(true);
		::PostMessage(hFrame, WM_LFV_INDEX_DONE, 0, 0);
	}

	int64_t absoluteTopByte()
	{
		const int64_t firstVis = sci(SCI_GETFIRSTVISIBLELINE);
		const int64_t inWin = sci(SCI_POSITIONFROMLINE, static_cast<uptr_t>(firstVis));
		return g_winStart + (inWin < 0 ? 0 : inWin);
	}

	void setTitle(HWND hFrame)
	{
		const int64_t firstVis = sci(SCI_GETFIRSTVISIBLELINE);
		const int64_t top = absoluteTopByte();
		const double gb  = g_size / 1073741824.0;
		const double pct = g_size ? (100.0 * top / g_size) : 0.0;
		wchar_t buf[700];
		if (g_indexReady.load() && g_winStartLine >= 0)
		{
			const long long line = static_cast<long long>(g_winStartLine + firstVis) + 1;
			swprintf(buf, 700,
				L"LargeFile PoC  -  %s  [%.2f GB]  -  line %lld / %lld  (%.2f%%)   |  Ctrl+F find, F3 next, wheel/PgUp/PgDn scroll, Ctrl+Home/End",
				g_fileNamePtr, gb, line, static_cast<long long>(g_totalLines), pct);
		}
		else
		{
			swprintf(buf, 700,
				L"LargeFile PoC  -  %s  [%.2f GB]  -  %.3f%%  (indexing...)   |  Ctrl+F find, wheel/PgUp/PgDn scroll, Ctrl+Home/End",
				g_fileNamePtr, gb, pct);
		}
		::SetWindowTextW(hFrame, buf);
	}

	void loadWindowAt(int64_t start, HWND hFrame)
	{
		if (g_size <= 0) return;
		if (start > g_size - WIN_BYTES) start = g_size - WIN_BYTES;
		if (start < 0) start = 0;

		int64_t readLen = WIN_BYTES;
		if (start + readLen > g_size) readLen = g_size - start;
		const int64_t got = readAligned(g_hFile, start, g_winBuf.data(), readLen, g_dispScratch, DISP_SCRATCH_CAP);
		readLen = got;

		int64_t s = 0;
		if (start > 0)
		{
			while (s < readLen && g_winBuf[static_cast<size_t>(s)] != '\n') ++s;
			if (s < readLen) ++s;
		}
		int64_t e = readLen;
		if (start + readLen < g_size)
		{
			while (e > s && g_winBuf[static_cast<size_t>(e - 1)] != '\n') --e;
		}

		g_winStart = start + s;
		g_winEnd = start + e;

		sci(SCI_SETREADONLY, 0);
		sci(SCI_CLEARALL);
		sci(SCI_APPENDTEXT, static_cast<uptr_t>(e - s), reinterpret_cast<sptr_t>(g_winBuf.data() + s));
		sci(SCI_SETREADONLY, 1);

		g_winStartLine = g_indexReady.load() ? byteToLine(g_winStart) : -1;
		setTitle(hFrame);
	}

	void updateScrollThumb(HWND hFrame)
	{
		const double frac = g_size ? static_cast<double>(absoluteTopByte()) / g_size : 0.0;
		SCROLLINFO si{};
		si.cbSize = sizeof(si);
		si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
		si.nMin   = 0;
		si.nMax   = SCROLL_RANGE;
		si.nPage  = SCROLL_PAGE;
		si.nPos   = static_cast<int>(frac * (SCROLL_RANGE - SCROLL_PAGE));
		::SetScrollInfo(hFrame, SB_VERT, &si, TRUE);
	}

	void shiftWindow(int dir, HWND hFrame)
	{
		g_shifting = true;
		const int64_t firstVis = sci(SCI_GETFIRSTVISIBLELINE);
		const int64_t oldStartLine = g_winStartLine;
		int64_t newStart = (dir > 0) ? g_winStart + SHIFT_BYTES : g_winStart - SHIFT_BYTES;
		if (newStart < 0) newStart = 0;

		loadWindowAt(newStart, hFrame);

		int64_t newFirstVis;
		if (oldStartLine >= 0 && g_winStartLine >= 0)
			newFirstVis = (oldStartLine + firstVis) - g_winStartLine;
		else
			newFirstVis = firstVis;

		const int64_t total = sci(SCI_GETLINECOUNT);
		if (newFirstVis < 0) newFirstVis = 0;
		if (newFirstVis > total - 1) newFirstVis = total - 1;
		sci(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(newFirstVis));

		g_shifting = false;
		updateScrollThumb(hFrame);
		setTitle(hFrame);
	}

	void onScroll(HWND hFrame)
	{
		if (g_shifting) return;
		const int64_t firstVis = sci(SCI_GETFIRSTVISIBLELINE);
		const int64_t onScreen = sci(SCI_LINESONSCREEN);
		const int64_t total    = sci(SCI_GETLINECOUNT);

		if (firstVis < BUFFER_LINES && g_winStart > 0)                       { shiftWindow(-1, hFrame); return; }
		if (firstVis + onScreen > total - BUFFER_LINES && g_winEnd < g_size) { shiftWindow(+1, hFrame); return; }
		updateScrollThumb(hFrame);
		setTitle(hFrame);
	}

	void jumpToScrollPos(HWND hFrame, int pos)
	{
		const int effRange = SCROLL_RANGE - SCROLL_PAGE;
		if (pos < 0) pos = 0;
		if (pos > effRange) pos = effRange;

		g_shifting = true;
		if (pos <= 0)
		{
			loadWindowAt(0, hFrame);
			sci(SCI_SETFIRSTVISIBLELINE, 0);
		}
		else if (pos >= effRange)
		{
			loadWindowAt(g_size - WIN_BYTES, hFrame);
			sci(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(sci(SCI_GETLINECOUNT)));
		}
		else
		{
			const int64_t target = static_cast<int64_t>(static_cast<double>(pos) / effRange * g_size);
			loadWindowAt(target - WIN_BYTES / 2, hFrame);
			const int64_t rel = target - g_winStart;
			int64_t line = (rel > 0) ? sci(SCI_LINEFROMPOSITION, static_cast<uptr_t>(rel)) : 0;
			const int64_t total = sci(SCI_GETLINECOUNT);
			if (line > total - 1) line = total - 1;
			if (line < 0) line = 0;
			sci(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(line));
		}
		g_shifting = false;
		updateScrollThumb(hFrame);
		setTitle(hFrame);
	}

	// ---- search ----

	int64_t scanFile(int64_t start)
	{
		const int len = g_searchLen;
		if (len <= 0) return -1;
		HANDLE hf = openUnbuffered();
		if (hf == INVALID_HANDLE_VALUE) return -1;
		char* scratch = static_cast<char*>(::VirtualAlloc(nullptr, static_cast<size_t>(SCAN_SCRATCH_CAP), MEM_COMMIT, PAGE_READWRITE));
		if (!scratch) { ::CloseHandle(hf); return -1; }
		std::vector<char> buf(static_cast<size_t>(SCAN_CHUNK + len));

		int64_t filePos = start;
		int carry = 0;
		int64_t result = -1;
		while (filePos < g_size)
		{
			if (g_abortSearch.load()) { result = -2; break; }
			const int64_t got = readAligned(hf, filePos, buf.data() + carry, SCAN_CHUNK, scratch, SCAN_SCRATCH_CAP);
			if (got <= 0) break;
			const int64_t avail = carry + got;
			for (int64_t i = 0; i + len <= avail; ++i)
			{
				if (buf[static_cast<size_t>(i)] == g_needle[0] &&
				    std::memcmp(buf.data() + i, g_needle.data(), len) == 0)
				{
					result = (filePos - carry) + i;
					break;
				}
			}
			if (result != -1) break;
			carry = static_cast<int>(std::min<int64_t>(len - 1, avail));
			std::memmove(buf.data(), buf.data() + (avail - carry), carry);
			filePos += got;
		}
		::VirtualFree(scratch, 0, MEM_RELEASE);
		::CloseHandle(hf);
		return result;
	}

	void searchThread(HWND hFrame, int64_t from)
	{
		int64_t r = scanFile(from);
		if (r == -1 && from > 0 && !g_abortSearch.load())
			r = scanFile(0);
		::PostMessage(hFrame, WM_LFV_SEARCH_DONE, 0, static_cast<LPARAM>(r));
	}

	void gotoMatch(HWND hFrame, int64_t matchByte, int len)
	{
		g_shifting = true;
		loadWindowAt(matchByte - WIN_BYTES / 2, hFrame);
		const int64_t rel = matchByte - g_winStart;
		sci(SCI_SETSEL, static_cast<uptr_t>(rel), static_cast<sptr_t>(rel + len));
		const int64_t line = sci(SCI_LINEFROMPOSITION, static_cast<uptr_t>(rel));
		const int64_t onScreen = sci(SCI_LINESONSCREEN);
		int64_t fv = line - onScreen / 3;
		if (fv < 0) fv = 0;
		sci(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(fv));
		g_shifting = false;
		updateScrollThumb(hFrame);
		setTitle(hFrame);
	}

	void doSearch(HWND hFrame, int64_t fromByte)
	{
		if (g_searching.load()) return;
		wchar_t wbuf[256]{};
		::GetWindowTextW(g_hEdit, wbuf, 256);
		if (wbuf[0] == 0) return;
		char needle[768]{};
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, needle, sizeof(needle), nullptr, nullptr);
		if (n <= 1) return;
		g_needle.assign(needle, static_cast<size_t>(n - 1));
		g_searchLen = n - 1;
		g_abortSearch.store(false);
		g_searching.store(true);
		::SetWindowTextW(hFrame, L"LargeFile PoC  -  searching...  (Esc to cancel)");
		// The previous search has finished (we only get here with g_searching false),
		// but its thread object may still be joinable - reap it before starting anew.
		if (g_searchThread.joinable()) g_searchThread.join();
		g_searchThread = std::thread(searchThread, hFrame, fromByte);
	}

	LRESULT CALLBACK frameProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		switch (m)
		{
			case WM_SIZE:
			{
				const int wd = LOWORD(l), ht = HIWORD(l);
				if (g_hEdit) ::MoveWindow(g_hEdit, 0, 0, wd, EDIT_H, TRUE);
				if (g_hSci)  ::MoveWindow(g_hSci, 0, EDIT_H, wd, ht - EDIT_H, TRUE);
				return 0;
			}

			case WM_LFV_INDEX_DONE:
				if (!g_shifting)
				{
					g_winStartLine = byteToLine(g_winStart);
					setTitle(h);
				}
				return 0;

			case WM_LFV_SEARCH_DONE:
			{
				g_searching.store(false);
				const int64_t mb = static_cast<int64_t>(l);
				if (mb >= 0) { g_lastMatchByte = mb; gotoMatch(h, mb, g_searchLen); }
				else { if (mb == -1) ::MessageBeep(MB_ICONEXCLAMATION); setTitle(h); }
				return 0;
			}

			case WM_VSCROLL:
			{
				SCROLLINFO si{};
				si.cbSize = sizeof(si);
				si.fMask  = SIF_ALL;
				::GetScrollInfo(h, SB_VERT, &si);
				switch (LOWORD(w))
				{
					case SB_LINEUP:   sci(SCI_LINESCROLL, 0, -1); break;
					case SB_LINEDOWN: sci(SCI_LINESCROLL, 0, 1); break;
					case SB_PAGEUP:   jumpToScrollPos(h, si.nPos - SCROLL_PAGE * 3); break;
					case SB_PAGEDOWN: jumpToScrollPos(h, si.nPos + SCROLL_PAGE * 3); break;
					case SB_THUMBTRACK:
					{
						g_lastTrackPos = si.nTrackPos;
						const unsigned long now = ::GetTickCount();
						if (now - g_lastLoadTick > 60)
						{
							g_lastLoadTick = now;
							jumpToScrollPos(h, si.nTrackPos);
						}
						else
						{
							SCROLLINFO sp{}; sp.cbSize = sizeof(sp); sp.fMask = SIF_POS; sp.nPos = si.nTrackPos;
							::SetScrollInfo(h, SB_VERT, &sp, TRUE);
						}
						break;
					}
					case SB_THUMBPOSITION:
						jumpToScrollPos(h, si.nTrackPos);
						break;
					default: break;
				}
				return 0;
			}

			case WM_NOTIFY:
			{
				auto* scn = reinterpret_cast<SCNotification*>(l);
				if (scn && scn->nmhdr.hwndFrom == g_hSci && scn->nmhdr.code == SCN_UPDATEUI)
					onScroll(h);
				return 0;
			}

			case WM_SETFOCUS:
				if (g_hSci) ::SetFocus(g_hSci);
				return 0;

			case WM_DESTROY:
				g_abortIndex.store(true);
				g_abortSearch.store(true);
				::PostQuitMessage(0);
				return 0;
		}
		return ::DefWindowProc(h, m, w, l);
	}
}

int LargeFileViewer::runStandalone(HINSTANCE hInst, const wchar_t* filePath, bool benchMode)
{
	LARGE_INTEGER freq{}, t0{}, t1{};
	::QueryPerformanceFrequency(&freq);
	::QueryPerformanceCounter(&t0);

	g_filePath = filePath;
	g_hFile = openUnbuffered();
	if (g_hFile == INVALID_HANDLE_VALUE)
	{
		::MessageBoxW(nullptr, filePath, L"LargeFile PoC: cannot open file", MB_OK | MB_ICONERROR);
		return 1;
	}

	LARGE_INTEGER li{};
	::GetFileSizeEx(g_hFile, &li);
	g_size = li.QuadPart;

	const wchar_t* slash = wcsrchr(filePath, L'\\');
	g_fileNamePtr = slash ? slash + 1 : filePath;

	g_winBuf.resize(static_cast<size_t>(WIN_BYTES));
	g_dispScratch = static_cast<char*>(::VirtualAlloc(nullptr, static_cast<size_t>(DISP_SCRATCH_CAP), MEM_COMMIT, PAGE_READWRITE));
	g_idxScratch  = static_cast<char*>(::VirtualAlloc(nullptr, static_cast<size_t>(IDX_SCRATCH_CAP), MEM_COMMIT, PAGE_READWRITE));

	auto cleanup = []()
	{
		if (g_dispScratch) { ::VirtualFree(g_dispScratch, 0, MEM_RELEASE); g_dispScratch = nullptr; }
		if (g_idxScratch)  { ::VirtualFree(g_idxScratch, 0, MEM_RELEASE); g_idxScratch = nullptr; }
		if (g_hFile != INVALID_HANDLE_VALUE) { ::CloseHandle(g_hFile); g_hFile = INVALID_HANDLE_VALUE; }
	};

	if (!g_dispScratch || !g_idxScratch)
	{
		::MessageBoxW(nullptr, L"Out of memory allocating buffers", L"LargeFile PoC", MB_OK | MB_ICONERROR);
		cleanup();
		return 1;
	}

	WNDCLASSW wc{};
	wc.lpfnWndProc   = frameProc;
	wc.hInstance     = hInst;
	wc.lpszClassName = L"NppLargeFilePoC";
	wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	::RegisterClassW(&wc);

	HWND hFrame = ::CreateWindowExW(0, L"NppLargeFilePoC", L"LargeFile PoC",
		WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750,
		nullptr, nullptr, hInst, nullptr);
	if (!hFrame)
	{
		::MessageBoxW(nullptr, L"Cannot create viewer window", L"LargeFile PoC", MB_OK | MB_ICONERROR);
		cleanup();
		return 1;
	}

	Scintilla_RegisterClasses(hInst);

	RECT rc{};
	::GetClientRect(hFrame, &rc);

	g_hEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, rc.right, EDIT_H, hFrame, nullptr, hInst, nullptr);
	::SendMessageW(g_hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);

	g_hSci = ::CreateWindowExW(0, L"Scintilla", L"",
		WS_CHILD | WS_VISIBLE | WS_HSCROLL, 0, EDIT_H, rc.right, rc.bottom - EDIT_H, hFrame, nullptr, hInst, nullptr);

	g_fn  = g_hSci ? reinterpret_cast<SciFnDirect>(::SendMessage(g_hSci, SCI_GETDIRECTFUNCTION, 0, 0)) : nullptr;
	g_ptr = g_hSci ? static_cast<sptr_t>(::SendMessage(g_hSci, SCI_GETDIRECTPOINTER, 0, 0)) : 0;
	if (!g_hSci || !g_fn || !g_ptr)
	{
		::MessageBoxW(nullptr, L"Cannot initialise Scintilla", L"LargeFile PoC", MB_OK | MB_ICONERROR);
		::DestroyWindow(hFrame);
		cleanup();
		return 1;
	}

	sci(SCI_SETCODEPAGE, SC_CP_UTF8);
	sci(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>("Consolas"));
	sci(SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
	sci(SCI_STYLECLEARALL);
	sci(SCI_SETVSCROLLBAR, 0);
	sci(SCI_SETCARETWIDTH, 0);
	// Read-only viewer: never accumulate undo history. Each window load does a
	// CLEARALL + APPENDTEXT of WIN_BYTES; with undo collection on (the default),
	// hundreds of loads (fast F3 / dragging) would pile up GBs of undo buffer.
	sci(SCI_SETUNDOCOLLECTION, 0);
	sci(SCI_EMPTYUNDOBUFFER);

	loadWindowAt(0, hFrame);
	updateScrollThumb(hFrame);

	::QueryPerformanceCounter(&t1);
	const double loadMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

	if (benchMode)
	{
		PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb = sizeof(pmc);
		::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
		const double peakWsMB = pmc.PeakWorkingSetSize / 1048576.0;
		wchar_t tmp[MAX_PATH]{};
		::GetTempPathW(MAX_PATH, tmp);
		const std::wstring outPath = std::wstring(tmp) + L"lfv_bench.json";
		FILE* f = _wfopen(outPath.c_str(), L"w");
		if (f)
		{
			fprintf(f, "{\"loadMs\":%.1f,\"peakWS_MB\":%.0f,\"sizeGB\":%.3f,\"winBytesMB\":%lld}",
				loadMs, peakWsMB, g_size / 1073741824.0, static_cast<long long>(WIN_BYTES / 1048576));
			fclose(f);
		}
		cleanup();
		return 0;
	}

	::ShowWindow(hFrame, SW_SHOW);
	::UpdateWindow(hFrame);
	::SetFocus(g_hSci);

	g_indexThread = std::thread(buildIndex, hFrame);

	MSG msg{};
	while (::GetMessage(&msg, nullptr, 0, 0))
	{
		if (msg.message == WM_KEYDOWN)
		{
			const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
			const bool inEdit = (::GetFocus() == g_hEdit);
			const sptr_t onScreen = sci(SCI_LINESONSCREEN);
			bool handled = true;
			if (ctrl && msg.wParam == 'F')
			{
				::SetFocus(g_hEdit);
				::SendMessageW(g_hEdit, EM_SETSEL, 0, -1);
			}
			else if (msg.wParam == VK_F3)
				doSearch(hFrame, g_lastMatchByte >= 0 ? g_lastMatchByte + 1 : absoluteTopByte());
			else if (inEdit && msg.wParam == VK_RETURN)
				doSearch(hFrame, absoluteTopByte());
			else if (msg.wParam == VK_ESCAPE)
			{
				if (g_searching.load()) g_abortSearch.store(true);
				::SetFocus(g_hSci);
			}
			else if (inEdit)
				handled = false;
			else if (ctrl && msg.wParam == VK_HOME)   jumpToScrollPos(hFrame, 0);
			else if (ctrl && msg.wParam == VK_END)    jumpToScrollPos(hFrame, SCROLL_RANGE);
			else if (msg.wParam == VK_NEXT)  sci(SCI_LINESCROLL, 0, onScreen);
			else if (msg.wParam == VK_PRIOR) sci(SCI_LINESCROLL, 0, -onScreen);
			else if (msg.wParam == VK_DOWN)  sci(SCI_LINESCROLL, 0, 1);
			else if (msg.wParam == VK_UP)    sci(SCI_LINESCROLL, 0, -1);
			else handled = false;
			if (handled) continue;
		}
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);
	}

	// Stop and reap the background threads before any globals they touch go away.
	g_abortIndex.store(true);
	g_abortSearch.store(true);
	if (g_indexThread.joinable())  g_indexThread.join();
	if (g_searchThread.joinable()) g_searchThread.join();
	cleanup();
	return 0;
}

bool LargeFileViewer::offerForHugeFile(HWND parent, const wchar_t* filePath)
{
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!::GetFileAttributesExW(filePath, GetFileExInfoStandard, &fad))
		return false;
	if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return false;

	LARGE_INTEGER sz{};
	sz.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
	sz.LowPart  = fad.nFileSizeLow;
	if (sz.QuadPart < HUGE_THRESHOLD)
		return false; // small enough: let the normal load handle it

	if (g_viewerActive.load())
	{
		::MessageBoxW(parent,
			L"A Large File Viewer is already open.\nPlease close it before opening another huge file.",
			L"Large File Viewer", MB_OK | MB_ICONINFORMATION);
		return true; // handled: do not also load normally
	}

	wchar_t msg[600];
	swprintf(msg, 600,
		L"This file is %.2f GB.\n\n"
		L"Open it in the Large File Viewer (read-only, fixed low memory, instant open)?\n\n"
		L"Yes  -  Large File Viewer\n"
		L"No  -  Normal load (may be very slow or run out of memory)\n"
		L"Cancel  -  Don't open",
		sz.QuadPart / 1073741824.0);
	const int r = ::MessageBoxW(parent, msg, L"Large file", MB_YESNOCANCEL | MB_ICONQUESTION);
	if (r == IDCANCEL) return true;  // abort the open entirely
	if (r == IDNO)     return false; // user wants the normal load

	// Launch the viewer on its own UI thread so it never blocks Notepad++.
	auto* path = new std::wstring(filePath);
	g_viewerActive.store(true);
	std::thread([path]()
	{
		runStandalone(::GetModuleHandle(nullptr), path->c_str(), false);
		delete path;
		g_viewerActive.store(false);
	}).detach();
	return true;
}
