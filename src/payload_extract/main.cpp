#include <algorithm>
#include <cctype>
#include <cstdio>
#include <future>
#include <getopt.h>
#include <print>
#include <ranges>
#include <string>
#include <sys/time.h>

#include <payload/ExtractConfig.h>
#include <payload/LogBase.h>
#include <payload/PartitionWriter.h>
#include <payload/PayloadParser.h>
#include <payload/Utils.h>
#include <payload/ZipDirectExtractor.h>
#include <payload/common/LogProgress.h>
#include <payload/verify/VerifyWriter.h>

#include "ExtractOperation.h"
#include "RemoteUpdater.h"

using namespace skkk;

static void usage(const ExtractOperation &eo) {
	char buf[4096] = {};
	snprintf(buf, sizeof(buf) - 1,
			 BROWN("usage: [options]") "\n"
			 "  " GREEN2_BOLD("-h, --help") "           " BROWN("Display this help and exit") "\n"
			 "  " GREEN2_BOLD("-i, --input=[PATH]") "   " BROWN("File path or URL") "\n"
			 "  " GREEN2_BOLD("--incremental=X") "      " BROWN("Old directory, Catalog requiring incremental patching") "\n"
			 "  " GREEN2_BOLD("--verify-update") "      " BROWN("  In the incremental mode, The dm-verify verified file") "\n"
			 "  "             "               "       "      " BROWN("  does not contain HASH_TREE and FEC. Only files that") "\n"
			 "  "             "               "       "      " BROWN("  have successfully updated this information can undergo") "\n"
			 "  "             "               "       "      " BROWN("  SHA256 verification.") "\n"
			 "  " GREEN2_BOLD("--verify-update=X") "    " BROWN("  Only Verify and update the specified targets: [boot,odm,...]") "\n"
			 "  " GREEN2_BOLD("-p") "                   " BROWN("Print all info") "\n"
			 "  " GREEN2_BOLD("-P, --print=X") "        " BROWN("Print the specified targets: [boot,odm,...]") "\n"
	         "  " GREEN2_BOLD("-x") "                   " BROWN("Extract all items") "\n"
	         "  " GREEN2_BOLD("-X, --extract=X") "      " BROWN("Extract the specified targets: [boot,odm,...]") "\n"
	         "  " GREEN2_BOLD("-e") "                   " BROWN("Exclude mode, exclude specific targets") "\n"
	         "  " GREEN2_BOLD("-s") "                   " BROWN("Silent mode, Don't show progress") "\n"
	         "  " GREEN2_BOLD("-T#") "                  " BROWN("[") GREEN2_BOLD("1-%u") BROWN("] Use # threads, default: -T0, is ") GREEN2_BOLD("%u") "\n"
	         "  " GREEN2_BOLD("-k") "                   " BROWN("Skip SSL verification") "\n"
	         "  " GREEN2_BOLD("-o, --outdir=X") "       " BROWN("Output dir") "\n"
	         "  " GREEN2_BOLD("--out-config=X") "       " BROWN("Output config file, One config per line: [boot:/path/to/xxx]") "\n"
	         "  " GREEN2_BOLD("-R") "                   " BROWN("Modify the URL in the remote config") "\n"
	         "  "             "               "       "      " BROWN("  May need to specify the output directory") "\n"
	         "  " GREEN2_BOLD("-V, --version") "        " BROWN("Print the version info") "\n",
	         eo.limitHardwareConcurrency,
	         eo.hardwareConcurrency
	);
	std::println( "{}", buf);
}

#ifndef PAYLOAD_EXTRACT_VERSION
#define PAYLOAD_EXTRACT_VERSION "v0.0.0"
#endif
#ifndef PAYLOAD_EXTRACT_BUILD_TIME
#define PAYLOAD_EXTRACT_BUILD_TIME "-0000000000"
#endif

static void printVersion() {
	printf("  " BROWN("payload_extract:") "     " RED2_BOLD(PAYLOAD_EXTRACT_VERSION PAYLOAD_EXTRACT_BUILD_TIME) "\n");
	printf("  " BROWN("author:") "              " RED2_BOLD("skkk") "\n");
}

static option argOptions[] = {
	{"help", no_argument, nullptr, 'h'},
	{"version", no_argument, nullptr, 'V'},
	{"input", required_argument, nullptr, 'i'},
	{"outdir", required_argument, nullptr, 'o'},
	{"print", required_argument, nullptr, 'P'},
	{"extract", required_argument, nullptr, 'X'},
	{"incremental", required_argument, nullptr, 200},
	{"verify-update", optional_argument, nullptr, 201},
	{"out-config",required_argument, nullptr, 202},
	{nullptr, no_argument, nullptr, 0},
};

