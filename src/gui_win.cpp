// vgz2flac - render chiptune logs (VGM/VGZ and friends) to high-quality FLAC.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Windows front end: one window with a file list (drop files or folders on
// it), output and format options, a progress bar and a Convert button.
// Conversions run on a worker thread, one file at a time.

#include "converter.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef VGZ2FLAC_VERSION
#define VGZ2FLAC_VERSION "dev"
#endif
#define VGZ2FLAC_WIDEN2(x) L##x
#define VGZ2FLAC_WIDEN(x) VGZ2FLAC_WIDEN2(x)

namespace fs = std::filesystem;

namespace
{

// ---------------------------------------------------------------------------
// ids and messages

enum : int
{
	IDC_HINT = 100,
	IDC_LIST,
	IDC_ADD_FILES,
	IDC_ADD_FOLDER,
	IDC_REMOVE,
	IDC_CLEAR,
	IDC_OUT_SAME,
	IDC_OUT_FOLDER,
	IDC_OUT_PATH,
	IDC_OUT_BROWSE,
	IDC_LOOPS_LABEL,
	IDC_LOOPS,
	IDC_FADE_LABEL,
	IDC_FADE,
	IDC_BITS_LABEL,
	IDC_BITS,
	IDC_OVERWRITE,
	IDC_PROGRESS,
	IDC_CONVERT,
};

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;	// wParam = item, lParam = permille
constexpr UINT WM_APP_RESULT = WM_APP + 2;		// wParam = item, lParam = ItemResult*
constexpr UINT WM_APP_FINISHED = WM_APP + 3;	// lParam = 1 if cancelled

enum class ItemState { Waiting, Working, Done, Skipped, Failed, Cancelled };

struct ItemResult
{
	ItemState state = ItemState::Failed;
	std::wstring text;
	fs::path output;
};

struct Item
{
	fs::path input;
	fs::path output;	// set once converted
	ItemState state = ItemState::Waiting;
};

// ---------------------------------------------------------------------------
// state

HINSTANCE g_inst = NULL;
HWND g_wnd = NULL;
HWND g_ctl[IDC_CONVERT + 1] = {};
HFONT g_font = NULL;
UINT g_dpi = 96;
std::vector<Item> g_items;
std::thread g_worker;
std::atomic<bool> g_cancel(false);
bool g_running = false;
unsigned g_runTotal = 0;	// files in the current run
unsigned g_runDone = 0;		// of those, finished

HWND Ctl(int id) { return g_ctl[id]; }
int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

std::wstring Widen(const std::string& s)
{
	if (s.empty())
		return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
	std::wstring w((size_t)len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], len);
	return w;
}

std::wstring GetText(HWND h)
{
	int len = GetWindowTextLengthW(h);
	std::wstring s((size_t)len + 1, L'\0');
	GetWindowTextW(h, &s[0], len + 1);
	s.resize((size_t)len);
	return s;
}

// ---------------------------------------------------------------------------
// list view

void SetItemStatus(int index, const std::wstring& text)
{
	LVITEMW it = {};
	it.iSubItem = 1;
	it.pszText = const_cast<LPWSTR>(text.c_str());
	SendMessageW(Ctl(IDC_LIST), LVM_SETITEMTEXTW, (WPARAM)index, (LPARAM)&it);
}

void AddPaths(const std::vector<fs::path>& paths)
{
	std::vector<std::string> problems;
	int added = 0;
	for (const fs::path& p : paths)
	{
		for (const fs::path& song : FindSongs(p, problems))
		{
			bool dup = false;
			for (const Item& it : g_items)
				if (it.input == song)
					dup = true;
			if (dup)
				continue;
			Item item;
			item.input = song;
			g_items.push_back(item);

			std::wstring name = song.filename().wstring();
			LVITEMW lv = {};
			lv.mask = LVIF_TEXT;
			lv.iItem = (int)g_items.size() - 1;
			lv.pszText = const_cast<LPWSTR>(name.c_str());
			SendMessageW(Ctl(IDC_LIST), LVM_INSERTITEMW, 0, (LPARAM)&lv);
			SetItemStatus(lv.iItem, L"Ready");
			added++;
		}
	}
	if (!problems.empty())
	{
		std::wstring msg = L"Some items were not added:\n\n";
		for (const std::string& p : problems)
			msg += Widen(p) + L"\n";
		MessageBoxW(g_wnd, msg.c_str(), L"vgz2flac", MB_ICONINFORMATION);
	}
	if (added > 0)
		SetWindowTextW(Ctl(IDC_HINT), L"Click Convert when ready. Double-click a finished file to show it in Explorer.");
}

