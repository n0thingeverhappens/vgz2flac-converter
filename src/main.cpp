// vgz2flac - render chiptune logs (VGM/VGZ and friends) to high-quality FLAC.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Usage: drag .vgz/.vgm files or whole folders onto vgz2flac.exe, or run
//   vgz2flac [options] <file or folder>...
// Each song is written as <name>.flac next to the original (or into --out).

#include "converter.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef VGZ2FLAC_VERSION
#define VGZ2FLAC_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace
{

void PrintUsage()
{
	std::printf(
		"vgz2flac " VGZ2FLAC_VERSION " - turns VGM/VGZ chip-music rips into high-quality FLAC files\n"
		"\n"
		"Usage: vgz2flac [options] <file or folder> [more files or folders...]\n"
		"       (or drag files/folders onto vgz2flac.exe)\n"
		"\n"
		"Folders are searched recursively for .vgz .vgm .s98 .dro .gym files.\n"
		"Each song becomes <name>.flac next to the original.\n"
		"\n"
		"Options:\n"
		"  -o, --out <folder>   write the FLAC files into this folder instead\n"
		"  --rate <hz>          output sample rate (default 48000)\n"
		"  --bits <16|24>       output bit depth (default 24)\n"
		"  --loops <n>          times the looping part plays before the fade (default 2)\n"
		"  --fade <seconds>     fade-out length for looping songs (default 8)\n"
		"  --overwrite          replace FLAC files that already exist (default: skip them)\n"
		"  --default-cores      use libvgm's default cores instead of the Nuked ones\n"
		"  --single-pass        mix all chips inside libvgm (its simple resampler) instead of\n"
		"                       rendering each chip at its native rate and resampling with SoX\n"
		"  -h, --help           show this text\n");
}

struct Args
{
	ConvertOptions opt;
	fs::path outDir;
	bool overwrite = false;
	bool help = false;
	std::vector<fs::path> inputs;
	std::string error;
};

bool ParseUnsigned(const std::string& s, unsigned long& out)
{
	if (s.empty())
		return false;
	char* end = NULL;
	out = std::strtoul(s.c_str(), &end, 10);
	return end != NULL && *end == '\0';
}

bool ParseDouble(const std::string& s, double& out)
{
	if (s.empty())
		return false;
	char* end = NULL;
	out = std::strtod(s.c_str(), &end);
	return end != NULL && *end == '\0';
}

Args ParseArgs(const std::vector<std::string>& argv)
{
	Args a;
	for (size_t i = 0; i < argv.size(); i++)
	{
		const std::string& arg = argv[i];
		auto value = [&](std::string& out) -> bool
		{
			if (i + 1 >= argv.size())
			{
				a.error = arg + " needs a value";
				return false;
			}
			out = argv[++i];
			return true;
		};
		std::string v;
		if (arg == "-h" || arg == "--help" || arg == "/?")
			a.help = true;
		else if (arg == "-o" || arg == "--out")
		{
			if (!value(v)) return a;
			a.outDir = fs::u8path(v);
		}
		else if (arg == "--rate")
		{
			unsigned long n = 0;
			if (!value(v)) return a;
			if (!ParseUnsigned(v, n) || n < 8000 || n > 384000)
			{
				a.error = "--rate must be between 8000 and 384000";
				return a;
			}
			a.opt.outRate = (uint32_t)n;
		}
		else if (arg == "--bits")
		{
			unsigned long n = 0;
			if (!value(v)) return a;
			if (!ParseUnsigned(v, n) || (n != 16 && n != 24))
			{
				a.error = "--bits must be 16 or 24";
				return a;
			}
			a.opt.bits = (unsigned)n;
		}
		else if (arg == "--loops")
		{
			unsigned long n = 0;
			if (!value(v)) return a;
			if (!ParseUnsigned(v, n) || n < 1 || n > 100)
			{
				a.error = "--loops must be between 1 and 100";
				return a;
			}
			a.opt.loops = (unsigned)n;
		}
		else if (arg == "--fade")
		{
			double d = 0.0;
			if (!value(v)) return a;
			if (!ParseDouble(v, d) || d < 0.0 || d > 600.0)
			{
				a.error = "--fade must be between 0 and 600 seconds";
				return a;
			}
			a.opt.fadeSec = d;
		}
		else if (arg == "--overwrite")
			a.overwrite = true;
		else if (arg == "--default-cores")
			a.opt.preferNuked = false;
		else if (arg == "--single-pass")
			a.opt.splitByRate = false;
		else if (arg.size() > 1 && arg[0] == '-' && arg != "-")
		{
			a.error = "unknown option " + arg;
			return a;
		}
		else
			a.inputs.push_back(fs::u8path(arg));
	}
	return a;
}

std::vector<fs::path> CollectSongs(const std::vector<fs::path>& inputs, std::vector<std::string>& problems)
{
	std::vector<fs::path> songs;
	for (const fs::path& in : inputs)
	{
		std::vector<fs::path> found = FindSongs(in, problems);
		songs.insert(songs.end(), found.begin(), found.end());
	}
	return songs;
}

#ifdef _WIN32
std::string WideToUtf8(const wchar_t* w)
{
	int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (len <= 1)
		return std::string();
	std::string s((size_t)len - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, NULL, NULL);
	return s;
}

// True when Explorer started us (drag-and-drop / double-click): then the
// console window would vanish before anyone can read the report.
bool OwnsConsoleWindow()
{
	DWORD ids[2];
	return GetConsoleProcessList(ids, 2) <= 1;
}
#endif

int Run(const std::vector<std::string>& argv)
{
	Args args = ParseArgs(argv);
	if (!args.error.empty())
	{
		std::fprintf(stderr, "vgz2flac: %s (see --help)\n", args.error.c_str());
		return 2;
	}
	if (args.help || args.inputs.empty())
	{
		PrintUsage();
		return args.help ? 0 : 2;
	}

	std::vector<std::string> problems;
	std::vector<fs::path> songs = CollectSongs(args.inputs, problems);
	for (const std::string& p : problems)
		std::printf("%s\n", p.c_str());

	if (!args.outDir.empty())
	{
		std::error_code ec;
		fs::create_directories(args.outDir, ec);
		if (ec)
		{
			std::fprintf(stderr, "vgz2flac: cannot create %s: %s\n", args.outDir.u8string().c_str(), ec.message().c_str());
			return 1;
		}
	}

	unsigned converted = 0, skipped = 0, failed = (unsigned)problems.size();
	for (size_t i = 0; i < songs.size(); i++)
	{
		const fs::path& song = songs[i];
		fs::path out = (args.outDir.empty() ? song.parent_path() : args.outDir) / song.stem();
		out += ".flac";

		std::printf("[%zu/%zu] %s\n", i + 1, songs.size(), song.filename().u8string().c_str());
		std::fflush(stdout);

		std::error_code ec;
		if (!args.overwrite && fs::exists(out, ec))
		{
			std::printf("  skipped: %s already exists (use --overwrite to replace it)\n", out.filename().u8string().c_str());
			skipped++;
			continue;
		}

		ConvertResult r = ConvertFile(song, out, args.opt);
		if (r.ok)
		{
			converted++;
			std::printf("  -> %s\n", out.u8string().c_str());
			for (const std::string& line : r.lines)
				std::printf("  %s\n", line.c_str());
		}
		else
		{
			failed++;
			for (const std::string& line : r.lines)
				std::printf("  FAILED: %s\n", line.c_str());
		}
		std::fflush(stdout);
	}

	std::printf("\nDone: %u converted, %u skipped, %u failed.\n", converted, skipped, failed);
	return failed ? 1 : 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv)
{
	SetConsoleOutputCP(CP_UTF8);
	std::vector<std::string> argv;
	for (int i = 1; i < argc; i++)
		argv.push_back(WideToUtf8(wargv[i]));

	int ret = Run(argv);

	if (OwnsConsoleWindow())
	{
		std::printf("\nPress Enter to close this window.");
		std::fflush(stdout);
		std::getchar();
	}
	return ret;
}
#else
int main(int argc, char** argv)
{
	std::vector<std::string> args(argv + 1, argv + argc);
	return Run(args);
}
#endif
