#include <cctype>

#include "payload/Utils.h"
#include "payload/ZipParser.h"
#include "payload/common/io.h"
#include "payload/common/Buffer.hpp"
#include "payload/LogBase.h"
#include "decompress/Decompress.h"

namespace skkk {
	ZipParser::ZipParser(const uint8_t *fileData, uint64_t fileSize)
		: fileData(fileData),
		  fileDataSize(fileSize) {
	}

	ZipParser::ZipParser(const std::shared_ptr<HttpDownload> &httpDownload)
		: httpDownload(httpDownload) {
	}

	bool ZipParser::getFileData(uint8_t *data, uint64_t offset, uint64_t len) const {
		if (httpDownload) {
			FileBuffer fb{data, 0};
			return std::get<0>(httpDownload->download(fb, offset, len));
		}
		// Bounds check for mmap path
		if (offset > fileDataSize || len > fileDataSize - offset) {
			return false;
		}
		return memcpy(data, fileData + offset, len) == data;
	}

	uint64_t ZipParser::getZipFileSize() const {
		if (httpDownload) {
			return httpDownload->getFileSize();
		}
		return fileDataSize;
	}

	bool ZipParser::parse() {
		constexpr uint64_t eocdSize = sizeof(ZipEOCD);
		const uint64_t fileSize = getZipFileSize();
		if (fileSize < eocdSize) return false;

		constexpr uint64_t maxEOCDSize = 4096;
		const uint64_t searchSize = std::min(fileSize, maxEOCDSize + eocdSize);
		uint64_t eocdStartOffset = 0;
		bool foundEOCD = false;
		ZipEOCD eocd{};

		std::string buffer;
		buffer.reserve(maxEOCDSize * 2);
		const auto buf = reinterpret_cast<uint8_t *>(buffer.data());

		if (!getFileData(buf, fileSize - searchSize, searchSize)) return false;
		for (uint64_t offset = 0; offset <= searchSize - eocdSize; ++offset) {
			eocdStartOffset = fileSize - eocdSize - offset;
			const auto *tmpEOCD = reinterpret_cast<ZipEOCD *>(buf + searchSize - eocdSize - offset);
			if (tmpEOCD->signature == 0x06054b50 && tmpEOCD->commentLength == offset) {
				memcpy(&eocd, tmpEOCD, eocdSize);
				foundEOCD = true;
				break;
			}
		}

		if (!foundEOCD) {
			return false;
		}

		const bool isZip64 = eocd.totalEntries == 0xFFFF ||
		                     eocd.centralDirOffset == 0xFFFFFFFF ||
		                     eocd.numEntriesThisDisk == 0xFFFF;

		Zip64EOCDLocator eocd64Locator{};
		if (isZip64) {
			const uint64_t locatorPos = eocdStartOffset - sizeof(Zip64EOCDLocator);
			if (!getFileData(reinterpret_cast<uint8_t *>(&eocd64Locator),
			                 locatorPos, sizeof(Zip64EOCDLocator)))
				return false;
			if (eocd64Locator.signature != 0x07064b50) {
				return false;
			}
		}

		uint64_t centralDirOffset = isZip64 ? eocd64Locator.eocd64Offset : eocd.centralDirOffset;
		Zip64EOCD eocd64{};
		if (isZip64) {
			if (!getFileData(reinterpret_cast<uint8_t *>(&eocd64),
			                 centralDirOffset, sizeof(Zip64EOCD)))
				return false;
			if (eocd64.signature != 0x06064b50) {
				return false;
			}

			centralDirOffset = eocd64.centralDirOffset;
		}

		const uint64_t totalEntries = isZip64 ? eocd64.totalEntries : eocd.totalEntries;
		const uint64_t centralDirSize = isZip64 ? eocd64.centralDirSize : eocd.centralDirSize;

		// Allocate separate buffer for central directory (can be much larger than 8192)
		Buffer<uint8_t> cdBuf;
		cdBuf.reserve(centralDirSize);
		if (!cdBuf.get()) {
			LOGCE("ZipParser: failed to allocate {} bytes for central directory", centralDirSize);
			return false;
		}

		if (!getFileData(cdBuf.get(), centralDirOffset, centralDirSize)) return false;
		files.reserve(32);
		uint64_t entryOffset = 0;
		for (uint64_t i = 0; i < totalEntries; ++i) {
			const auto *header = reinterpret_cast<ZipCentralDirFileHeader *>(cdBuf.get() + entryOffset);
			if (header->signature != 0x02014b50) return false;
			entryOffset += sizeof(ZipCentralDirFileHeader);

			const std::string filename{
				reinterpret_cast<char *>(cdBuf.get()) + entryOffset,
				0, header->filenameLength
			};
			entryOffset += header->filenameLength;

			uint64_t uncompressedSize = header->uncompressedSize32;
			uint64_t compressedSize = header->compressedSize32;
			uint64_t localHeaderOffset = header->localHeaderOffset32;

			if (header->extraFieldLength > 0) {
				const auto extra = reinterpret_cast<Zip64ExtendedInfo *>(cdBuf.get() + entryOffset);
				if (extra->headerId == 0x0001) {
					uint32_t dataConsumed = 0;
					auto *data = cdBuf.get() + entryOffset + sizeof(Zip64ExtendedInfo);

					if (uncompressedSize == 0xFFFFFFFF && dataConsumed + 8 <= extra->dataSize) {
						uncompressedSize = *reinterpret_cast<uint64_t *>(data + dataConsumed);
						dataConsumed += 8;
					}

					if (compressedSize == 0xFFFFFFFF && dataConsumed + 8 <= extra->dataSize) {
						compressedSize = *reinterpret_cast<uint64_t *>(data + dataConsumed);
						dataConsumed += 8;
					}

					if (localHeaderOffset == 0xFFFFFFFF && dataConsumed + 8 <= extra->dataSize) {
						localHeaderOffset = *reinterpret_cast<uint64_t *>(data + dataConsumed);
						dataConsumed += 8;
					}
				}
				entryOffset += header->extraFieldLength;
			}
			entryOffset += header->commentLength;

			files.emplace_back(filename, compressedSize, uncompressedSize,
			                   localHeaderOffset, header->compressionMethod, header->crc32);
		}
		return true;
	}

