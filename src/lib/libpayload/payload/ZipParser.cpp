#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <vector>

#include <zlib.h>

#include "payload/LogBase.h"
#include "payload/Utils.h"
#include "payload/ZipParser.h"
#include "payload/common/io.h"
#include "payload/common/ioDefs.h"

namespace skkk {
	namespace {
		bool equalsIgnoreCase(std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); ++i) {
				if (std::tolower(static_cast<unsigned char>(a[i])) !=
				    std::tolower(static_cast<unsigned char>(b[i]))) {
					return false;
				}
			}
			return true;
		}

		constexpr uint16_t ZIP_COMPRESSION_STORE = 0;
		constexpr uint16_t ZIP_COMPRESSION_DEFLATE = 8;

		bool inflateBufferToFd(const uint8_t *compressed, size_t compSize, int outFd, uint64_t expectedOut,
		                       int windowBits, uint64_t &outWritten) {
			z_stream strm{};
			if (inflateInit2(&strm, windowBits) != Z_OK) {
				return false;
			}

			constexpr size_t outChunk = 256 * 1024;
			std::vector<uint8_t> outBuf(outChunk);
			strm.next_in = const_cast<Bytef *>(compressed);
			strm.avail_in = static_cast<uInt>(compSize);
			outWritten = 0;
			int inflateRet = Z_OK;

			while (inflateRet != Z_STREAM_END) {
				strm.next_out = outBuf.data();
				strm.avail_out = static_cast<uInt>(outChunk);
				inflateRet = inflate(&strm, Z_NO_FLUSH);
				if (inflateRet != Z_OK && inflateRet != Z_STREAM_END) {
					LOGCE("ZIP: inflate error {} ({})", inflateRet, strm.msg ? strm.msg : "unknown");
					inflateEnd(&strm);
					return false;
				}
				const size_t produced = outChunk - strm.avail_out;
				if (produced > 0) {
					if (blobWrite(outFd, outBuf.data(), outWritten, produced) != 0) {
						inflateEnd(&strm);
						return false;
					}
					outWritten += produced;
				}
				if (inflateRet == Z_OK && produced == 0 && strm.avail_in == 0) {
					break;
				}
			}
			inflateEnd(&strm);
			if (inflateRet != Z_STREAM_END) {
				return false;
			}
			if (outWritten != expectedOut) {
				LOGCE("ZIP: deflate size mismatch, expected {} bytes, got {}", expectedOut, outWritten);
				return false;
			}
			return true;
		}

		bool extractDeflateToFd(const ZipParser &parser, const ZipEntryDataLocation &location, int outFd,
		                        uint64_t uncompressedSize) {
			if (location.compressedSize == 0) {
				LOGCE("ZIP: deflate entry has zero compressed size");
				return false;
			}

			std::vector<uint8_t> compressed;
			if (parser.httpDownload) {
				compressed.resize(static_cast<size_t>(location.compressedSize));
				if (!parser.getFileData(compressed.data(), location.dataOffset, location.compressedSize)) {
					LOGCE("ZIP: failed to download compressed data ({} bytes at offset {})",
					      location.compressedSize, location.dataOffset);
					return false;
				}
			} else {
				constexpr uint64_t bufferThreshold = 256 * 1024 * 1024;
				if (location.compressedSize <= bufferThreshold) {
					compressed.resize(static_cast<size_t>(location.compressedSize));
					if (!parser.getFileData(compressed.data(), location.dataOffset, location.compressedSize)) {
						return false;
					}
				} else {
					LOGCE("ZIP: deflate entry too large for streaming ({} bytes), use a local zip file",
					      location.compressedSize);
					return false;
				}
			}

			uint64_t outWritten = 0;
			if (inflateBufferToFd(compressed.data(), compressed.size(), outFd, uncompressedSize, -MAX_WBITS,
			                      outWritten)) {
				return true;
			}
			outWritten = 0;
			if (payload_ftruncate(outFd, 0) == 0 &&
			    inflateBufferToFd(compressed.data(), compressed.size(), outFd, uncompressedSize, 15, outWritten)) {
				return true;
			}
			LOGCE("ZIP: deflate decompress failed (compressed {} bytes)", location.compressedSize);
			return false;
		}

		bool ensureParentDirs(const std::string &filePath) {
			const auto pos = filePath.find_last_of("/\\");
			if (pos == std::string::npos || pos == 0) {
				return true;
			}
			std::string dir = filePath.substr(0, pos);
			handleWinPath(dir);
			return mkdirs(dir.c_str(), 0755) == 0;
		}

		int extensionPriority(const std::string &filename) {
			const auto dot = filename.find_last_of('.');
			if (dot == std::string::npos) {
				return 0;
			}
			const std::string ext = filename.substr(dot);
			if (equalsIgnoreCase(ext, ".img")) {
				return 3;
			}
			if (equalsIgnoreCase(ext, ".bin")) {
				return 2;
			}
			if (equalsIgnoreCase(ext, ".ext4") || equalsIgnoreCase(ext, ".dat")) {
				return 1;
			}
			return 0;
		}
	}
	ZipParser::ZipParser(const uint8_t *fileData, uint64_t fileSize)
		: fileData(fileData),
		  fileDataSize(fileSize) {
	}

	ZipParser::ZipParser(const std::shared_ptr<HttpDownload> &httpDownload)
		: httpDownload(httpDownload) {
	}

	bool ZipParser::getFileData(uint8_t *data, uint64_t offset, uint64_t len) const {
		if (len == 0) {
			return true;
		}
		if (httpDownload) {
			for (int retry = 0; retry < 4; ++retry) {
				FileBuffer fb{data, 0};
				if (std::get<0>(httpDownload->download(fb, offset, len))) {
					return true;
				}
				if (retry < 3) {
					std::this_thread::sleep_for(std::chrono::milliseconds(800));
				}
			}
			LOGCE("ZIP: HTTP range read failed at offset {} len {}", offset, len);
			return false;
		}
		if (!fileData || offset + len > fileDataSize) {
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
		const uint64_t searchSize = std::min(fileSize - eocdSize, maxEOCDSize);
		uint64_t eocdStartOffset = 0;
		bool foundEOCD = false;
		ZipEOCD eocd{};

		std::string buffer;
		buffer.reserve(maxEOCDSize * 2);
		const auto buf = reinterpret_cast<uint8_t *>(buffer.data());

		if (!getFileData(buf, fileSize - searchSize - eocdSize, searchSize)) return false;
		for (uint64_t offset = 0; offset < searchSize; ++offset) {
			eocdStartOffset = fileSize - eocdSize - offset;
			const auto *tmpEOCD = reinterpret_cast<ZipEOCD *>(buf + searchSize - offset);
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

		if (!getFileData(buf, centralDirOffset, centralDirSize)) return false;
		files.reserve(32);
		uint64_t entryOffset = 0;
		for (uint64_t i = 0; i < totalEntries; ++i) {
			const auto *header = reinterpret_cast<ZipCentralDirFileHeader *>(buf + entryOffset);
			if (header->signature != 0x02014b50) return false;
			entryOffset += sizeof(ZipCentralDirFileHeader);

			const std::string filename{
				reinterpret_cast<char *>(buf) + entryOffset,
				0, header->filenameLength
			};
			entryOffset += header->filenameLength;

			uint64_t uncompressedSize = header->uncompressedSize32;
			uint64_t compressedSize = header->compressedSize32;
			uint64_t localHeaderOffset = header->localHeaderOffset32;

			if (header->extraFieldLength > 0) {
				const auto extra = reinterpret_cast<Zip64ExtendedInfo *>(buf + entryOffset);
				if (extra->headerId == 0x0001) {
					uint32_t dataConsumed = 0;
					auto *data = buf + entryOffset + sizeof(Zip64ExtendedInfo);

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

			files.emplace_back(filename, uncompressedSize, compressedSize,
			                   localHeaderOffset, header->compressionMethod, header->crc32);
		}
		return true;
	}

	std::string ZipParser::getPathBasename(const std::string &path) {
		const auto pos = path.find_last_of("/\\");
		if (pos == std::string::npos) {
			return path;
		}
		return path.substr(pos + 1);
	}

	std::string ZipParser::getFilenameStem(const std::string &filename) {
		const auto dot = filename.find_last_of('.');
		if (dot == std::string::npos || dot == 0) {
			return filename;
		}
		return filename.substr(0, dot);
	}

	bool ZipParser::partitionNameMatchesEntry(const std::string &zipEntryName,
	                                         const std::string &partitionName) {
		if (partitionName.empty()) {
			return false;
		}
		const auto stem = getFilenameStem(getPathBasename(zipEntryName));
		return equalsIgnoreCase(stem, partitionName);
	}

	bool ZipParser::isLikelyDirectExtractEntry(const std::string &zipEntryName) {
		if (zipEntryName.empty()) {
			return false;
		}
		if (zipEntryName.starts_with("META-INF/")) {
			return false;
		}
		const auto basename = getPathBasename(zipEntryName);
		if (basename.empty() || basename.back() == '/') {
			return false;
		}
		const auto stem = getFilenameStem(basename);
		if (stem.empty()) {
			return false;
		}
		static constexpr std::string_view skipNames[] = {
			"android-info",
			"care_map",
			"payload_properties",
			"metadata",
		};
		for (const auto skip: skipNames) {
			if (equalsIgnoreCase(stem, skip)) {
				return false;
			}
		}
		return true;
	}

	const ZipFileItem *ZipParser::findEntryForPartition(const std::string &partitionName) const {
		const ZipFileItem *best = nullptr;
		int bestPriority = -1;
		size_t bestPathLen = SIZE_MAX;
		for (const auto &item: files) {
			if (!isLikelyDirectExtractEntry(item.name) ||
			    !partitionNameMatchesEntry(item.name, partitionName)) {
				continue;
			}
			const int priority = extensionPriority(getPathBasename(item.name));
			const size_t pathLen = item.name.size();
			if (best == nullptr || priority > bestPriority ||
			    (priority == bestPriority && pathLen < bestPathLen)) {
				best = &item;
				bestPriority = priority;
				bestPathLen = pathLen;
			}
		}
		return best;
	}

	std::vector<const ZipFileItem *> ZipParser::findAllDirectExtractEntries() const {
		std::vector<const ZipFileItem *> result;
		for (const auto &item: files) {
			if (isLikelyDirectExtractEntry(item.name)) {
				result.push_back(&item);
			}
		}
		return result;
	}

	bool ZipParser::resolveEntryDataLocation(const ZipFileItem &item, ZipEntryDataLocation &location) const {
		ZipLocalHeader localHeader{};
		if (!getFileData(reinterpret_cast<uint8_t *>(&localHeader), item.localHeaderOffset,
		                 sizeof(ZipLocalHeader))) {
			return false;
		}
		if (localHeader.signature != 0x04034b50) {
			return false;
		}

		location.compressionMethod = localHeader.compressionMethod;
		location.compressedSize = localHeader.compressedSize;
		location.uncompressedSize = localHeader.uncompressedSize;
		uint64_t extraConsumed = 0;

		if (localHeader.extraFieldLength > 0) {
			std::string extra;
			extra.resize(localHeader.extraFieldLength);
			const uint64_t extraOffset = item.localHeaderOffset + sizeof(ZipLocalHeader) +
			                             localHeader.filenameLength;
			if (!getFileData(reinterpret_cast<uint8_t *>(extra.data()), extraOffset,
			                 localHeader.extraFieldLength)) {
				return false;
			}
			uint32_t pos = 0;
			while (pos + 4 <= extra.size()) {
				const auto headerId = *reinterpret_cast<const uint16_t *>(extra.data() + pos);
				const auto dataSize = *reinterpret_cast<const uint16_t *>(extra.data() + pos + 2);
				pos += 4;
				if (pos + dataSize > extra.size()) {
					break;
				}
				if (headerId == 0x0001) {
					uint32_t dataPos = 0;
					if (location.uncompressedSize == 0xFFFFFFFF && dataPos + 8 <= dataSize) {
						location.uncompressedSize = *reinterpret_cast<const uint64_t *>(extra.data() + pos + dataPos);
						dataPos += 8;
					}
					if (location.compressedSize == 0xFFFFFFFF && dataPos + 8 <= dataSize) {
						location.compressedSize = *reinterpret_cast<const uint64_t *>(extra.data() + pos + dataPos);
						dataPos += 8;
					}
					extraConsumed = dataSize;
					(void)extraConsumed;
				}
				pos += dataSize;
			}
		}

		location.dataOffset = item.localHeaderOffset + sizeof(ZipLocalHeader) +
		                      localHeader.filenameLength + localHeader.extraFieldLength;
		if (location.compressionMethod == 0 && location.compressedSize == 0 &&
		    location.uncompressedSize > 0) {
			location.compressedSize = location.uncompressedSize;
		}
		if (location.uncompressedSize == 0) {
			location.uncompressedSize = item.uncompressedSize;
		}
		if (location.compressedSize == 0) {
			location.compressedSize = item.compressedSize;
		}
		if (location.compressionMethod == 0 && item.compression != 0) {
			location.compressionMethod = static_cast<uint16_t>(item.compression);
		}
		return location.uncompressedSize > 0 && location.compressedSize > 0;
	}

	bool ZipParser::extractEntryToFile(const ZipFileItem &item, const std::string &outPath) const {
		if (!ensureParentDirs(outPath)) {
			LOGCE("ZIP: failed to create parent directory for '{}'", outPath);
			return false;
		}
		ZipEntryDataLocation location{};
		if (!resolveEntryDataLocation(item, location)) {
			LOGCE("ZIP: failed to resolve entry offset for '{}'", item.name);
			return false;
		}
		LOGCD("ZIP: extract '{}' method={} compressed={} uncompressed={} offset={}",
		      item.name, location.compressionMethod, location.compressedSize,
		      location.uncompressedSize, location.dataOffset);
		if (location.compressionMethod != ZIP_COMPRESSION_STORE &&
		    location.compressionMethod != ZIP_COMPRESSION_DEFLATE) {
			LOGCE("ZIP: '{}' uses compression method {}, only stored (0) and deflate (8) are supported",
			      item.name, location.compressionMethod);
			return false;
		}
		const uint64_t size = location.uncompressedSize;
		if (size == 0) {
			LOGCE("ZIP: '{}' is empty", item.name);
			return false;
		}

		int outFd = open(outPath.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_BINARY, 0644);
		if (outFd <= 0 || payload_ftruncate(outFd, static_cast<off64_t>(size))) {
			if (outFd > 0) {
				closeFd(outFd);
			}
			LOGCE("ZIP: failed to create '{}'", outPath);
			return false;
		}

		bool ok = false;
		if (location.compressionMethod == ZIP_COMPRESSION_STORE) {
			constexpr uint64_t chunkSize = 4 * 1024 * 1024;
			std::string buffer;
			buffer.resize(static_cast<size_t>(std::min(chunkSize, size)));
			uint64_t written = 0;
			ok = true;
			while (written < size) {
				const uint64_t toRead = std::min(chunkSize, size - written);
				if (!getFileData(reinterpret_cast<uint8_t *>(buffer.data()),
				                 location.dataOffset + written, toRead)) {
					ok = false;
					break;
				}
				if (blobWrite(outFd, buffer.data(), written, toRead)) {
					ok = false;
					break;
				}
				written += toRead;
			}
		} else {
			ok = extractDeflateToFd(*this, location, outFd, size);
		}
		closeFd(outFd);
		if (!ok) {
			LOGCE("ZIP: failed to extract '{}' to '{}'", item.name, outPath);
		}
		return ok;
	}
}
