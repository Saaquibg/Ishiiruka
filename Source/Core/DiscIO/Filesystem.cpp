// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DiscIO/Filesystem.h"
#include <memory>
#include "DiscIO/FileSystemGCWii.h"

namespace DiscIO
{
IFileSystem::IFileSystem(const IVolume* _rVolume) : m_rVolume(_rVolume)
{
}

IFileSystem::~IFileSystem()
{
}

std::unique_ptr<IFileSystem> CreateFileSystem(const IVolume* volume)
{
	if (!volume)
		return nullptr;

	std::unique_ptr<IFileSystem> filesystem = std::make_unique<CFileSystemGCWii>(volume);

	if (!filesystem)
		return nullptr;

	if (!filesystem->IsValid())
		filesystem.reset();

	return filesystem;
}

}  // namespace