void RemoveSelected()
{
	HWND list = Ctl(IDC_LIST);
	for (int i = (int)g_items.size() - 1; i >= 0; i--)
	{
		if (SendMessageW(list, LVM_GETITEMSTATE, (WPARAM)i, LVIS_SELECTED) & LVIS_SELECTED)
		{
			SendMessageW(list, LVM_DELETEITEM, (WPARAM)i, 0);
			g_items.erase(g_items.begin() + i);
		}
	}
}

void ClearList()
{
	SendMessageW(Ctl(IDC_LIST), LVM_DELETEALLITEMS, 0, 0);
	g_items.clear();
}

// ---------------------------------------------------------------------------
// file dialogs

std::vector<fs::path> PickPaths(bool folders)
{
	std::vector<fs::path> out;
	IFileOpenDialog* dlg = NULL;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
		return out;
	DWORD flags = 0;
	dlg->GetOptions(&flags);
	flags |= FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM;
	if (folders)
		flags |= FOS_PICKFOLDERS;
	dlg->SetOptions(flags);
	if (!folders)
	{
		COMDLG_FILTERSPEC filters[] = {
			{ L"Chip music rips (*.vgz, *.vgm, *.s98, *.dro, *.gym)", L"*.vgz;*.vgm;*.s98;*.dro;*.gym" },
			{ L"All files", L"*.*" },
		};
		dlg->SetFileTypes(2, filters);
		dlg->SetTitle(L"Add songs");
	}
	else
		dlg->SetTitle(L"Add folders (searched including subfolders)");

	if (SUCCEEDED(dlg->Show(g_wnd)))
	{
		IShellItemArray* items = NULL;
		if (SUCCEEDED(dlg->GetResults(&items)))
		{
			DWORD count = 0;
			items->GetCount(&count);
			for (DWORD i = 0; i < count; i++)
			{
				IShellItem* item = NULL;
				if (SUCCEEDED(items->GetItemAt(i, &item)))
				{
					PWSTR path = NULL;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
					{
						out.push_back(fs::path(path));
						CoTaskMemFree(path);
					}
					item->Release();
				}
			}
			items->Release();
		}
	}
	dlg->Release();
	return out;
}

bool PickOneFolder(fs::path& result)
{
	IFileOpenDialog* dlg = NULL;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
		return false;
	DWORD flags = 0;
	dlg->GetOptions(&flags);
	dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	dlg->SetTitle(L"Save the FLAC files in");
	bool ok = false;
	if (SUCCEEDED(dlg->Show(g_wnd)))
	{
		IShellItem* item = NULL;
		if (SUCCEEDED(dlg->GetResult(&item)))
		{
			PWSTR path = NULL;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
			{
				result = fs::path(path);
				CoTaskMemFree(path);
				ok = true;
			}
			item->Release();
		}
	}
	dlg->Release();
	return ok;
}

void ShowInExplorer(const fs::path& file)
{
	PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str());
	if (pidl != NULL)
	{
		SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
		ILFree(pidl);
	}
}

// ---------------------------------------------------------------------------
// converting

struct Job
{
	std::vector<fs::path> inputs;
	std::vector<int> listIndex;
	ConvertOptions opt;
	fs::path outDir;	// empty = next to each file
	bool overwrite = false;
};

