#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <string>
#include <memory>

inline void initLogger(const std::string& logDir, const std::string& level)
{
    std::vector<spdlog::sink_ptr> sinks;

    // Console sink
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    // Rotating file sink: 10 MB per file, 5 files
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logDir + "/recorder.log", 10 * 1024 * 1024, 5));

    auto logger = std::make_shared<spdlog::logger>("recorder",
        sinks.begin(), sinks.end());

    if (level == "debug")       logger->set_level(spdlog::level::debug);
    else if (level == "warn")   logger->set_level(spdlog::level::warn);
    else if (level == "error")  logger->set_level(spdlog::level::err);
    else                        logger->set_level(spdlog::level::info);

    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
}
