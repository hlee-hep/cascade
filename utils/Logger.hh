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
    void PrintProgressBar(const std::string &name, double progress,
                          double elapsed = -1.0f, double eta = -1.0f);
    std::string GetCurrentTime();

  private:
    Logger();
    LogLevel g_level = LogLevel::INFO;
    std::recursive_mutex log_mutex;
    std::unique_ptr<std::ofstream> log_file_out;
    bool is_terminal = isatty(fileno(stdout));

    std::string LevelToColor(LogLevel level);
    std::string ToString(LogLevel level);
    std::string ApplyColor(LogLevel level, const std::string &module,
                           const std::string &msg);

    template <typename T> std::string ToStr(const T &value)
    {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    std::string colored(const std::string &text, const std::string &color = "",
                        const std::string &style = "");
    std::string HighlightMsg(const std::string &msg);
    std::string
    ReplaceRegex(const std::string &input, const std::regex &pattern,
                 std::function<std::string(const std::smatch &)> replacer);
    std::string HighlightStatus(std::string msg);
    std::string HighlightFileNames(std::string msg);
    std::string HighlightExprs(std::string msg);
    std::string HighlightParams(std::string msg);
    std::string HighlightWarnings(std::string msg);
};

#define LOG_STREAM(level, mod, msgstream)                                      \
    do                                                                         \
    {                                                                          \
        std::ostringstream __log_oss;                                          \
        __log_oss << msgstream;                                                \
        logger::Logger::Get().Log(level, mod, __log_oss.str());                \
    } while (0)

#define LOG_DEBUG(mod, msg) LOG_STREAM(logger::LogLevel::DEBUG, mod, msg)
#define LOG_INFO(mod, msg) LOG_STREAM(logger::LogLevel::INFO, mod, msg)
#define LOG_WARN(mod, msg) LOG_STREAM(logger::LogLevel::WARN, mod, msg)
#define LOG_ERROR(mod, msg) LOG_STREAM(logger::LogLevel::ERROR, mod, msg)

} // namespace logger