void WorkerMain(Job job)
{
	for (size_t i = 0; i < job.inputs.size(); i++)
	{
		const int index = job.listIndex[i];
		auto* res = new ItemResult;
		if (g_cancel.load())
		{
			res->state = ItemState::Cancelled;
			res->text = L"Cancelled";
			PostMessageW(g_wnd, WM_APP_RESULT, (WPARAM)index, (LPARAM)res);
			continue;
		}

		const fs::path& in = job.inputs[i];
		fs::path out = (job.outDir.empty() ? in.parent_path() : job.outDir) / in.stem();
		out += ".flac";
		res->output = out;

		std::error_code ec;
		if (!job.overwrite && fs::exists(out, ec))
		{
			res->state = ItemState::Skipped;
			res->text = L"Skipped: " + out.filename().wstring() + L" already exists";
			PostMessageW(g_wnd, WM_APP_RESULT, (WPARAM)index, (LPARAM)res);
			continue;
		}

		PostMessageW(g_wnd, WM_APP_PROGRESS, (WPARAM)index, 0);
		int lastPermille = -1;
		ConvertResult r = ConvertFile(in, out, job.opt, [&](double f) -> bool
		{
			int pm = (int)(f * 1000.0);
			if (pm != lastPermille)
			{
				lastPermille = pm;
				PostMessageW(g_wnd, WM_APP_PROGRESS, (WPARAM)index, (LPARAM)pm);
			}
			return !g_cancel.load();
		});

		if (r.ok)
		{
			res->state = ItemState::Done;
			res->text = L"Done: " + (r.lines.empty() ? std::wstring() : Widen(r.lines[0]));
		}
		else if (g_cancel.load())
		{
			res->state = ItemState::Cancelled;
			res->text = L"Cancelled";
		}
		else
		{
			res->state = ItemState::Failed;
			res->text = L"Failed: " + (r.lines.empty() ? std::wstring(L"unknown error") : Widen(r.lines[0]));
		}
		PostMessageW(g_wnd, WM_APP_RESULT, (WPARAM)index, (LPARAM)res);
	}
	PostMessageW(g_wnd, WM_APP_FINISHED, 0, g_cancel.load() ? 1 : 0);
}

void SetRunning(bool running)
{
	g_running = running;
	const int lockIds[] = { IDC_ADD_FILES, IDC_ADD_FOLDER, IDC_REMOVE, IDC_CLEAR, IDC_OUT_SAME, IDC_OUT_FOLDER,
	                        IDC_OUT_BROWSE, IDC_LOOPS, IDC_FADE, IDC_BITS, IDC_OVERWRITE };
	for (int id : lockIds)
		EnableWindow(Ctl(id), !running);
	if (!running)
		EnableWindow(Ctl(IDC_OUT_BROWSE), SendMessageW(Ctl(IDC_OUT_FOLDER), BM_GETCHECK, 0, 0) == BST_CHECKED);
	SetWindowTextW(Ctl(IDC_CONVERT), running ? L"Cancel" : L"Convert");
	DragAcceptFiles(g_wnd, running ? FALSE : TRUE);
}

