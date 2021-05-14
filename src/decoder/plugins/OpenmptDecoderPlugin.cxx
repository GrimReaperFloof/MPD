/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "OpenmptDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "fs/NarrowPath.hxx"
#include "input/InputStream.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "tag/Type.h"
#include "song/DetachedSong.hxx"
#include "util/Domain.hxx"
#include "util/RuntimeError.hxx"
#include "util/StringCompare.hxx"
#include "util/StringFormat.hxx"
#include "util/StringView.hxx"
#include "Log.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"

#include <fstream>
#include <libopenmpt/libopenmpt.hpp>

#include <cassert>

#define SUBTUNE_PREFIX "tune_"

static constexpr Domain openmpt_domain("openmpt");

static constexpr size_t OPENMPT_FRAME_SIZE = 4096;
static constexpr int32_t OPENMPT_SAMPLE_RATE = 48000;

struct OpenmptContainerPath {
	AllocatedPath path;
	unsigned track;
};

static int openmpt_stereo_separation;
static int openmpt_interpolation_filter;
static bool openmpt_override_mptm_interp_filter;
static int openmpt_volume_ramping;
static bool openmpt_sync_samples;
static bool openmpt_emulate_amiga;
static std::string_view openmpt_emulate_amiga_type;

static bool
openmpt_decoder_init(const ConfigBlock &block)
{
	openmpt_stereo_separation = block.GetBlockValue("stereo_separation", 100);
	openmpt_interpolation_filter = block.GetBlockValue("interpolation_filter", 0);
	openmpt_override_mptm_interp_filter = block.GetBlockValue("override_mptm_interp_filter", false);
	openmpt_volume_ramping = block.GetBlockValue("volume_ramping", -1);
	openmpt_sync_samples = block.GetBlockValue("sync_samples", true);
	openmpt_emulate_amiga = block.GetBlockValue("emulate_amiga", true);
	openmpt_emulate_amiga_type = block.GetBlockValue("emulate_amiga_type", "auto");

	return true;
}

gcc_pure
static unsigned
ParseSubtuneName(const char *base) noexcept
{
	base = StringAfterPrefix(base, SUBTUNE_PREFIX);
	if (base == nullptr)
		return 0;

	char *endptr;
	auto track = strtoul(base, &endptr, 10);
	if (endptr == base || *endptr != '.')
		return 0;

	return track;
}


/**
 * returns the file path stripped of any /tune_xxx.* subtune suffix
 * and the track number (or 0 if no "tune_xxx" suffix is present).
 */
static OpenmptContainerPath
ParseContainerPath(Path path_fs)
{
	const Path base = path_fs.GetBase();
	unsigned track;
	if (base.IsNull() ||
	    (track = ParseSubtuneName(NarrowPath(base))) < 1)
		return { AllocatedPath(path_fs), 0 };

	return { path_fs.GetDirectoryName(), track - 1 };
}

static void
mod_decode(DecoderClient &client, Path path_fs)
{
	int ret;
	char audio_buffer[OPENMPT_FRAME_SIZE];

	const auto container = ParseContainerPath(path_fs);

	std::ifstream file(container.path.c_str());
	openmpt::module mod(file);
	
	const int song_num = container.track;
	mod.select_subsong(song_num);

	/* alter settings */
	mod.set_render_param(mod.RENDER_STEREOSEPARATION_PERCENT, openmpt_stereo_separation);
	mod.set_render_param(mod.RENDER_INTERPOLATIONFILTER_LENGTH, openmpt_interpolation_filter);
	if (!openmpt_override_mptm_interp_filter && mod.get_metadata("type") == "mptm") {
		/* The MPTM format has a setting for which interpolation filter should be used.
		 * If we want to play the module back the way the composer intended it,
		 * we have to set the interpolation filter setting in libopenmpt back to 0: internal default. */
		mod.set_render_param(mod.RENDER_INTERPOLATIONFILTER_LENGTH, 0);
	}
	mod.set_render_param(mod.RENDER_VOLUMERAMPING_STRENGTH, openmpt_volume_ramping);
	mod.ctl_set_boolean("seek.sync_samples", openmpt_sync_samples);
	mod.ctl_set_boolean("render.resampler.emulate_amiga", openmpt_emulate_amiga);
	mod.ctl_set_text("render.resampler.emulate_amiga_type", openmpt_emulate_amiga_type);

	static constexpr AudioFormat audio_format(OPENMPT_SAMPLE_RATE, SampleFormat::FLOAT, 2);
	assert(audio_format.IsValid());

	client.Ready(audio_format, true,
		     SongTime::FromS(mod.get_duration_seconds()));

	DecoderCommand cmd;
	do {
		ret = mod.read_interleaved_stereo(OPENMPT_SAMPLE_RATE, OPENMPT_FRAME_SIZE / 2 / sizeof(float), (float*)audio_buffer);
		if (ret <= 0)
			break;

		cmd = client.SubmitData(nullptr,
					audio_buffer, ret * 2 * sizeof(float),
					0);

		if (cmd == DecoderCommand::SEEK) {
			mod.set_position_seconds(client.GetSeekTime().ToS());
			client.CommandFinished();
		}

	} while (cmd != DecoderCommand::STOP);
}