static int parseExtractOperation(const int argc, char **argv, ExtractOperation &eo) {
	int opt, ret = RET_EXTRACT_CONFIG_FAIL;
	bool enterCheckOpt = false;
	while ((opt = getopt_long(argc, argv, "ehi:ko:pst:xP:T:VX:R", argOptions, nullptr)) != -1) {
		enterCheckOpt = true;
		switch (opt) {
			case 'h':
				usage(eo);
				goto exit;
			case 'V':
				printVersion();
				goto exit;
			case 'i':
				if (optarg) {
					eo.setPayloadPath(optarg);
				}
				LOGCD("path={}", eo.getPayloadPath());
				break;
			case 'k':
				eo.sslVerification = false;
				LOGCD("Skip SSL verification={}", eo.sslVerification);
				break;
			case 'o':
				if (optarg) {
					eo.setOutDir(optarg);
				}
				LOGCD("outDir={}", eo.getOutDir());
				break;
			case 'p':
				eo.isPrintAll = true;
				LOGCD("isPrintAllNode={}", eo.isPrintAll);
				break;
			case 'P':
				eo.isPrintTarget = true;
				if (optarg) eo.setTargetName(optarg);
				LOGCD("isPrintTarget={} targetPath={}", eo.isPrintTarget, eo.getTargetName());
				break;
			case 's':
				eo.isSilent = true;
				LOGCD("isSilent={}", eo.isSilent);
				break;
			case 'x':
				eo.isExtractAll = true;
				LOGCD("isExtractAll={}", eo.isExtractAll);
				break;
			case 'X':
				eo.isExtractTarget = true;
				if (optarg) eo.setTargetName(optarg);
				LOGCD("isExtractTarget={} targetName={}", eo.isExtractTarget, eo.getTargetName().c_str());
				break;
			case 'e':
				eo.isExcludeMode = true;
				LOGCD("isExcludeMode={}", eo.isExcludeMode);
				break;
			case 'T':
				if (optarg) {
					char *endPtr;
					uint64_t n = strtoull(optarg, &endPtr, 0);
					if (*endPtr == '\0') {
						eo.threadNum = n;
					}
				}
				break;
			case 'R':
				eo.remoteUpdate = true;
				LOGCD("remoteUpdate={}", eo.remoteUpdate);
				break;
			case 200:
				eo.isIncremental = true;
				if (optarg) {
					eo.isIncremental = true;
					eo.setOldDir(optarg);
				}
				LOGCD("isIncremental={} oldDir={}", eo.isIncremental, eo.getOldDir());
				break;
			case 201:
				eo.isVerifyUpdate = true;
				if (eo.getTargetName().empty()) {
					if (optarg) {
						eo.setTargetName(optarg);
					}
				}
				LOGCD("isVerifyUpdate={} oldDir={}", eo.isVerifyUpdate, eo.getOldDir());
				break;
			case 202:
				if (optarg) {
					eo.setOutConfigPath(optarg);
				}
				LOGCD("outConfigPath={}", eo.getOutConfigPath());
				break;
			default:
				usage(eo);
				printVersion();
				goto exit;
		}
	}

	if (enterCheckOpt) {
		if (eo.getPayloadPath().empty()) {
			ret = RET_EXTRACT_OPEN_FILE;
			goto exit;
		}

		eo.handleUrl();
		LOGCD("isUrl={}", eo.isUrl);

		eo.initHttpDownload();
		LOGCD("httpDownload={}", eo.httpDownload != nullptr);

		if (eo.payloadType != PAYLOAD_TYPE_URL) {
			if (!fileExists(eo.getPayloadPath())) {
				LOGCE("payload file '{}' does not exist", eo.getPayloadPath().c_str());
				ret = RET_EXTRACT_OPEN_FILE;
				goto exit;
			}
		}

		if (eo.isIncremental) {
			ret = eo.initOldDir();
			if (ret) goto exit;
		}

		ret = eo.initOutDir();
		if (ret) goto exit;

		if (!eo.getOutConfigPath().empty()) {
			ret = eo.initOutConfig();
			if (ret) goto exit;
		}

		if (!eo.getTargetName().empty()) {
			ret = eo.initTargetNames();
			if (ret) goto exit;
		}

		if (eo.threadNum > eo.limitHardwareConcurrency) {
			ret = RET_EXTRACT_THREAD_NUM_ERROR;
			LOGCE("Threads min: 1 , max: {}", eo.limitHardwareConcurrency);
			goto exit;
		}
		if (eo.threadNum == 0) {
			eo.threadNum = eo.hardwareConcurrency;
		}
		LOGCD("Threads num={}", eo.threadNum);
		ret = RET_EXTRACT_CONFIG_DONE;
	} else {
		usage(eo);
	}
exit:
	return ret;
}