void StartConversion()
{
	if (g_items.empty())
	{
		MessageBoxW(g_wnd, L"Add some .vgz files first: drag them into the list, or use Add files / Add folder.",
		            L"vgz2flac", MB_ICONINFORMATION);
		return;
	}

	Job job;
	BOOL ok = FALSE;
	UINT loops = GetDlgItemInt(g_wnd, IDC_LOOPS, &ok, FALSE);
	if (!ok || loops < 1 || loops > 100)
	{
		MessageBoxW(g_wnd, L"Loops must be a whole number from 1 to 100.", L"vgz2flac", MB_ICONWARNING);
		return;
	}
	std::wstring fadeText = GetText(Ctl(IDC_FADE));
	wchar_t* end = NULL;
	double fade = wcstod(fadeText.c_str(), &end);
	if (fadeText.empty() || end == NULL || *end != L'\0' || fade < 0.0 || fade > 600.0)
	{
		MessageBoxW(g_wnd, L"Fade must be a number of seconds from 0 to 600.", L"vgz2flac", MB_ICONWARNING);
		return;
	}
	job.opt.loops = loops;
	job.opt.fadeSec = fade;
	job.opt.bits = SendMessageW(Ctl(IDC_BITS), CB_GETCURSEL, 0, 0) == 1 ? 16 : 24;
	job.overwrite = SendMessageW(Ctl(IDC_OVERWRITE), BM_GETCHECK, 0, 0) == BST_CHECKED;
	if (SendMessageW(Ctl(IDC_OUT_FOLDER), BM_GETCHECK, 0, 0) == BST_CHECKED)
	{
		std::wstring dir = GetText(Ctl(IDC_OUT_PATH));
		if (dir.empty())
		{
			MessageBoxW(g_wnd, L"Choose the output folder with Browse, or pick \"Next to each file\".",
			            L"vgz2flac", MB_ICONWARNING);
			return;
		}
		job.outDir = fs::path(dir);
		std::error_code ec;
		fs::create_directories(job.outDir, ec);
		if (ec)
		{
			MessageBoxW(g_wnd, (L"Cannot create the output folder:\n" + dir).c_str(), L"vgz2flac", MB_ICONWARNING);
			return;
		}
	}

	for (size_t i = 0; i < g_items.size(); i++)
	{
		if (g_items[i].state == ItemState::Done)
			continue;	// already converted in an earlier run
		job.inputs.push_back(g_items[i].input);
		job.listIndex.push_back((int)i);
		g_items[i].state = ItemState::Waiting;
		SetItemStatus((int)i, L"Waiting");
	}
	if (job.inputs.empty())
	{
		MessageBoxW(g_wnd, L"Every file in the list is already converted.", L"vgz2flac", MB_ICONINFORMATION);
		return;
	}

	g_runTotal = (unsigned)job.inputs.size();
	g_runDone = 0;
	SendMessageW(Ctl(IDC_PROGRESS), PBM_SETRANGE32, 0, (LPARAM)(g_runTotal * 1000));
	SendMessageW(Ctl(IDC_PROGRESS), PBM_SETPOS, 0, 0);
	SetWindowTextW(Ctl(IDC_HINT), L"Converting...");
	g_cancel.store(false);
	SetRunning(true);
	if (g_worker.joinable())
		g_worker.join();
	g_worker = std::thread(WorkerMain, std::move(job));
}

// ---------------------------------------------------------------------------
// layout

void ApplyFont()
{
	NONCLIENTMETRICSW ncm = {};
	ncm.cbSize = sizeof(ncm);
	SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
	HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
	for (HWND h : g_ctl)
		if (h != NULL)
			SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
	if (g_font != NULL)
		DeleteObject(g_font);
	g_font = font;
}

void Layout()
{
	RECT rc;
	GetClientRect(g_wnd, &rc);
	const int w = rc.right, h = rc.bottom;
	const int m = S(12), gap = S(8), btnH = S(28), rowH = S(24);

	auto place = [](int id, int x, int y, int cx, int cy)
	{
		SetWindowPos(Ctl(id), NULL, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
	};

	// bottom: progress + convert
	const int yConvert = h - m - btnH;
	place(IDC_CONVERT, w - m - S(110), yConvert, S(110), btnH);
	place(IDC_PROGRESS, m, yConvert + S(4), w - 2 * m - S(110) - gap, btnH - S(8));

	// options row
	const int yOpt = yConvert - gap - rowH - S(4);
	int x = m;
	place(IDC_LOOPS_LABEL, x, yOpt + S(4), S(42), rowH); x += S(42);
	place(IDC_LOOPS, x, yOpt, S(40), rowH); x += S(40) + S(16);
	place(IDC_FADE_LABEL, x, yOpt + S(4), S(80), rowH); x += S(80);
	place(IDC_FADE, x, yOpt, S(44), rowH); x += S(44) + S(16);
	place(IDC_BITS_LABEL, x, yOpt + S(4), S(52), rowH); x += S(52);
	place(IDC_BITS, x, yOpt, S(70), S(200)); x += S(70) + S(16);
	place(IDC_OVERWRITE, x, yOpt + S(2), S(200), rowH);

	// output row
	const int yOut = yOpt - gap - rowH;
	x = m;
	place(IDC_OUT_SAME, x, yOut + S(2), S(150), rowH); x += S(150);
	place(IDC_OUT_FOLDER, x, yOut + S(2), S(70), rowH); x += S(70);
	place(IDC_OUT_BROWSE, w - m - S(90), yOut - S(2), S(90), btnH);
	place(IDC_OUT_PATH, x, yOut, w - m - S(90) - gap - x, rowH);

	// list buttons
	const int yBtn = yOut - gap - btnH - S(4);
	x = m;
	place(IDC_ADD_FILES, x, yBtn, S(100), btnH); x += S(100) + gap;
	place(IDC_ADD_FOLDER, x, yBtn, S(100), btnH); x += S(100) + gap;
	place(IDC_REMOVE, x, yBtn, S(80), btnH); x += S(80) + gap;
	place(IDC_CLEAR, x, yBtn, S(80), btnH);

	// hint + list
	place(IDC_HINT, m, m, w - 2 * m, S(20));
	const int yList = m + S(24);
	place(IDC_LIST, m, yList, w - 2 * m, std::max(S(60), yBtn - gap - yList));

	// list columns: name gets what the status column doesn't need
	const int listW = w - 2 * m - GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi) - S(4);
	const int nameW = std::max(S(160), listW * 2 / 5);
	SendMessageW(Ctl(IDC_LIST), LVM_SETCOLUMNWIDTH, 0, nameW);
	SendMessageW(Ctl(IDC_LIST), LVM_SETCOLUMNWIDTH, 1, std::max(S(160), listW - nameW));
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0)
{
	HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd,
	                         (HMENU)(INT_PTR)id, g_inst, NULL);
	g_ctl[id] = h;
	return h;
}

