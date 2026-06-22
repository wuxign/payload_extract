#ifndef ZIP_DIRECT_EXTRACTOR_H
#define ZIP_DIRECT_EXTRACTOR_H

#include <atomic>
#include <memory>
#include <string>

#include "payload/HttpDownload.h"
#include "payload/ZipParser.h"

namespace skkk {
	class ZipDirectExtractor {
		public:
			// Extract a ZIP entry to a file (memory-mapped mode).
			// fileData/fileDataSize: memory-mapped ZIP file data
			// partitionName: name to search for (e.g., "boot" matches "boot.img")
			// outputPath: full output file path
			// progress: optional atomic counter for progress tracking (incremented from 0 to totalProgressUnits)
			// totalProgressUnits: max value for progress (used to calculate percentage)
			static bool extractEntry(const uint8_t *fileData, uint64_t fileDataSize,
			                         const std::string &partitionName,
			                         const std::string &outputPath,
			                         std::shared_ptr<std::atomic_int> progress = nullptr,
			                         uint64_t totalProgressUnits = 0);

			// Extract a ZIP entry to a file (HTTP download mode).
			static bool extractEntryUrl(const std::shared_ptr<HttpDownload> &httpDownload,
			                            const std::string &partitionName,
			                            const std::string &outputPath,
			                            std::shared_ptr<std::atomic_int> progress = nullptr,
			                            uint64_t totalProgressUnits = 0);
	};
}

#endif //ZIP_DIRECT_EXTRACTOR_H
