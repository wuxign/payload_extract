#include <brotli/decode.h>
#include <bsdiff/bspatch.h>
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include "Decompress.h"

namespace skkk {
	int Decompress::brotliDecompress(const void *src, uint64_t srcSize, void *destBuf, uint64_t destSize) {
		size_t dstSize = destSize;
		BrotliDecoderResult bret = BrotliDecoderDecompress(srcSize, static_cast<const uint8_t *>(src),
		                                                   &dstSize, static_cast<uint8_t *>(destBuf));
		if (bret == BROTLI_DECODER_RESULT_SUCCESS) {
			return 0;
		}
		return -EBADMSG;
	}

	int Decompress::bzipDecompress(const void *src, uint64_t srcSize, void *destBuf, uint64_t destSize) {
		int ret = 0;
		bz_stream strm = {};
		int err = BZ2_bzDecompressInit(&strm, 0, 0);
		if (err != BZ_OK) {
			ret = -EFAULT;
			goto out;
		}
		strm.next_in = const_cast<char *>(static_cast<const char *>(src));
		strm.avail_in = srcSize;
		strm.next_out = static_cast<char *>(destBuf);
		strm.avail_out = destSize;
		err = BZ2_bzDecompress(&strm);
		if (err != BZ_STREAM_END) {
			ret = -EBADMSG;
			goto out_bzip_end;
		}
		// destSize = strm.total_out_lo32;

	out_bzip_end:
		BZ2_bzDecompressEnd(&strm);
	out:
		return ret;
	}

	int Decompress::xzDecompress(const void *src, uint64_t srcSize, void *destBuf, uint64_t destSize) {
		int ret = 0;
		lzma_stream strm = LZMA_STREAM_INIT;
		lzma_ret err = lzma_stream_decoder(&strm, MaxDictSize, LZMA_CONCATENATED);
		if (err != LZMA_OK) {
			ret = -EFAULT;
			goto out;
		}
		strm.next_in = static_cast<const uint8_t *>(src);
		strm.avail_in = srcSize;
		strm.next_out = static_cast<uint8_t *>(destBuf);
		strm.avail_out = destSize;
		err = lzma_code(&strm, LZMA_FINISH);
		if (err != LZMA_STREAM_END) {
			ret = -EBADMSG;
			goto out_lzma_end;
		}
		destSize = strm.total_out;

	out_lzma_end:
		lzma_end(&strm);
	out:
		return ret;
	}

	int Decompress::zstdDecompress(const void *src, uint64_t srcSize, void *destBuf, uint64_t destSize) {
		int ret = 0;
		ret = ZSTD_decompress(destBuf, destSize, src, srcSize);
		if (ZSTD_isError(ret)) {
			ret = -EBADMSG;
			goto out;
		}
		ret = 0;
	out:
		return ret;
	}

	int Decompress::zipDeflateDecompress(const void *src, uint64_t srcSize, void *destBuf, uint64_t destSize) {
		z_stream strm{};
		int err = inflateInit2(&strm, -MAX_WBITS);
		if (err != Z_OK) {
			return -EFAULT;
		}
		strm.next_in = const_cast<Bytef *>(static_cast<const Bytef *>(src));
		strm.avail_in = static_cast<uInt>(srcSize > UINT_MAX ? UINT_MAX : srcSize);
		strm.next_out = static_cast<Bytef *>(destBuf);
		strm.avail_out = static_cast<uInt>(destSize > UINT_MAX ? UINT_MAX : destSize);
		err = inflate(&strm, Z_FINISH);
		const bool ok = err == Z_STREAM_END && strm.total_out == destSize;
		inflateEnd(&strm);
		return ok ? 0 : -EBADMSG;
	}
}
