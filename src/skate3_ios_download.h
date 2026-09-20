// NSURLSession download, for the one platform that can neither shell out to
// curl nor delegate to an app shell. See skate3_ios_download.mm.

#ifndef SKATE3_IOS_DOWNLOAD_H_
#define SKATE3_IOS_DOWNLOAD_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace skate3 {

// Blocks until the transfer finishes. Progress is published through the same
// two atomics every other platform's DownloadToFile uses, so the installer's
// progress bar needs no platform knowledge. Never called from the UI thread.
bool IosDownloadToFile(const std::string& url, const std::filesystem::path& destination,
                       std::atomic<uint64_t>& copied_bytes, std::atomic<uint64_t>& total_bytes,
                       std::string& error);

}  // namespace skate3

#endif  // SKATE3_IOS_DOWNLOAD_H_
