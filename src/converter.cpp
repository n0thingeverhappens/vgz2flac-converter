// vgz2flac - render chiptune logs (VGM/VGZ and friends) to high-quality FLAC.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// How a file is rendered:
//  1. A probe run starts the song once to learn which chips it uses and the
//     native sample rate of each chip's emulation core (Nuked cores preferred).
//  2. Chips are grouped by native rate. Each group is rendered in its own run
//     with libvgm's output rate set to exactly that native rate, so libvgm's
//     built-in (linear) resampler is never used: the chip output is copied 1:1.
//     All other chips are switched off for that run.
//  3. Each group is converted to the output rate with libsoxr at "very high
//     quality" (linear phase) and summed in double precision. libvgm's mixer
//     simply adds the chips together at fixed volumes, so the sum of the
//     groups is the same mix VGMPlay would produce, minus its resampling.
//  4. The loop fade, the clipping guard and the rounding to 24 (or 16) bits
//     are done once, at the end, and the result is encoded with libFLAC with
//     verification on.

#include "converter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "player/playerbase.hpp"
#include "player/vgmplayer.hpp"
#include "player/s98player.hpp"
#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "utils/DataLoader.h"
#include "utils/FileLoader.h"
#include "emu/EmuCores.h"
#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

#include <soxr.h>
#include <FLAC/format.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#ifndef VGZ2FLAC_VERSION
#define VGZ2FLAC_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace
{

constexpr double kLibvgmFullScale = 8388608.0;	// libvgm renders at 24-bit scale (0x800000 = full scale)
constexpr UINT32 kChunkFrames = 4096;

// ---------------------------------------------------------------------------
// small helpers

std::string FccToString(UINT32 fcc)
{
	std::string s;
	for (int shift = 24; shift >= 0; shift -= 8)
	{
		char c = (char)((fcc >> shift) & 0xFF);
		if (c != '\0')
			s += c;
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '_'))
		s.pop_back();
	return s.empty() ? std::string("default") : s;
}

std::string CoreName(UINT32 fcc)
{
	switch (fcc)
	{
	case FCC_NUKE: return "Nuked";
	case FCC_MAME: return "MAME";
	case FCC_GPGX: return "Genesis Plus GX";
	case FCC_MAXM: return "Maxim";
	case FCC_EMU_: return "EMU2413/EMU2149";
	case FCC_GENS: return "Gens";
	case FCC_ADLE: return "AdLibEmu";
	case FCC_NSFP: return "NSFPlay";
	case FCC_OOTK: return "Ootake";
	case FCC_SBOY: return "SameBoy";
	default: return FccToString(fcc);
	}
}

std::string FormatTime(double sec)
{
	if (sec < 0.0)
		sec = 0.0;
	unsigned total = (unsigned)sec;
	unsigned tenths = (unsigned)((sec - total) * 10.0);
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%u:%02u.%u", total / 60, total % 60, tenths);
	return buf;
}

std::string FormatDb(double linear)
{
	char buf[32];
	if (linear <= 0.0)
		std::snprintf(buf, sizeof(buf), "-inf dBFS");
	else
		std::snprintf(buf, sizeof(buf), "%.1f dBFS", 20.0 * std::log10(linear));
	return buf;
}

std::string Trim(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
		s.pop_back();
	size_t start = 0;
	while (start < s.size() && s[start] == ' ')
		start++;
	return s.substr(start);
}

// ---------------------------------------------------------------------------
// libvgm callbacks

struct LogSink
{
	std::set<std::string> messages;
};

void LogCallback(void* userParam, PlayerBase* /*player*/, UINT8 level, UINT8 /*srcType*/,
                 const char* srcTag, const char* message)
{
	if (userParam == NULL || message == NULL || level == PLRLOG_OFF || level > PLRLOG_WARN)
		return;
	std::string m = Trim(message);
	if (m.empty())
		return;
	if (srcTag != NULL && *srcTag != '\0')
		m = std::string(srcTag) + ": " + m;
	m = std::string(level == PLRLOG_ERROR ? "libvgm error: " : "libvgm warning: ") + m;
	static_cast<LogSink*>(userParam)->messages.insert(m);
}

