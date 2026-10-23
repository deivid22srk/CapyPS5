// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only


#include "common/logging/log.h"

#include "common/assert.h"
#include "common/debugFlags.h"
#include "common/emulatorConfig.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <spdlog/formatter.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <string_view>

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
#include <android/log.h>
#endif

namespace {

class RawFormatter final: public spdlog::formatter {
public:
	void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
		const std::string_view payload(msg.payload.data(), msg.payload.size());
		dest.append(payload.data(), payload.data() + payload.size());
	}

	std::unique_ptr<spdlog::formatter> clone() const override {
		return std::make_unique<RawFormatter>();
	}
};

std::shared_ptr<spdlog::logger> MakeLogger(std::string name, spdlog::sink_ptr sink) {
	sink->set_formatter(std::make_unique<RawFormatter>());

	auto logger = std::make_shared<spdlog::logger>(std::move(name), std::move(sink));
	logger->set_level(spdlog::level::trace);
	return logger;
}

std::shared_ptr<spdlog::logger> MakeFileLogger(std::string                  name,
                                               const std::filesystem::path& path,
                                               bool                         truncate) {
	const auto parent = path.parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent);
	}

	auto sink =
	    std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.native(), truncate);
	return MakeLogger(std::move(name), std::move(sink));
}

static bool HasStyle(fmt::text_style style) {
	return style != fmt::text_style {};
}

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
// Android has no console: LOGF output goes to logcat, but only when explicitly requested with
// `adb shell setprop debug.capy.verbose 1`. Formatting and sending thousands of lines per
// second to logd otherwise steals CPU time from the game.
bool AndroidVerbose() {
	static Common::DebugFlags::CachedValue verbose("debug.capy.verbose", 0);
	return verbose.Get() != 0;
}

void AndroidLog(int priority, std::string_view text) {
	__android_log_print(priority, "CapyPS5-Core", "%.*s", static_cast<int>(text.size()), text.data());
}
#endif

void WriteStdout(std::string_view text, fmt::text_style style = {}) {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	(void)style;
	AndroidLog(ANDROID_LOG_INFO, text);
	return;
#endif
	if (HasStyle(style)) {
		fmt::print(stdout, style, "{}", text);
	} else {
		std::fwrite(text.data(), 1, text.size(), stdout);
	}
	std::fflush(stdout);
}

} // namespace

namespace Log {

static bool                            g_initialized = false;
static Direction                       g_direction   = Direction::Console;
static std::filesystem::path           g_output_file;
static std::mutex                      g_logger_mutex;
static std::shared_ptr<spdlog::logger> g_logger;
// the live per-game log file requested by the Android frontend (see SetRealtimeFile)
static bool                            g_realtime_file   = false;
static bool                            g_file_truncate   = true;
static Direction                       g_saved_direction = Direction::Console;
static std::filesystem::path           g_saved_output_file;
// a live log past this size starts over at the next boot, so a chatty game cannot fill
// the storage without end
constexpr uint64_t                     kRealtimeLogMaxBytes = 64ull * 1024 * 1024;

void Flush() {
	if (g_logger != nullptr) {
		g_logger->flush();
	}
}

// caller holds g_logger_mutex
static void SetupLoggerLocked() {
	Flush();
	g_logger.reset();

	switch (g_direction) {
		case Direction::Silent:
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::null_sink_mt>());
			break;
		case Direction::Console:
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::stdout_sink_mt>());
			break;
		case Direction::File:
			if (!g_output_file.empty()) {
				g_logger = MakeFileLogger("kyty", g_output_file, g_file_truncate);
			}
			break;
	}
}

static void SetupLogger() {
	std::lock_guard lock(g_logger_mutex);
	SetupLoggerLocked();
}

static std::shared_ptr<spdlog::logger> GetLogger() {
	std::lock_guard lock(g_logger_mutex);
	return g_logger;
}

