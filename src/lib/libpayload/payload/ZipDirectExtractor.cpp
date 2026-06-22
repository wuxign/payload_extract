#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>

#include "payload/ZipDirectExtractor.h"
#include "payload/LogBase.h"
#include "payload/common/Buffer.hpp"
#include "payload/common/io.h"

namespace {
	void finalizeProgressOnFail(const std::shared_ptr<std::atomic_int> &progress,
	                            uint64_t totalProgressUnits) {
		if (progress) {
			*progress = static_cast<int>(totalProgressUnits);
		}
	}
}

namespace skkk {
	bool ZipDirectExtractor::extractEntry(const uint8_t *fileData, uint64_t fileDataSize,
	                                      const std::string &partitionName,
	                                      const std::string &outputPath,
	                                      std::shared_ptr<std::atomic_int> progress,
	                                      uint64_t totalProgressUnits) {
		// Create ZipParser and parse the ZIP
		ZipParser parser(fileData, fileDataSize);
		if (!parser.parse()) {
			LOGCE("ZipDirectExtractor: failed to parse ZIP data");
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		// Find the entry by partition name
		const ZipFileItem *item = parser.findEntryByPartition(partitionName);
		if (!item) {
			LOGCE("ZipDirectExtractor: partition '{}' not found in ZIP", partitionName);
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		// Threshold for chunked writing: 128MB
		constexpr uint64_t CHUNK_THRESHOLD = 128ULL * 1024 * 1024;

		if (item->uncompressedSize <= CHUNK_THRESHOLD) {
			// Small file: read all into memory, then write
			Buffer<uint8_t> data;
			data.reserve(item->uncompressedSize);
			if (!data.get()) {
				LOGCE("ZipDirectExtractor: failed to allocate {} bytes", item->uncompressedSize);
				finalizeProgressOnFail(progress, totalProgressUnits);
				return false;
			}

		if (!parser.readEntry(*item, data.get(), item->uncompressedSize, progress, totalProgressUnits)) {
			LOGCE("ZipDirectExtractor: failed to read entry '{}'", item->name);
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		// Write to output file
		int fd = open(outputPath.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_BINARY, 0644);
		if (fd < 0) {
			LOGCE("ZipDirectExtractor: failed to create output file '{}' ({})", outputPath, strerror(errno));
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		ssize_t written = write(fd, data.get(), item->uncompressedSize);

			if (static_cast<uint64_t>(written) != item->uncompressedSize) {
				LOGCE("ZipDirectExtractor: wrote {} bytes, expected {}", written, item->uncompressedSize);
				close(fd);
				finalizeProgressOnFail(progress, totalProgressUnits);
				return false;
			}

			close(fd);
		} else {
			// Large file: create output file first
			int fd = open(outputPath.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_BINARY, 0644);
			if (fd < 0) {
				LOGCE("ZipDirectExtractor: failed to create output file '{}' ({})", outputPath, strerror(errno));
				finalizeProgressOnFail(progress, totalProgressUnits);
				return false;
			}

			if (item->compression != 0) {
				// Compressed entry: need full buffer for decompression
				Buffer<uint8_t> data;
				data.reserve(item->uncompressedSize);
				if (!data.get()) {
					LOGCE("ZipDirectExtractor: failed to allocate {} bytes for decompression", item->uncompressedSize);
					close(fd);
					finalizeProgressOnFail(progress, totalProgressUnits);
					return false;
				}

			if (!parser.readEntry(*item, data.get(), item->uncompressedSize, progress, totalProgressUnits)) {
				LOGCE("ZipDirectExtractor: failed to read entry '{}'", item->name);
				close(fd);
				finalizeProgressOnFail(progress, totalProgressUnits);
				return false;
			}

			// Pre-allocate space (non-fatal: Windows doesn't support fallocate, write() will extend as needed)
				if (blobFallocate(fd, 0, item->uncompressedSize)) {
					LOGCW("ZipDirectExtractor: fallocate failed for '{}', continuing without preallocation", item->name);
				}

				ssize_t written = write(fd, data.get(), item->uncompressedSize);
				if (static_cast<uint64_t>(written) != item->uncompressedSize) {
					LOGCE("ZipDirectExtractor: wrote {} bytes, expected {}", written, item->uncompressedSize);
					close(fd);
					finalizeProgressOnFail(progress, totalProgressUnits);
					return false;
				}
				close(fd);
			} else {
				// Uncompressed: read chunk by chunk
				constexpr uint64_t CHUNK_SIZE = 64ULL * 1024 * 1024; // 64MB chunks
				Buffer<uint8_t> chunk;
				chunk.reserve(CHUNK_SIZE);

				uint64_t dataOffset = 0;
				uint64_t compressedDataSize = 0;
				if (!parser.getEntryDataOffset(*item, dataOffset, compressedDataSize)) {
					LOGCE("ZipDirectExtractor: failed to get data offset for '{}'", item->name);
					close(fd);
					finalizeProgressOnFail(progress, totalProgressUnits);
					return false;
				}

				uint64_t remaining = item->uncompressedSize;
				uint64_t offset = dataOffset;

				while (remaining > 0) {
					uint64_t toRead = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
					if (!parser.getFileData(chunk.get(), offset, toRead)) {
						LOGCE("ZipDirectExtractor: failed to read chunk at offset {}", offset);
						close(fd);
						finalizeProgressOnFail(progress, totalProgressUnits);
						return false;
					}

					ssize_t written = write(fd, chunk.get(), toRead);
					if (static_cast<uint64_t>(written) != toRead) {
						LOGCE("ZipDirectExtractor: write failed at offset {}", offset);
						close(fd);
						finalizeProgressOnFail(progress, totalProgressUnits);
						return false;
					}

					offset += toRead;
					remaining -= toRead;

					if (progress) {
						*progress += static_cast<int>(toRead * totalProgressUnits / item->uncompressedSize);
					}
				}

				close(fd);
			}
		}

		if (progress) {
			*progress = static_cast<int>(totalProgressUnits);
		}

		return true;
	}

	bool ZipDirectExtractor::extractEntryUrl(const std::shared_ptr<HttpDownload> &httpDownload,
	                                         const std::string &partitionName,
	                                         const std::string &outputPath,
	                                         std::shared_ptr<std::atomic_int> progress,
	                                         uint64_t totalProgressUnits) {
		// Create ZipParser from HTTP download and parse
		ZipParser parser(httpDownload);
		if (!parser.parse()) {
			LOGCE("ZipDirectExtractor URL: failed to parse ZIP");
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		// Find the entry by partition name
		const ZipFileItem *item = parser.findEntryByPartition(partitionName);
		if (!item) {
			LOGCE("ZipDirectExtractor URL: partition '{}' not found in ZIP", partitionName);
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		// Allocate buffer for the full entry data
		Buffer<uint8_t> data;
		data.reserve(item->uncompressedSize);
		if (!data.get()) {
			LOGCE("ZipDirectExtractor URL: failed to allocate {} bytes", item->uncompressedSize);
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		if (!parser.readEntry(*item, data.get(), item->uncompressedSize, progress, totalProgressUnits)) {
			LOGCE("ZipDirectExtractor URL: failed to read entry '{}'", item->name);
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		// Write to output file
		int fd = open(outputPath.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_BINARY, 0644);
		if (fd < 0) {
			LOGCE("ZipDirectExtractor URL: failed to create output file '{}' ({})", outputPath, strerror(errno));
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		ssize_t written = write(fd, data.get(), item->uncompressedSize);

		if (static_cast<uint64_t>(written) != item->uncompressedSize) {
			LOGCE("ZipDirectExtractor URL: wrote {} bytes, expected {}", written, item->uncompressedSize);
			close(fd);
			finalizeProgressOnFail(progress, totalProgressUnits);
			return false;
		}

		close(fd);

		if (progress) {
			*progress = static_cast<int>(totalProgressUnits);
		}

		return true;
	}
}