DATA_LOADER* OpenLoader(const fs::path& path, UINT32 preloadBytes)
{
#ifdef _WIN32
	DATA_LOADER* dl = FileLoader_InitW(path.wstring().c_str());
#else
	DATA_LOADER* dl = FileLoader_Init(path.string().c_str());
#endif
	if (dl == NULL)
		return NULL;
	if (preloadBytes)
		DataLoader_SetPreloadBytes(dl, preloadBytes);
	if (DataLoader_Load(dl))
	{
		DataLoader_Deinit(dl);
		return NULL;
	}
	return dl;
}

// Some files need extra data files (e.g. the OPL4 sample ROM "yrw801.rom").
// They are looked up as given, next to the song, and next to the executable.
struct FileRequestContext
{
	std::vector<fs::path> searchDirs;
};

DATA_LOADER* FileRequestCallback(void* userParam, PlayerBase* /*player*/, const char* fileName)
{
	if (userParam == NULL || fileName == NULL || *fileName == '\0')
		return NULL;
	const FileRequestContext* ctx = static_cast<const FileRequestContext*>(userParam);
	const fs::path name = fs::u8path(fileName);

	std::vector<fs::path> candidates;
	candidates.push_back(name);
	for (const fs::path& dir : ctx->searchDirs)
		candidates.push_back(dir / name.filename());

	for (const fs::path& candidate : candidates)
	{
		std::error_code ec;
		if (!fs::is_regular_file(candidate, ec))
			continue;
		DATA_LOADER* dl = OpenLoader(candidate, 0);
		if (dl != NULL)
			return dl;
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// One loaded song. VGMPlayer forgets its chip list on Stop(), so every render
// run uses a fresh Song.

class Song
{
public:
	Song() = default;
	Song(const Song&) = delete;
	Song& operator=(const Song&) = delete;
	~Song() { Close(); }

	bool Open(const fs::path& path, FileRequestContext* req, LogSink* log, std::string& err)
	{
		_loader = OpenLoader(path, 0x100);
		if (_loader == NULL)
		{
			err = "could not open or decompress the file";
			return false;
		}

		_engines.emplace_back(new VGMPlayer);
		_engines.emplace_back(new S98Player);
		_engines.emplace_back(new DROPlayer);
		_engines.emplace_back(new GYMPlayer);

		PlayerBase* found = NULL;
		for (auto& engine : _engines)
		{
			if (engine->CanLoadFile(_loader) == 0x00)
			{
				found = engine.get();
				break;
			}
		}
		if (found == NULL)
		{
			err = "not a VGM/VGZ, S98, DRO or GYM file";
			return false;
		}

		found->SetLogCallback(LogCallback, log);
		found->SetFileReqCallback(FileRequestCallback, req);
		found->SetSampleRate(48000);	// placeholder; every run sets its real rate before Start()
		UINT8 ret = found->LoadFile(_loader);
		if (ret >= 0x80)
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), "libvgm could not read the file (error 0x%02X)", ret);
			err = buf;
			return false;
		}
		_player = found;
		return true;
	}

	PlayerBase* Player() const { return _player; }

	void Close()
	{
		if (_player != NULL)
		{
			_player->Stop();
			_player->UnloadFile();
			_player = NULL;
		}
		_engines.clear();
		if (_loader != NULL)
		{
			DataLoader_Deinit(_loader);
			_loader = NULL;
		}
	}

private:
	DATA_LOADER* _loader = NULL;
	std::vector<std::unique_ptr<PlayerBase>> _engines;
	PlayerBase* _player = NULL;
};

// ---------------------------------------------------------------------------
// Chip bookkeeping

struct ChipInfo
{
	DEV_ID type = 0;
	UINT16 instance = 0;			// top-level: chip instance; linked: index below its parent
	int parent = -1;				// index into the chip list, -1 for top-level chips
	UINT32 rate = 0;				// native rate of the emulation core (0 = not running)
	UINT32 core = 0;				// FCC of the core in use
	std::string name;
};