static void ScanModInfo(openmpt::module &mod, Path path_fs, unsigned subsong, TagHandler &handler) noexcept
{
	int subsongs = mod.get_num_subsongs();
	std::string title = mod.get_metadata("title");

	if (subsongs > 1) {
		handler.OnTag(TAG_TRACK, (StringView)std::to_string(subsong + 1));
		handler.OnTag(TAG_ALBUM, title.c_str());

		std::string subsong_name = mod.get_subsong_names()[subsong];
		if (!subsong_name.empty()) {
			handler.OnTag(TAG_TITLE, subsong_name.c_str());
		} else {
			if (title.empty()) {
				const auto container = ParseContainerPath(path_fs);
				Path path = container.path;
				title = path.GetBase().ToUTF8();
			}

			const auto tag_title =
				StringFormat<1024>("%s (%u/%u)", title.c_str(), subsong + 1, subsongs);
			handler.OnTag(TAG_TITLE, tag_title.c_str());
		}
	} else {
		handler.OnTag(TAG_TITLE, title.c_str());
	}

	handler.OnTag(TAG_ARTIST, mod.get_metadata("artist").c_str());
	handler.OnTag(TAG_COMMENT, mod.get_metadata("message").c_str());
	handler.OnTag(TAG_DATE, mod.get_metadata("date").c_str());
	handler.OnTag(TAG_PERFORMER, mod.get_metadata("tracker").c_str());
}

static bool
openmpt_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	const auto container = ParseContainerPath(path_fs);
	const unsigned song_num = container.track;

	std::ifstream file(container.path.c_str(), std::ios::binary);
	openmpt::module mod(file);

	mod.select_subsong(song_num);
	
	handler.OnDuration(SongTime::FromS(mod.get_duration_seconds()));
	
	ScanModInfo(mod, path_fs, song_num, handler);
	return true;
}

static std::forward_list<DetachedSong>
openmpt_container_scan(Path path_fs)
{
	std::forward_list<DetachedSong> list;

	std::ifstream file(path_fs.c_str(), std::ios::binary);
	openmpt::module mod(file);
	
	int32_t subsongs = mod.get_num_subsongs();

	if (subsongs <= 1)
		return list;
	
	TagBuilder tag_builder;

	std::vector<std::string> subsong_names = mod.get_subsong_names();
	
	auto tail = list.before_begin();
	for (int32_t i = 0; i <= subsongs - 1; ++i) {
		mod.select_subsong(i);
		
		AddTagHandler h(tag_builder);
		ScanModInfo(mod, path_fs, i, h);
		h.OnDuration(SongTime::FromS(mod.get_duration_seconds()));
		
		std::string ext = mod.get_metadata("type");
		tail = list.emplace_after(
				tail,
				StringFormat<32>(SUBTUNE_PREFIX "%03u.%s", i+1, ext.c_str()),
				tag_builder.Commit());
	}

	return list;
}

static const char *const mod_suffixes[] = {
	"mptm", "mod", "s3m", "xm", "it", "669", "amf", "ams",
	"c67", "dbm", "digi", "dmf", "dsm", "dtm", "far", "imf",
	"ice", "j2b", "m15", "mdl", "med", "mms", "mt2", "mtm",
	"nst", "okt", "plm", "psm", "pt36", "ptm", "sfx", "sfx2",
	"st26", "stk", "stm", "stp", "ult", "wow", "gdm", "mo3",
	"oxm", "umx", "xpk", "ppm", "mmcmp",
	nullptr
};

constexpr DecoderPlugin openmpt_decoder_plugin =
	DecoderPlugin("openmpt", mod_decode, openmpt_scan_file)
	.WithInit(openmpt_decoder_init)
	.WithContainer(openmpt_container_scan)
	.WithSuffixes(mod_suffixes);