	bool ZipParser::getEntryDataOffset(const ZipFileItem &item, uint64_t &dataOffset, uint64_t &compressedDataSize) const {
		ZipLocalHeader localHeader;
		if (!getFileData(reinterpret_cast<uint8_t *>(&localHeader), item.localHeaderOffset, sizeof(ZipLocalHeader)))
			return false;
		if (localHeader.signature != 0x04034b50)
			return false;

		uint16_t filenameLength = localHeader.filenameLength;
		uint16_t extraFieldLength = localHeader.extraFieldLength;

		dataOffset = item.localHeaderOffset + sizeof(ZipLocalHeader) + filenameLength + extraFieldLength;
		compressedDataSize = item.compressedSize;
		return true;
	}

	bool ZipParser::readEntry(const ZipFileItem &item, uint8_t *output, uint64_t outputSize,
	                          std::shared_ptr<std::atomic_int> progress, uint64_t totalProgressUnits) {
		uint64_t dataOffset = 0;
		uint64_t compressedDataSize = 0;

		if (!getEntryDataOffset(item, dataOffset, compressedDataSize)) {
			return false;
		}

		if (item.compression == 0) {
			return getFileData(output, dataOffset, outputSize);
		}

		if (item.compression == 8) {
			if (compressedDataSize == 0) {
				LOGCE("readEntry: compressedDataSize is 0 for '{}'", item.name);
				return false;
			}
			Buffer<uint8_t> compressed;
			compressed.reserve(compressedDataSize);
			if (!compressed.get()) {
				LOGCE("readEntry: failed to allocate {} bytes for '{}'", compressedDataSize, item.name);
				return false;
			}
			// Download compressed data in 1MB chunks with progress
			constexpr uint64_t CHUNK_SIZE = 1ULL * 1024 * 1024; // 1MB
			uint64_t remaining = compressedDataSize;
			uint64_t srcOffset = dataOffset;
			uint8_t *dstPtr = compressed.get();
			while (remaining > 0) {
				uint64_t toRead = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
				if (!getFileData(dstPtr, srcOffset, toRead)) {
					LOGCE("readEntry: getFileData failed for '{}' (offset={}, size={})", item.name, srcOffset, toRead);
					return false;
				}
				dstPtr += toRead;
				srcOffset += toRead;
				remaining -= toRead;
				if (progress && totalProgressUnits > 0) {
					uint64_t downloaded = compressedDataSize - remaining;
					*progress = static_cast<int>(downloaded * totalProgressUnits / compressedDataSize);
				}
			}
			int ret = Decompress::inflateDecompress(compressed.get(), compressedDataSize, output, outputSize);
			if (ret != 0) {
				LOGCE("readEntry: inflateDecompress failed for '{}' (ret={})", item.name, ret);
				return false;
			}
			return true;
		}

		return false;
	}

	const ZipFileItem *ZipParser::findEntryByPartition(const std::string &partitionName) const {
		const ZipFileItem *bestMatch = nullptr;

		for (const auto &file : files) {
			// Extract base filename (without directory path)
			size_t lastSlash = file.name.find_last_of("/\\");
			std::string basename = (lastSlash == std::string::npos) ? file.name : file.name.substr(lastSlash + 1);

			// Extract name without extension
			size_t lastDot = basename.find_last_of('.');
			std::string nameWithoutExt = (lastDot == std::string::npos) ? basename : basename.substr(0, lastDot);

			// Case-insensitive comparison
			auto ciEqual = [](const std::string &a, const std::string &b) -> bool {
				if (a.size() != b.size()) return false;
				for (size_t i = 0; i < a.size(); i++) {
					if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
						return false;
				}
				return true;
			};

			if (ciEqual(nameWithoutExt, partitionName)) {
				// Prefer exact match: partitionName + ".img"
				std::string expectedName = partitionName + ".img";
				if (ciEqual(basename, expectedName)) {
					return &file; // Best possible match, return immediately
				}
				// Otherwise, remember first match
				if (bestMatch == nullptr) {
					bestMatch = &file;
				}
			}
		}

		return bestMatch;
	}
}