std::vector<ChipInfo> ReadChips(const PlayerBase* player)
{
	std::vector<PLR_DEV_INFO> devs;
	player->GetSongDeviceInfo(devs);

	std::vector<ChipInfo> chips;
	for (const PLR_DEV_INFO& d : devs)
	{
		ChipInfo c;
		c.type = d.type;
		c.instance = d.instance;
		c.parent = (d.parentIdx == (UINT32)-1) ? -1 : (int)d.parentIdx;
		c.rate = d.smplRate;
		c.core = d.core;
		const char* name = NULL;
		if (d.devDecl != NULL && d.devDecl->name != NULL && d.devCfg != NULL)
			name = d.devDecl->name(d.devCfg);
		if (name != NULL && *name != '\0')
			c.name = name;
		else
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "chip 0x%02X", (unsigned)d.type);
			c.name = buf;
		}
		chips.push_back(c);
	}
	return chips;
}

// Chooses the cores and (optionally) which chips are audible for one run.
// 'audibleRate' = 0 keeps every chip audible.
// Returns false if libvgm refused the options for a chip (then chips can't be
// switched off individually and the caller falls back to a single run).
bool ConfigureChips(PlayerBase* player, const std::vector<ChipInfo>& chips, bool preferNuked, UINT32 audibleRate)
{
	bool allOk = true;
	for (size_t i = 0; i < chips.size(); i++)
	{
		const ChipInfo& top = chips[i];
		if (top.parent >= 0)
			continue;

		const UINT32 id = PLR_DEV_ID((UINT32)top.type, (UINT32)(top.instance & 0xFF));
		PLR_DEV_OPTS opts;
		PlayerBase::InitDeviceOptions(opts);
		if (player->GetDeviceOptions(id, opts) != 0x00)
		{
			allOk = false;
			continue;
		}

		if (preferNuked)
		{
			const DEV_DEF* const* defs = SndEmu_GetDevDefList(top.type);
			for (; defs != NULL && *defs != NULL; defs++)
			{
				if ((*defs)->coreID == FCC_NUKE)
				{
					opts.emuCore[0] = FCC_NUKE;
					break;
				}
			}
		}
		opts.srMode = DEVRI_SRMODE_NATIVE;	// let every core run at its own rate
		opts.smplRate = 0;

		UINT8 disable = 0x00;
		if (audibleRate != 0)
		{
			disable = 0xFF;
			if (top.rate == audibleRate)
				disable &= (UINT8)~0x01;
			for (size_t j = 0; j < chips.size(); j++)
			{
				const ChipInfo& link = chips[j];
				if (link.parent == (int)i && link.rate == audibleRate && link.instance < 7)
					disable &= (UINT8)~(0x02 << link.instance);
			}
		}
		opts.muteOpts.disable = disable;
		opts.muteOpts.chnMute[0] = 0x00;
		opts.muteOpts.chnMute[1] = 0x00;

		if (player->SetDeviceOptions(id, opts) != 0x00)
			allOk = false;
	}
	return allOk;
}

// ---------------------------------------------------------------------------
// Rendering

struct SongTiming
{
	bool looping = false;
	UINT32 loops = 1;
	double playSec = 0.0;	// intro + loops, before the fade
	double fadeSec = 0.0;
	double TotalSec() const { return playSec + fadeSec; }
};

SongTiming ComputeTiming(PlayerBase* player, const ConvertOptions& opt)
{
	SongTiming t;
	UINT32 loops = std::max(1u, opt.loops);
	if (player->GetPlayerType() == FCC_VGM)
	{
		VGMPlayer* vgm = dynamic_cast<VGMPlayer*>(player);
		if (vgm != NULL)
			loops = std::max<UINT32>(1, vgm->GetModifiedLoopCount(loops));
	}
	const UINT32 total = player->GetTotalTicks();
	const UINT32 loopTicks = player->GetLoopTicks();
	t.looping = loopTicks > 0;
	t.loops = t.looping ? loops : 1;
	UINT64 playTicks = total;
	if (t.looping)
		playTicks += (UINT64)loopTicks * (loops - 1);
	if (playTicks > 0xFFFFFFFEull)
		playTicks = 0xFFFFFFFEull;
	t.playSec = player->Tick2Second((UINT32)playTicks);
	t.fadeSec = t.looping ? std::max(0.0, opt.fadeSec) : 0.0;
	return t;
}

