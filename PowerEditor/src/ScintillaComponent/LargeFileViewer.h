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

#pragma once

#include <windows.h>

// -----------------------------------------------------------------------------
// LargeFileViewer  (proof-of-concept for issue: huge-file performance)
//
// Normal Notepad++ loads a whole file into one contiguous Scintilla buffer, so
// RAM grows ~linearly with file size (a 10 GB file needs ~11 GB RAM). This viewer
// instead reads only the currently-visible slice (a fixed window) from disk via
// FILE_FLAG_NO_BUFFERING ReadFile and feeds just that slice to a Scintilla control.
// Peak memory is therefore fixed (~tens of MB) regardless of file size.
//
// It is intentionally self-contained: it uses the raw Scintilla control directly
// and does NOT touch NppParameters / NppDarkMode / docking, so it can run before
// (and independently of) normal Notepad++ startup. That keeps it low-risk and easy
// to benchmark against the native open path.
//
// Triggered two ways:
//   * command line:  notepad++.exe -largefilepoc "C:\path\to\huge.log"
//   * automatically: opening a file above the size threshold in Notepad++ offers
//     this viewer (see offerForHugeFile, called from Notepad_plus::doOpen).
//
// Navigation: wheel / PgUp / PgDn / arrows scroll; Ctrl+Home / Ctrl+End jump to
//             file start / end; Ctrl+F / Enter / F3 search.
// -----------------------------------------------------------------------------
class LargeFileViewer
{
public:
	// Runs a self-contained viewer with its own top-level window and message
	// loop. Returns the process exit code. hInst may be GetModuleHandle(NULL).
	// benchMode: headless timing run - measure load time + peak working set,
	// write %TEMP%\lfv_bench.json, and exit without showing a window.
	static int runStandalone(HINSTANCE hInst, const wchar_t* filePath, bool benchMode = false);

	// Called from the normal open path. If 'filePath' is at/above the huge-file
	// threshold, asks the user whether to open it in this low-memory viewer; if so,
	// launches the viewer on its own thread and returns true (caller should NOT
	// continue the normal load). Returns false to let the normal load proceed.
	static bool offerForHugeFile(HWND parent, const wchar_t* filePath);
};