static void printOperationTime(const timeval *start, const timeval *end) {
	LOGCI(GREEN2_BOLD("The operation took: ") RED2("{:.3f}") "{}",
	      (end->tv_sec - start->tv_sec) + static_cast<float>(end->tv_usec - start->tv_usec) / 1000000,
	      GREEN2_BOLD(" second(s)."));
}

#if defined(_WIN32)
#include <windows.h>

static void handleWinTerminal() {
	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
	if (hStdin != INVALID_HANDLE_VALUE) {
		DWORD mode;
		if (GetConsoleMode(hStdin, &mode)) {
			mode &= ~ENABLE_QUICK_EDIT_MODE;
			mode &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			mode &= ~ENABLE_MOUSE_INPUT;
			SetConsoleMode(hStdin, mode);
		}
	}
}

static void enableWinTerminalColor(DWORD handle) {
	HANDLE nHandle = GetStdHandle(handle);
	if (nHandle != INVALID_HANDLE_VALUE) {
		DWORD mode;
		if (GetConsoleMode(nHandle, &mode)) {
			mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			SetConsoleMode(nHandle, mode);
		}
	}
}
#endif

int main(const int argc, char *argv[]) {
	int ret = RET_EXTRACT_DONE;
	bool err = false;
	timeval start{}, end{};

#if defined(_WIN32)
	handleWinTerminal();
	enableWinTerminalColor(STD_OUTPUT_HANDLE);
	enableWinTerminalColor(STD_ERROR_HANDLE);
#endif

	setbuf(stdout, nullptr);
	setbuf(stderr, nullptr);

	// LOG
	LOG_TAG("Extract");

	// Start time
	gettimeofday(&start, nullptr);

	// Config
	ExtractOperation eo;
	PayloadParser payloadParser;
	std::shared_ptr<RemoteUpdater> ru;
	std::shared_ptr<PartitionWriter> pw;
	std::shared_ptr<VerifyWriter> vw;
	std::shared_ptr<PayloadInfo> payloadInfo;
	if (parseExtractOperation(argc, argv, eo) != RET_EXTRACT_CONFIG_DONE) {
		ret = RET_EXTRACT_INIT_FAIL;
		goto exit;
	}

	// RemoteUpdater
	ru = std::make_shared<RemoteUpdater>(eo);
	if (eo.remoteUpdate) {
		if (!ru->initRemoteUpdate(false)) {
			ret = RET_EXTRACT_INIT_FAIL;
			goto exit;
		}
		ru->notifyRemoteUpdate();
		goto exit;
	}

	// Parse payload.bin
	if (!payloadParser.parse(eo)) {
		ret = RET_EXTRACT_INIT_FAIL;
		goto exit;
	}

	payloadInfo = payloadParser.getPayloadInfo();

	// Direct ZIP extraction mode (no payload.bin found in ZIP)
	if (!payloadInfo->hasValidPayload) {
		// Print ZIP entries if requested
		if (eo.isPrintAll || eo.isPrintTarget) {
			LOGCI("ZIP entries available:");
			for (const auto &file : payloadInfo->zipFiles) {
				std::cout << std::format("  {:20} size: {}", file.name, file.uncompressedSize) << std::endl;
			}
			if (eo.isPrintTarget || eo.isPrintAll) {
				goto exit;
			}
		}

		if (eo.isExtractAll || eo.isExtractTarget) {
			err = eo.createExtractOutDir();
			if (err) {
				ret = RET_EXTRACT_CREATE_DIR_FAIL;
				goto exit;
			}

			// Build target list from ZIP entries
			std::vector<std::string> targets;
			if (!eo.getTargetName().empty() && !eo.isExcludeMode) {
				targets = eo.getTargets();
			} else if (eo.isExtractAll) {
				for (const auto &file : payloadInfo->zipFiles) {
					size_t lastSlash = file.name.find_last_of("/\\");
					std::string basename = (lastSlash == std::string::npos) ? file.name : file.name.substr(lastSlash + 1);
					size_t lastDot = basename.find_last_of('.');
					std::string nameWithoutExt = (lastDot == std::string::npos) ? basename : basename.substr(0, lastDot);
					// Lowercase for case-insensitive dedup
					std::transform(nameWithoutExt.begin(), nameWithoutExt.end(), nameWithoutExt.begin(),
					               [](char c) { return std::tolower(static_cast<unsigned char>(c)); });
					targets.push_back(std::move(nameWithoutExt));
				}
				// Remove duplicates
				std::sort(targets.begin(), targets.end());
				targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
			}

			if (targets.empty()) {
				LOGCE("No targets specified for direct extraction");
				ret = RET_EXTRACT_INIT_PART_FAIL;
				goto exit;
			}

			LOGCI(GREEN2_BOLD("Starting..."));

			// Extract each target
			for (const auto &target : targets) {
				// Check exclude mode
				if (eo.isExcludeMode && std::ranges::find(eo.getTargets(), target) != eo.getTargets().end()) {
					LOGCI("Skipping excluded target: {}", target);
					continue;
				}

				// Determine output path from --out-config or default
				std::string outPath;
				auto configIt = eo.getOutConfig().find(target);
				if (configIt != eo.getOutConfig().end()) {
					outPath = configIt->second;
				} else {
					outPath = eo.getOutDir() + "/" + target + ".img";
				}

				// Create progress tracking
				auto progress = std::make_shared<std::atomic_int>(0);

				// Estimate uncompressed size for progress display
				uint64_t estimatedSize = 0;
				for (const auto &zipFile : payloadInfo->zipFiles) {
					size_t lastSlash = zipFile.name.find_last_of("/\\");
					std::string basename = (lastSlash == std::string::npos) ? zipFile.name : zipFile.name.substr(lastSlash + 1);
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
					if (ciEqual(nameWithoutExt, target)) {
						estimatedSize = zipFile.uncompressedSize;
						break;
					}
				}
				// Launch progress display thread
				std::future<void> progressThread;
				progressThread = std::async(std::launch::async, [isSilent = eo.isSilent, target = target, estimatedSize = estimatedSize, progress]() {
					printProgressMT(isSilent, target, estimatedSize, 100, *progress, true);
				});

				bool success = false;
				if (eo.isUrl && eo.httpDownload) {
					success = ZipDirectExtractor::extractEntryUrl(
						eo.httpDownload, target, outPath, progress, 100);
				} else {
					success = ZipDirectExtractor::extractEntry(
						payloadInfo->getPayloadData(), payloadInfo->getPayloadSize(),
						target, outPath, progress, 100);
				}

				if (progressThread.valid()) progressThread.wait();

				if (success) {
					LOGCI("{:18} result: {}", target, GREEN2_BOLD("success"));
				} else {
					LOGCI("{:18} result: {}", target, RED2_BOLD("failed"));
				}
			}

			goto end;
		}

		goto end;
	}

	// PartitionWriter
	pw = payloadParser.getPartitionWriter();

	if (!eo.getTargetName().empty()) {
		err = pw->initPartitionsByTarget();
	} else if (eo.isPrintAll || eo.isExtractAll) {
		err = pw->initPartitions();
	}
	if (!err) {
		ret = RET_EXTRACT_INIT_PART_FAIL;
		LOGCE("Cannot find the image file to be extracted!");
		goto exit;
	}

	// VerifyWriter
	vw = pw->getVerifyWriter();
	vw->initHashTreeLevel();

	if (eo.isPrintTarget || eo.isPrintAll) {
		pw->printPartitionsInfo();
		goto exit;
	}

	LOGCI(GREEN2_BOLD("Starting..."));

	if (eo.isExtractAll || eo.isExtractTarget) {
		err = eo.createExtractOutDir();
		if (err) {
			ret = RET_EXTRACT_CREATE_DIR_FAIL;
			goto exit;
		}

		if (eo.isUrl) {
			if (!ru->initRemoteUpdate(true)) {
				ret = RET_EXTRACT_INIT_FAIL;
				goto exit;
			}
			ru->startMonitor();
		}

		pw->extractPartitions();

		if (eo.isIncremental && eo.isVerifyUpdate) {
			vw->updateVerifyData();
		}
		goto end;
	}

	if (eo.isIncremental && eo.isVerifyUpdate) {
		vw->updateVerifyData();
		goto end;
	}

end:
	// End time
	gettimeofday(&end, nullptr);
	printOperationTime(&start, &end);

exit:
	return ret;
}