// Renders the song once at 'rate' (the native rate of the chips audible in
// this run) and adds the result, resampled to 'outRate', into 'mix'.
bool RenderRun(PlayerBase* player, UINT32 rate, UINT32 outRate, double totalSec, double gain,
               std::vector<double>& mix, size_t outFrames, std::string& err,
               const ConvertProgress& progress, double progBase, double progSpan)
{
	const UINT64 frames = (UINT64)std::ceil(totalSec * rate);

	soxr_t resampler = NULL;
	if (rate != outRate)
	{
		soxr_error_t sxErr = NULL;
		soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
		soxr_quality_spec_t quality = soxr_quality_spec(SOXR_VHQ, 0);
		resampler = soxr_create((double)rate, (double)outRate, 2, &sxErr, &io, &quality, NULL);
		if (resampler == NULL || sxErr != NULL)
		{
			err = std::string("could not set up the resampler: ") + (sxErr ? sxErr : "unknown error");
			if (resampler != NULL)
				soxr_delete(resampler);
			return false;
		}
	}

	std::vector<WAVE_32BS> chip(kChunkFrames);
	std::vector<double> in(kChunkFrames * 2);
	const size_t outCap = (size_t)((double)kChunkFrames * outRate / rate) + 1024;
	std::vector<double> out(outCap * 2);
	size_t outPos = 0;

	auto addToMix = [&](const double* data, size_t count)
	{
		for (size_t i = 0; i < count && outPos < outFrames; i++, outPos++)
		{
			mix[outPos * 2 + 0] += data[i * 2 + 0];
			mix[outPos * 2 + 1] += data[i * 2 + 1];
		}
	};

	bool ok = true;
	UINT64 done = 0;
	unsigned chunkCounter = 0;
	while (done < frames)
	{
		if (progress && (chunkCounter++ % 16) == 0)
		{
			if (!progress(progBase + progSpan * (double)done / (double)frames))
			{
				err = "cancelled";
				ok = false;
				break;
			}
		}
		const UINT32 count = (UINT32)std::min<UINT64>(kChunkFrames, frames - done);
		std::memset(chip.data(), 0, count * sizeof(WAVE_32BS));
		UINT32 got = 0;
		while (got < count)
		{
			UINT32 r = player->Render(count - got, chip.data() + got);
			if (r == 0)
				break;	// nothing more to render; the rest stays silent
			got += r;
		}
		for (UINT32 i = 0; i < count; i++)
		{
			in[i * 2 + 0] = chip[i].L * gain;
			in[i * 2 + 1] = chip[i].R * gain;
		}

		if (resampler == NULL)
		{
			addToMix(in.data(), count);
		}
		else
		{
			size_t consumed = 0;
			while (consumed < count)
			{
				size_t idone = 0, odone = 0;
				soxr_error_t sxErr = soxr_process(resampler, in.data() + consumed * 2, count - consumed, &idone,
				                                  out.data(), outCap, &odone);
				if (sxErr != NULL)
				{
					err = std::string("resampler error: ") + sxErr;
					ok = false;
					break;
				}
				addToMix(out.data(), odone);
				consumed += idone;
				if (idone == 0 && odone == 0)
					break;
			}
			if (!ok)
				break;
		}
		done += count;
	}

	if (resampler != NULL)
	{
		// flush what is left in the filter
		while (ok && resampler != NULL)
		{
			size_t odone = 0;
			soxr_error_t sxErr = soxr_process(resampler, NULL, 0, NULL, out.data(), outCap, &odone);
			if (sxErr != NULL)
			{
				err = std::string("resampler error: ") + sxErr;
				ok = false;
				break;
			}
			if (odone == 0)
				break;
			addToMix(out.data(), odone);
		}
		soxr_delete(resampler);
	}
	return ok;
}

// ---------------------------------------------------------------------------
// Tags

struct SongTags
{
	std::map<std::string, std::string> raw;	// libvgm tag name -> UTF-8 value

	std::string Get(const char* key) const
	{
		auto it = raw.find(key);
		return it == raw.end() ? std::string() : Trim(it->second);
	}
	std::string GetEnOrJp(const char* key) const
	{
		std::string v = Get(key);
		if (v.empty())
			v = Get((std::string(key) + "-JPN").c_str());
		return v;
	}
};