// Android fatal reports also reach the live log file, so a DbgExit ends the story in the
// same file the rest of the session wrote (unused off-Android, where file direction is set
// by the config and WriteFatal goes through WriteImpl anyway).
[[maybe_unused]] static void WriteFatalToRealtimeFile(std::string_view text) {
	if (g_direction != Direction::File) {
		return;
	}
	if (auto logger = GetLogger()) {
		logger->log(spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
		if (text.empty() || text.back() != '\n') {
			logger->log(spdlog::level::info, spdlog::string_view_t("\n", 1));
		}
		logger->flush();
	}
}

static void WriteImpl(std::string_view text, fmt::text_style style = {}) {
	if (text.empty()) {
		return;
	}

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	if (AndroidVerbose()) {
		AndroidLog(ANDROID_LOG_INFO, text);
	}
	if (g_direction != Direction::File) {
		return;
	}
#endif

	if (!g_initialized) {
		WriteStdout(text, style);
		return;
	}

	if (g_direction == Direction::Silent) {
		return;
	}

	if (auto logger = GetLogger()) {
		if (g_direction == Direction::Console && HasStyle(style)) {
			const auto styled = fmt::format(style, "{}", text);
			logger->log(spdlog::level::info, spdlog::string_view_t(styled.data(), styled.size()));
		} else {
			logger->log(spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
		}
		if (g_direction == Direction::File) {
			logger->flush(); // the live log: every line on disk before the next one
		}
	}
}

void WriteToConsoleAndLog(std::string_view text) {
	WriteImpl(text);
	if (g_initialized && g_direction != Direction::Console) {
		WriteStdout(text);
	}
	Flush();
}

static std::string g_fatal_report_path;

void SetFatalReportPath(std::string path) {
	g_fatal_report_path = std::move(path);
}

static void AppendFatalReport(std::string_view text) {
	if (g_fatal_report_path.empty()) {
		return;
	}
	if (auto* file = std::fopen(g_fatal_report_path.c_str(), "a"); file != nullptr) {
		std::fwrite(text.data(), 1, text.size(), file);
		if (text.empty() || text.back() != '\n') {
			std::fputc('\n', file);
		}
		std::fclose(file);
	}
}

void WriteFatal(std::string_view text) {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	AndroidLog(ANDROID_LOG_ERROR, text);
	AppendFatalReport(text);
	WriteFatalToRealtimeFile(text);
	Flush();
	return;
#endif
	if (g_direction == Direction::Silent || !g_initialized) {
		WriteStdout(text);
	} else {
		WriteStdout(text);
		WriteImpl(text);
	}
	Flush();
}

void WriteFatal(fmt::text_style style, std::string_view text) {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	(void)style;
	AndroidLog(ANDROID_LOG_ERROR, text);
	AppendFatalReport(text);
	WriteFatalToRealtimeFile(text);
	Flush();
	return;
#endif
	if (g_direction == Direction::Silent || !g_initialized) {
		WriteStdout(text, style);
	} else {
		WriteStdout(text, style);
		WriteImpl(text, style);
	}
	Flush();
}

void Initialize() {
	g_initialized = true;
	if (g_realtime_file) {
		// the frontend pointed the log at a live game file before boot; keep it
		SetupLogger();
		return;
	}
	g_file_truncate = true; // the config-driven log file restarts each boot
	switch (Config::GetPrintfDirection()) {
		case Config::LogDirection::Silent: g_direction = Direction::Silent; break;
		case Config::LogDirection::Console: g_direction = Direction::Console; break;
		case Config::LogDirection::File: g_direction = Direction::File; break;
	}
	g_output_file =
	    (g_direction == Direction::File ? Config::GetPrintfOutputFile() : std::filesystem::path {});
	SetupLogger();
}

void Shutdown() {
	Flush();
	std::lock_guard lock(g_logger_mutex);
	g_logger.reset();
}

void SetRealtimeFile(std::filesystem::path path) {
	std::lock_guard lock(g_logger_mutex);
	if (!g_realtime_file) {
		g_saved_direction   = g_direction;
		g_saved_output_file = g_output_file;
	}
	g_realtime_file = true;
	g_direction     = Direction::File;
	g_output_file   = std::move(path);
	g_file_truncate = false; // sessions append after the previous ones
	// a file past the cap starts over, so a chatty game cannot fill the storage
	std::error_code ec;
	if (std::filesystem::file_size(g_output_file, ec) > kRealtimeLogMaxBytes) {
		std::filesystem::remove(g_output_file, ec);
	}
	SetupLoggerLocked();
	if (g_logger) {
		const std::time_t now = std::time(nullptr);
		std::tm           tm {};
#ifdef _WIN32
		localtime_s(&tm, &now);
#else
		localtime_r(&now, &tm);
#endif
		char stamp[32]  = {};
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
		const auto header = fmt::format("\n===== CapyPS5 session {} =====\n", stamp);
		g_logger->log(spdlog::level::info, spdlog::string_view_t(header.data(), header.size()));
		g_logger->flush();
	}
}

void ClearRealtimeFile() {
	std::lock_guard lock(g_logger_mutex);
	if (!g_realtime_file) {
		return;
	}
	g_realtime_file = false;
	g_direction     = g_saved_direction;
	g_output_file   = g_saved_output_file;
	g_file_truncate = true;
	SetupLoggerLocked();
}

Direction GetDirection() {
	EXIT_IF(!g_initialized);
	return g_direction;
}

bool IsSilent() {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	return !AndroidVerbose() && g_direction != Direction::File;
#else
	// Before init LOGF must keep writing to stdout, so report non-silent.
	return g_initialized && g_direction == Direction::Silent;
#endif
}

void Write(std::string_view text) {
	WriteImpl(text);
}

void Write(fmt::text_style style, std::string_view text) {
	WriteImpl(text, style);
}

} // namespace Log