void CreateControls()
{
	Make(L"STATIC", L"Drag .vgz files or folders into this list, or use Add files / Add folder.", SS_LEFT | SS_NOPREFIX,
	     IDC_HINT);

	HWND list = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST, WS_EX_CLIENTEDGE);
	SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	LVCOLUMNW col = {};
	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.cx = 200;
	col.pszText = const_cast<LPWSTR>(L"File");
	SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
	col.pszText = const_cast<LPWSTR>(L"Status");
	SendMessageW(list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

	Make(L"BUTTON", L"Add files...", BS_PUSHBUTTON | WS_TABSTOP, IDC_ADD_FILES);
	Make(L"BUTTON", L"Add folder...", BS_PUSHBUTTON | WS_TABSTOP, IDC_ADD_FOLDER);
	Make(L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, IDC_REMOVE);
	Make(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLEAR);

	Make(L"BUTTON", L"Save next to each file", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, IDC_OUT_SAME);
	Make(L"BUTTON", L"Save in:", BS_AUTORADIOBUTTON, IDC_OUT_FOLDER);
	Make(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY, IDC_OUT_PATH, WS_EX_CLIENTEDGE);
	Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, IDC_OUT_BROWSE);
	SendMessageW(Ctl(IDC_OUT_SAME), BM_SETCHECK, BST_CHECKED, 0);
	EnableWindow(Ctl(IDC_OUT_BROWSE), FALSE);

	Make(L"STATIC", L"Loops:", SS_LEFT, IDC_LOOPS_LABEL);
	Make(L"EDIT", L"2", ES_NUMBER | ES_CENTER | WS_TABSTOP | WS_GROUP, IDC_LOOPS, WS_EX_CLIENTEDGE);
	Make(L"STATIC", L"Fade (sec):", SS_LEFT, IDC_FADE_LABEL);
	Make(L"EDIT", L"8", ES_CENTER | WS_TABSTOP, IDC_FADE, WS_EX_CLIENTEDGE);
	Make(L"STATIC", L"Bit depth:", SS_LEFT, IDC_BITS_LABEL);
	HWND bits = Make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_BITS);
	SendMessageW(bits, CB_ADDSTRING, 0, (LPARAM)L"24-bit");
	SendMessageW(bits, CB_ADDSTRING, 0, (LPARAM)L"16-bit");
	SendMessageW(bits, CB_SETCURSEL, 0, 0);
	Make(L"BUTTON", L"Replace existing FLAC files", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_OVERWRITE);

	Make(PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS);
	Make(L"BUTTON", L"Convert", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_CONVERT);
}