SongTags ReadTags(PlayerBase* player)
{
	SongTags tags;
	const char* const* t = player->GetTags();
	while (t != NULL && t[0] != NULL)
	{
		if (t[1] != NULL)
			tags.raw[t[0]] = t[1];
		t += 2;
	}
	return tags;
}

std::string TrackNumberFromName(const fs::path& file)
{
	const std::string stem = file.stem().u8string();
	std::string digits;
	for (char c : stem)
	{
		if (c >= '0' && c <= '9')
			digits += c;
		else
			break;
	}
	if (digits.empty() || digits.size() > 4)
		return std::string();
	return std::to_string(std::stoul(digits));
}

// ---------------------------------------------------------------------------
// FLAC output

FILE* OpenForWrite(const fs::path& path)
{
#ifdef _WIN32
	return _wfopen(path.wstring().c_str(), L"w+b");
#else
	return std::fopen(path.string().c_str(), "w+b");
#endif
}

bool WriteFlac(const fs::path& path, const std::vector<FLAC__int32>& pcm, size_t frames, unsigned bits,
               UINT32 rate, unsigned compression, const std::vector<std::pair<std::string, std::string>>& tags,
               std::string& err)
{
	FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
	if (enc == NULL)
	{
		err = "out of memory (FLAC encoder)";
		return false;
	}

	FLAC__stream_encoder_set_verify(enc, true);	// decode while encoding and compare: proves it is lossless
	FLAC__stream_encoder_set_compression_level(enc, std::min(8u, compression));
	FLAC__stream_encoder_set_channels(enc, 2);
	FLAC__stream_encoder_set_bits_per_sample(enc, bits);
	FLAC__stream_encoder_set_sample_rate(enc, rate);
	FLAC__stream_encoder_set_total_samples_estimate(enc, (FLAC__uint64)frames);

	FLAC__StreamMetadata* comments = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
	FLAC__StreamMetadata* seekTable = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
	FLAC__StreamMetadata* padding = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
	if (comments == NULL || seekTable == NULL || padding == NULL)
	{
		err = "out of memory (FLAC metadata)";
		if (comments) FLAC__metadata_object_delete(comments);
		if (seekTable) FLAC__metadata_object_delete(seekTable);
		if (padding) FLAC__metadata_object_delete(padding);
		FLAC__stream_encoder_delete(enc);
		return false;
	}
	for (const auto& tag : tags)
	{
		if (tag.second.empty())
			continue;
		FLAC__StreamMetadata_VorbisComment_Entry entry;
		if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, tag.first.c_str(), tag.second.c_str()))
			FLAC__metadata_object_vorbiscomment_append_comment(comments, entry, /*copy=*/false);
	}
	FLAC__metadata_object_seektable_template_append_spaced_points_by_samples(seekTable, rate * 10, (FLAC__uint64)frames);
	FLAC__metadata_object_seektable_template_sort(seekTable, /*compact=*/true);
	padding->length = 4096;	// room to edit tags later without rewriting the file

	FLAC__StreamMetadata* metadata[3] = { comments, seekTable, padding };
	FLAC__stream_encoder_set_metadata(enc, metadata, 3);

	bool ok = false;
	FILE* file = OpenForWrite(path);
	if (file == NULL)
	{
		err = "could not create the output file";
	}
	else
	{
		FLAC__StreamEncoderInitStatus st = FLAC__stream_encoder_init_FILE(enc, file, NULL, NULL);
		if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		{
			err = std::string("FLAC encoder setup failed: ") + FLAC__StreamEncoderInitStatusString[st];
		}
		else
		{
			ok = true;
			const size_t chunk = 4096;
			for (size_t pos = 0; pos < frames && ok; pos += chunk)
			{
				const size_t n = std::min(chunk, frames - pos);
				if (!FLAC__stream_encoder_process_interleaved(enc, pcm.data() + pos * 2, (uint32_t)n))
				{
					err = std::string("FLAC encoding failed: ") +
					      FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(enc)];
					ok = false;
				}
			}
			// finish() also closes the file
			if (!FLAC__stream_encoder_finish(enc) && ok)
			{
				err = std::string("FLAC encoding failed: ") +
				      FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(enc)];
				ok = false;
			}
		}
	}

	FLAC__stream_encoder_delete(enc);
	FLAC__metadata_object_delete(comments);
	FLAC__metadata_object_delete(seekTable);
	FLAC__metadata_object_delete(padding);
	return ok;
}

} // namespace

