// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "Walk.hxx"
#include "UpdateDomain.hxx"
#include "db/DatabaseLock.hxx"
#include "db/plugins/simple/Directory.hxx"
#include "db/plugins/simple/Song.hxx"
#include "storage/StorageInterface.hxx"
#include "lib/fmt/PathFormatter.hxx"
#include "fs/AllocatedPath.hxx"
#include "storage/FileInfo.hxx"
#include "archive/ArchiveList.hxx"
#include "archive/ArchivePlugin.hxx"
#include "archive/ArchiveFile.hxx"
#include "archive/ArchiveVisitor.hxx"
#include "util/StringSplit.hxx"
#include "Log.hxx"

#include <exception>

#include <string.h>

static Directory *
LockMakeChild(Directory &directory, std::string_view name) noexcept
{
	const ScopeDatabaseLock protect;
	return directory.MakeChild(name);
}

static Song *
LockFindSong(Directory &directory, std::string_view name) noexcept
{
	const ScopeDatabaseLock protect;
	return directory.FindSong(name);
}

[[gnu::pure]]
static bool
IsAcceptableFilename(std::string_view name) noexcept
{
	return !name.empty() &&
		/* newlines cannot be represented in MPD's protocol */
		name.find('\n') == name.npos;
}

void
UpdateWalk::UpdateArchiveTree(ArchiveFile &archive, Directory &directory,
			      std::string_view name) noexcept
{
	const auto [child_name, rest] = Split(name, '/');
	if (rest.data() != nullptr) {
		if (!IsAcceptableFilename(child_name))
			return;

		//add dir is not there already
		Directory *subdir = LockMakeChild(directory, child_name);
		subdir->device = DEVICE_INARCHIVE;

		//create directories first
		UpdateArchiveTree(archive, *subdir, rest);
	} else {
		if (!IsAcceptableFilename(name))
			return;

		//add file
		Song *song = LockFindSong(directory, name);
		if (song == nullptr) {
			auto new_song = Song::LoadFromArchive(archive, name, directory);
			if (new_song) {
				{
					const ScopeDatabaseLock protect;
					directory.AddSong(std::move(new_song));
				}

				modified = true;
				FmtNotice(update_domain, "added {}/{}",
					  directory.GetPath(), name);
			}
		} else {
			if (!song->UpdateFileInArchive(archive)) {
				FmtDebug(update_domain,
					 "deleting unrecognized file {}/{}",
					 directory.GetPath(), name);
				editor.LockDeleteSong(directory, song);
			}
		}
	}
}

class UpdateArchiveVisitor final : public ArchiveVisitor {
	UpdateWalk &walk;
	ArchiveFile &archive;
	Directory &directory;

 public:
	UpdateArchiveVisitor(UpdateWalk &_walk, ArchiveFile &_archive,
			     Directory &_directory) noexcept
		:walk(_walk), archive(_archive), directory(_directory) {}

	void VisitArchiveEntry(const char *path_utf8) override {
		FmtDebug(update_domain,
			 "adding archive file: {}", path_utf8);
		walk.UpdateArchiveTree(archive, directory, path_utf8);
	}
};

/**
 * Updates the file listing from an archive file.
 *
 * @param parent the parent directory the archive file resides in
 * @param name the UTF-8 encoded base name of the archive file
 * @param st stat() information on the archive file
 * @param plugin the archive plugin which fits this archive type
 */
void
UpdateWalk::UpdateArchiveFile(Directory &parent, std::string_view name,
			      const StorageFileInfo &info,
			      const ArchivePlugin &plugin) noexcept
{
	const auto path_fs = storage.MapChildFS(parent.GetPath(), name);
	if (path_fs.IsNull())
		/* not a local file: skip, because the archive API
		   supports only local files */
		return;

	Directory *directory =
		LockMakeVirtualDirectoryIfModified(parent, name, info,
						   DEVICE_INARCHIVE);
	if (directory == nullptr)
		/* not modified */
		return;

	/* open archive */
	std::unique_ptr<ArchiveFile> file;
	try {
		file = archive_file_open(&plugin, path_fs);
	} catch (...) {
		LogError(std::current_exception());
		editor.LockDeleteDirectory(directory);
		return;
	}

	FmtDebug(update_domain, "archive {} opened", path_fs);

	UpdateArchiveVisitor visitor(*this, *file, *directory);
	file->Visit(visitor);
}

bool
UpdateWalk::UpdateArchiveFile(Directory &directory,
			      std::string_view name, std::string_view suffix,
			      const StorageFileInfo &info) noexcept
{
	const ArchivePlugin *plugin = archive_plugin_from_suffix(suffix);
	if (plugin == nullptr)
		return false;

	UpdateArchiveFile(directory, name, info, *plugin);
	return true;
}
