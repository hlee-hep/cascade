// Logger.hh (with integrated highlight logic)
#pragma once
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace logger
{

enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR,
    NONE
};

class Logger
{
  public:
    static Logger &Get();

    void SetLogLevel(LogLevel level);
    void Log(LogLevel level, const std::string &module, const std::string &msg);
    void InitLogFile(const std::string &path);
    void PrintProgressBar(const std::string &name, double progress, double elapsed = -1.0f, double eta = -1.0f);
    std::string GetCurrentTime();

  private:
    Logger();
    LogLevel m_Level = LogLevel::INFO;
    std::recursive_mutex m_LogMutex;
    std::unique_ptr<std::ofstream> m_LogFileOut;
    bool m_IsTerminal = isatty(fileno(stdout));

    std::string LevelToColor_(LogLevel level);
    std::string ToString_(LogLevel level);
    std::string ApplyColor_(LogLevel level, const std::string &module, const std::string &msg);

    template <typename T> std::string ToStr_(const T &value)
    {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    std::string Colored_(const std::string &text, const std::string &color = "", const std::string &style = "");
    std::string HighlightMsg_(const std::string &msg);
    std::string ReplaceRegex_(const std::string &input, const std::regex &pattern, std::function<std::string(const std::smatch &)> replacer);
    std::string HighlightStatus_(std::string msg);
    std::string HighlightFileNames_(std::string msg);
    std::string HighlightExprs_(std::string msg);
    std::string HighlightParams_(std::string msg);
    std::string HighlightWarnings_(std::string msg);
};

#define LOG_STREAM(level, mod, msgstream)                                                                                                                      \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        std::ostringstream logOss;                                                                                                                             \
        logOss << msgstream;                                                                                                                                   \
        logger::Logger::Get().Log(level, mod, logOss.str());                                                                                                   \
    } while (0)

#define LOG_DEBUG(mod, msg) LOG_STREAM(logger::LogLevel::DEBUG, mod, msg)
#define LOG_INFO(mod, msg) LOG_STREAM(logger::LogLevel::INFO, mod, msg)
#define LOG_WARN(mod, msg) LOG_STREAM(logger::LogLevel::WARN, mod, msg)
#define LOG_ERROR(mod, msg) LOG_STREAM(logger::LogLevel::ERROR, mod, msg)

} // namespace logger
