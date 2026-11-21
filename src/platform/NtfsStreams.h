#pragma once
// ============================================================================
// platform/NtfsStreams.h - alternate data stream operations.
//
// Win32 + STL only. This module knows how to enumerate and copy ADS content;
// filesystem safety decisions stay in the caller.
// ============================================================================

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "platform/FileOps.h"

namespace ze::platform {

struct AlternateStream {
    std::wstring name;       // Empty for the unnamed/default :$DATA stream.
    std::uint64_t size = 0;
    bool isDefault = false;
};

bool ListAlternateStreams(const std::wstring& basePath,
                          std::vector<AlternateStream>* streams,
                          std::wstring* error = nullptr);

bool ValidateAlternateStreamName(const std::wstring& name,
                                  std::wstring* error = nullptr);

bool CopyFileToAlternateStream(const std::wstring& basePath,
                               const std::wstring& streamName,
                               const std::wstring& sourceFile,
                               bool overwrite,
                               const ProgressFn& progress,
                               OpResult* result);

bool CopyAlternateStreamToFile(const std::wstring& basePath,
                               const std::wstring& streamName,
                               const std::wstring& destinationFile,
                               bool overwrite,
                               const ProgressFn& progress,
                               OpResult* result);

bool DeleteAlternateStream(const std::wstring& basePath,
                           const std::wstring& streamName,
                           std::wstring* error = nullptr);

bool ReadAlternateStreamText(const std::wstring& basePath,
                             const std::wstring& streamName,
                             std::wstring* text,
                             std::wstring* error = nullptr);

bool WriteAlternateStreamText(const std::wstring& basePath,
                              const std::wstring& streamName,
                              const std::wstring& text,
                              bool overwrite,
                              std::wstring* error = nullptr);

bool RenameAlternateStream(const std::wstring& basePath,
                           const std::wstring& oldName,
                           const std::wstring& newName,
                           const ProgressFn& progress,
                           OpResult* result);

} // namespace ze::platform
