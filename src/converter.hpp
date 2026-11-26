// vgz2flac - render chiptune logs (VGM/VGZ and friends) to high-quality FLAC.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct ConvertOptions
{
	uint32_t outRate = 48000;	// output sample rate (Hz)
	unsigned bits = 24;			// output bit depth (16 or 24)
	unsigned loops = 2;			// times the looping part is played before the fade
	double fadeSec = 8.0;		// fade-out length for looping songs (seconds)
	bool preferNuked = true;	// use the Nuked (die-shot accurate) cores where libvgm has one
	bool splitByRate = true;	// render each chip at its native rate, then resample with SoX VHQ
	unsigned compression = 8;	// FLAC compression level (0-8, lossless either way)
};

struct ConvertResult
{
	bool ok = false;
	std::vector<std::string> lines;	// human-readable report (summary, chips, warnings or the error)
};

// Called now and then with the fraction done (0..1). Return false to cancel.
using ConvertProgress = std::function<bool(double fraction)>;

// Converts one file. 'outFile' is written via a temporary ".part" file and only
// appears once the FLAC has been fully written and verified by the encoder.
ConvertResult ConvertFile(const std::filesystem::path& inFile,
                          const std::filesystem::path& outFile,
                          const ConvertOptions& opt,
                          const ConvertProgress& progress = ConvertProgress());

// File extensions the converter understands (lower case, with the dot).
bool HasSongExtension(const std::filesystem::path& p);

// A file is returned as is; a folder is searched recursively for song files
// (sorted). Problems (missing paths, empty folders) are appended to 'problems'.
std::vector<std::filesystem::path> FindSongs(const std::filesystem::path& in, std::vector<std::string>& problems);