// ---------------------------------------------------------------------------

bool HasSongExtension(const fs::path& p)
{
	std::string ext = p.extension().u8string();
	for (char& c : ext)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return ext == ".vgm" || ext == ".vgz" || ext == ".s98" || ext == ".dro" || ext == ".gym";
}

std::vector<fs::path> FindSongs(const fs::path& in, std::vector<std::string>& problems)
{
	std::vector<fs::path> songs;
	std::error_code ec;
	if (fs::is_directory(in, ec))
	{
		fs::recursive_directory_iterator it(in, fs::directory_options::skip_permission_denied, ec);
		for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			std::error_code ec2;
			if (it->is_regular_file(ec2) && HasSongExtension(it->path()))
				songs.push_back(it->path());
		}
		std::sort(songs.begin(), songs.end());
		if (songs.empty())
			problems.push_back(in.u8string() + ": no .vgz/.vgm files in this folder");
	}
	else if (fs::is_regular_file(in, ec))
		songs.push_back(in);
	else
		problems.push_back(in.u8string() + ": not found");
	return songs;
}

ConvertResult ConvertFile(const fs::path& inFile, const fs::path& outFile, const ConvertOptions& opt,
                          const ConvertProgress& progress)
{
	ConvertResult result;
	LogSink log;
	FileRequestContext req;
	req.searchDirs.push_back(inFile.parent_path());

	try
	{
		// --- 1. probe: chips, native rates, length, tags ---
		std::vector<ChipInfo> chips;
		SongTiming timing;
		SongTags tags;
		double gain = 1.0 / kLibvgmFullScale;
		bool canSplit = opt.splitByRate;
		{
			Song song;
			std::string err;
			if (!song.Open(inFile, &req, &log, err))
			{
				result.lines.push_back(err);
				return result;
			}
			PlayerBase* player = song.Player();

			std::vector<ChipInfo> headerChips = ReadChips(player);	// from the file header, before starting
			if (!ConfigureChips(player, headerChips, opt.preferNuked, 0))
				canSplit = false;

			timing = ComputeTiming(player, opt);
			tags = ReadTags(player);
			PLR_SONG_INFO info;
			if (player->GetSongInfo(info) == 0x00 && info.volGain > 0)
				gain = (info.volGain / 65536.0) / kLibvgmFullScale;

			player->SetSampleRate(opt.outRate);
			if (player->Start() != 0x00)
			{
				result.lines.push_back("libvgm could not start the song");
				return result;
			}
			chips = ReadChips(player);	// live: actual cores and native rates
		}

		std::vector<UINT32> rates;
		for (const ChipInfo& c : chips)
		{
			if (c.rate != 0 && std::find(rates.begin(), rates.end(), c.rate) == rates.end())
				rates.push_back(c.rate);
		}
		if (rates.empty())
		{
			result.lines.push_back("the file uses no sound chip that libvgm can emulate");
			return result;
		}
		if (timing.TotalSec() <= 0.0)
		{
			result.lines.push_back("the file has no length");
			return result;
		}

		const size_t outFrames = (size_t)std::llround(timing.TotalSec() * opt.outRate);
		std::vector<double> mix(outFrames * 2, 0.0);

		// --- 2./3. render each native-rate group and resample it ---
		if (canSplit)
		{
			for (size_t runIdx = 0; runIdx < rates.size(); runIdx++)
			{
				const UINT32 rate = rates[runIdx];
				Song song;
				std::string err;
				if (!song.Open(inFile, &req, &log, err))
				{
					result.lines.push_back(err);
					return result;
				}
				PlayerBase* player = song.Player();
				std::vector<ChipInfo> headerChips = ReadChips(player);
				// map header entries to the probed rates (same order: same file, same options)
				bool sameLayout = headerChips.size() == chips.size();
				for (size_t i = 0; sameLayout && i < chips.size(); i++)
				{
					sameLayout = headerChips[i].type == chips[i].type && headerChips[i].parent == chips[i].parent;
					headerChips[i].rate = chips[i].rate;
				}
				if (!sameLayout)
				{
					canSplit = false;
					break;
				}
				if (!ConfigureChips(player, headerChips, opt.preferNuked, rate))
				{
					canSplit = false;
					break;
				}
				player->SetSampleRate(rate);
				if (player->Start() != 0x00)
				{
					result.lines.push_back("libvgm could not start the song");
					return result;
				}
				for (const ChipInfo& c : ReadChips(player))
				{
					// a core that ignored the native-rate request is still fine (libvgm resamples it),
					// but say so
					bool wanted = false;
					for (const ChipInfo& p : chips)
						if (p.type == c.type && p.instance == c.instance && p.parent == c.parent && p.rate == rate)
							wanted = true;
					if (wanted && c.rate != rate)
						log.messages.insert("note: " + c.name + " ran at " + std::to_string(c.rate) +
						                    " Hz instead of " + std::to_string(rate) + " Hz");
				}
				const double span = 0.9 / rates.size();
				if (!RenderRun(player, rate, opt.outRate, timing.TotalSec(), gain, mix, outFrames, err,
				               progress, span * runIdx, span))
				{
					result.lines.push_back(err);
					return result;
				}
			}
		}
		if (!canSplit)
		{
			// Fallback: one run with every chip audible, at the output rate.
			std::fill(mix.begin(), mix.end(), 0.0);
			Song song;
			std::string err;
			if (!song.Open(inFile, &req, &log, err))
			{
				result.lines.push_back(err);
				return result;
			}
			PlayerBase* player = song.Player();
			ConfigureChips(player, ReadChips(player), opt.preferNuked, 0);
			player->SetSampleRate(opt.outRate);
			if (player->Start() != 0x00)
			{
				result.lines.push_back("libvgm could not start the song");
				return result;
			}
			if (!RenderRun(player, opt.outRate, opt.outRate, timing.TotalSec(), gain, mix, outFrames, err,
			               progress, 0.0, 0.9))
			{
				result.lines.push_back(err);
				return result;
			}
			if (opt.splitByRate)
				log.messages.insert("note: chips could not be rendered separately, so libvgm's own resampler was used");
		}

		// --- 4. fade, clipping guard, rounding ---
		if (timing.looping && timing.fadeSec > 0.0)
		{
			const double fadeStart = timing.playSec * opt.outRate;
			const double fadeLen = timing.fadeSec * opt.outRate;
			const size_t first = (size_t)std::max(0.0, std::floor(fadeStart));
			for (size_t i = first; i < outFrames; i++)
			{
				double x = (i - fadeStart) / fadeLen;
				double v = 0.0;
				if (x < 0.0)
					v = 1.0;
				else if (x < 1.0)
					v = (1.0 - x) * (1.0 - x);	// same curve as libvgm/VGMPlay
				mix[i * 2 + 0] *= v;
				mix[i * 2 + 1] *= v;
			}
		}

		double peak = 0.0;
		for (double s : mix)
			peak = std::max(peak, std::fabs(s));

		const double fullScale = (double)(1u << (opt.bits - 1));
		const double maxSample = (fullScale - 1.0) / fullScale;
		double outGain = 1.0;
		if (peak > maxSample)
		{
			outGain = std::pow(10.0, -0.1 / 20.0) / peak;	// bring the peak to -0.1 dBFS instead of clipping
			char buf[96];
			std::snprintf(buf, sizeof(buf), "note: lowered by %.2f dB so the loudest peak does not clip",
			              -20.0 * std::log10(outGain));
			log.messages.insert(buf);
		}

		std::vector<FLAC__int32> pcm(outFrames * 2);
		std::mt19937 rng(0x5EED);
		std::uniform_real_distribution<double> uni(-0.5, 0.5);
		const bool dither = opt.bits < 24;	// TPDF dither only when cutting down to 16 bits
		const double lo = -fullScale, hi = fullScale - 1.0;
		for (size_t i = 0; i < pcm.size(); i++)
		{
			double v = mix[i] * outGain * fullScale;
			if (dither)
				v += uni(rng) + uni(rng);
			v = std::nearbyint(v);
			pcm[i] = (FLAC__int32)std::min(hi, std::max(lo, v));
		}
		std::vector<double>().swap(mix);

		// --- 5. tags and FLAC ---
		std::string cores;
		for (const ChipInfo& c : chips)
		{
			if (c.rate == 0)
				continue;
			if (!cores.empty())
				cores += ", ";
			cores += c.name + " (" + CoreName(c.core) + ")";
		}
		std::vector<std::pair<std::string, std::string>> flacTags;
		flacTags.emplace_back("TITLE", tags.GetEnOrJp("TITLE"));
		flacTags.emplace_back("ALBUM", tags.GetEnOrJp("GAME"));
		flacTags.emplace_back("ARTIST", tags.GetEnOrJp("ARTIST"));
		flacTags.emplace_back("DATE", tags.Get("DATE"));
		flacTags.emplace_back("TRACKNUMBER", TrackNumberFromName(inFile));
		flacTags.emplace_back("GENRE", "Game");
		if (!tags.Get("TITLE-JPN").empty() && tags.Get("TITLE-JPN") != tags.Get("TITLE"))
			flacTags.emplace_back("TITLE_JP", tags.Get("TITLE-JPN"));
		if (!tags.Get("GAME-JPN").empty() && tags.Get("GAME-JPN") != tags.Get("GAME"))
			flacTags.emplace_back("ALBUM_JP", tags.Get("GAME-JPN"));
		if (!tags.Get("ARTIST-JPN").empty() && tags.Get("ARTIST-JPN") != tags.Get("ARTIST"))
			flacTags.emplace_back("ARTIST_JP", tags.Get("ARTIST-JPN"));
		flacTags.emplace_back("SYSTEM", tags.GetEnOrJp("SYSTEM"));
		flacTags.emplace_back("RIPPER", tags.Get("ENCODED_BY"));
		{
			std::string comment = tags.Get("COMMENT");
			std::string how = "Rendered by vgz2flac " VGZ2FLAC_VERSION " with libvgm: " + cores +
			                  ", each chip at its native rate, resampled with SoX VHQ";
			flacTags.emplace_back("COMMENT", comment.empty() ? how : comment + "\n\n" + how);
		}
		flacTags.emplace_back("ENCODER", "vgz2flac " VGZ2FLAC_VERSION);

		if (progress && !progress(0.9))
		{
			result.lines.push_back("cancelled");
			return result;
		}
		fs::path tmp = outFile;
		tmp += ".part";
		std::string err;
		if (!WriteFlac(tmp, pcm, outFrames, opt.bits, opt.outRate, opt.compression, flacTags, err))
		{
			std::error_code ec;
			fs::remove(tmp, ec);
			result.lines.push_back(err);
			return result;
		}
		std::error_code ec;
		fs::rename(tmp, outFile, ec);
		if (ec)
		{
			std::error_code ec2;
			fs::remove(outFile, ec2);
			fs::rename(tmp, outFile, ec);
		}
		if (ec)
		{
			fs::remove(tmp, ec);
			result.lines.push_back("could not move the finished file into place: " + ec.message());
			return result;
		}

		// --- report ---
		char summary[256];
		std::snprintf(summary, sizeof(summary), "%s, %u Hz / %u-bit, peak %s%s",
		              FormatTime(timing.TotalSec()).c_str(), (unsigned)opt.outRate, opt.bits,
		              FormatDb(peak * outGain).c_str(),
		              timing.looping ? (", " + std::to_string(timing.loops) + " loops + " +
		                                FormatTime(timing.fadeSec) + " fade").c_str() : "");
		result.lines.push_back(summary);
		for (const ChipInfo& c : chips)
		{
			if (c.rate == 0)
				continue;
			result.lines.push_back("  " + c.name + ": " + CoreName(c.core) + " core at " + std::to_string(c.rate) +
			                       " Hz" + (c.rate == opt.outRate ? "" : " -> SoX VHQ"));
		}
		for (const std::string& m : log.messages)
			result.lines.push_back("  " + m);
		if (progress)
			progress(1.0);
		result.ok = true;
	}
	catch (const std::bad_alloc&)
	{
		result.ok = false;
		result.lines.push_back("out of memory");
	}
	catch (const std::exception& e)
	{
		result.ok = false;
		result.lines.push_back(std::string("error: ") + e.what());
	}
	return result;
}