// ---------------------------------------------------------------------------
// window procedure

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CREATE:
		g_wnd = hwnd;
		g_dpi = GetDpiForWindow(hwnd);
		CreateControls();
		ApplyFont();
		DragAcceptFiles(hwnd, TRUE);
		return 0;

	case WM_SIZE:
		Layout();
		return 0;

	case WM_GETMINMAXINFO:
	{
		MINMAXINFO* mmi = (MINMAXINFO*)lp;
		mmi->ptMinTrackSize.x = S(720);
		mmi->ptMinTrackSize.y = S(380);
		return 0;
	}

	case WM_DPICHANGED:
	{
		g_dpi = HIWORD(wp);
		ApplyFont();
		const RECT* r = (const RECT*)lp;
		SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		Layout();
		return 0;
	}

	case WM_DROPFILES:
	{
		HDROP drop = (HDROP)wp;
		std::vector<fs::path> paths;
		UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
		for (UINT i = 0; i < count; i++)
		{
			UINT len = DragQueryFileW(drop, i, NULL, 0);
			std::wstring p((size_t)len + 1, L'\0');
			DragQueryFileW(drop, i, &p[0], len + 1);
			p.resize(len);
			paths.push_back(fs::path(p));
		}
		DragFinish(drop);
		if (!g_running)
			AddPaths(paths);
		return 0;
	}

	case WM_COMMAND:
	{
		const int id = LOWORD(wp);
		switch (id)
		{
		case IDC_ADD_FILES: AddPaths(PickPaths(false)); break;
		case IDC_ADD_FOLDER: AddPaths(PickPaths(true)); break;
		case IDC_REMOVE: RemoveSelected(); break;
		case IDC_CLEAR: ClearList(); break;
		case IDC_OUT_SAME:
		case IDC_OUT_FOLDER:
		{
			bool folder = SendMessageW(Ctl(IDC_OUT_FOLDER), BM_GETCHECK, 0, 0) == BST_CHECKED;
			EnableWindow(Ctl(IDC_OUT_BROWSE), folder);
			if (folder && GetWindowTextLengthW(Ctl(IDC_OUT_PATH)) == 0)
				PostMessageW(hwnd, WM_COMMAND, IDC_OUT_BROWSE, 0);
			break;
		}
		case IDC_OUT_BROWSE:
		{
			fs::path dir;
			if (PickOneFolder(dir))
			{
				SetWindowTextW(Ctl(IDC_OUT_PATH), dir.c_str());
				SendMessageW(Ctl(IDC_OUT_FOLDER), BM_SETCHECK, BST_CHECKED, 0);
				SendMessageW(Ctl(IDC_OUT_SAME), BM_SETCHECK, BST_UNCHECKED, 0);
				EnableWindow(Ctl(IDC_OUT_BROWSE), TRUE);
			}
			else if (GetWindowTextLengthW(Ctl(IDC_OUT_PATH)) == 0)
			{
				SendMessageW(Ctl(IDC_OUT_SAME), BM_SETCHECK, BST_CHECKED, 0);
				SendMessageW(Ctl(IDC_OUT_FOLDER), BM_SETCHECK, BST_UNCHECKED, 0);
				EnableWindow(Ctl(IDC_OUT_BROWSE), FALSE);
			}
			break;
		}
		case IDC_CONVERT:
			if (g_running)
			{
				g_cancel.store(true);
				EnableWindow(Ctl(IDC_CONVERT), FALSE);
				SetWindowTextW(Ctl(IDC_HINT), L"Cancelling...");
			}
			else
				StartConversion();
			break;
		}
		return 0;
	}

	case WM_NOTIFY:
	{
		const NMHDR* hdr = (const NMHDR*)lp;
		if (hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK)
		{
			const NMITEMACTIVATE* act = (const NMITEMACTIVATE*)lp;
			if (act->iItem >= 0 && act->iItem < (int)g_items.size())
			{
				const Item& it = g_items[(size_t)act->iItem];
				std::error_code ec;
				if (!it.output.empty() && fs::exists(it.output, ec))
					ShowInExplorer(it.output);
				else
					ShowInExplorer(it.input);
			}
		}
		else if (hdr->idFrom == IDC_LIST && hdr->code == LVN_KEYDOWN && !g_running)
		{
			if (((const NMLVKEYDOWN*)lp)->wVKey == VK_DELETE)
				RemoveSelected();
		}
		return 0;
	}

	case WM_APP_PROGRESS:
	{
		const int index = (int)wp;
		const int pm = (int)lp;
		if (index >= 0 && index < (int)g_items.size())
		{
			g_items[(size_t)index].state = ItemState::Working;
			wchar_t buf[64];
			swprintf(buf, 64, L"Converting... %d%%", pm / 10);
			SetItemStatus(index, buf);
			SendMessageW(Ctl(IDC_LIST), LVM_ENSUREVISIBLE, (WPARAM)index, FALSE);
		}
		// overall bar: files finished in this run + fraction of the current one
		SendMessageW(Ctl(IDC_PROGRESS), PBM_SETPOS, (WPARAM)(g_runDone * 1000 + pm), 0);
		return 0;
	}

	case WM_APP_RESULT:
	{
		std::unique_ptr<ItemResult> res((ItemResult*)lp);
		const int index = (int)wp;
		if (index >= 0 && index < (int)g_items.size())
		{
			Item& it = g_items[(size_t)index];
			it.state = res->state;
			if (res->state == ItemState::Done)
				it.output = res->output;
			SetItemStatus(index, res->text);
		}
		g_runDone++;
		SendMessageW(Ctl(IDC_PROGRESS), PBM_SETPOS, (WPARAM)(g_runDone * 1000), 0);
		return 0;
	}

	case WM_APP_FINISHED:
	{
		if (g_worker.joinable())
			g_worker.join();
		EnableWindow(Ctl(IDC_CONVERT), TRUE);
		SetRunning(false);
		unsigned done = 0, failed = 0, skipped = 0;
		for (const Item& it : g_items)
		{
			if (it.state == ItemState::Done) done++;
			else if (it.state == ItemState::Failed) failed++;
			else if (it.state == ItemState::Skipped) skipped++;
		}
		wchar_t buf[256];
		swprintf(buf, 256, L"%ls %u converted, %u skipped, %u failed. Double-click a file to show it in Explorer.",
		         lp ? L"Cancelled." : L"Finished:", done, skipped, failed);
		SetWindowTextW(Ctl(IDC_HINT), buf);
		FLASHWINFO fw = { sizeof(fw), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
		FlashWindowEx(&fw);
		return 0;
	}

	case WM_CLOSE:
		if (g_running)
		{
			if (MessageBoxW(hwnd, L"A conversion is still running. Stop it and close?", L"vgz2flac",
			                MB_YESNO | MB_ICONQUESTION) != IDYES)
				return 0;
			g_cancel.store(true);
			if (g_worker.joinable())
				g_worker.join();
		}
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		if (g_font != NULL)
			DeleteObject(g_font);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
	g_inst = inst;
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"vgz2flacWindow";
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	if (wc.hIcon == NULL)
		wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
	wc.hIconSm = wc.hIcon;
	RegisterClassExW(&wc);

	const UINT dpi = GetDpiForSystem();
	const std::wstring title = L"vgz2flac " VGZ2FLAC_WIDEN(VGZ2FLAC_VERSION) L" - chip music to FLAC";
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                            MulDiv(820, (int)dpi, 96), MulDiv(520, (int)dpi, 96), NULL, NULL, inst, NULL);
	if (hwnd == NULL)
		return 1;

	// files dropped onto the .exe arrive as arguments
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv != NULL)
	{
		std::vector<fs::path> paths;
		for (int i = 1; i < argc; i++)
			paths.push_back(fs::path(argv[i]));
		LocalFree(argv);
		if (!paths.empty())
			AddPaths(paths);
	}

	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0)
	{
		if (!IsDialogMessageW(hwnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	CoUninitialize();
	return (int)msg.wParam;
}
