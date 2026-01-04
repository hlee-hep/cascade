#include "Logger.hh"

namespace logger
{

Logger &Logger::Get()
{
    static Logger instance;
    return instance;
}

Logger::Logger() {}

void Logger::SetLogLevel(LogLevel level)
{
    std::lock_guard<std::recursive_mutex> lock(m_LogMutex);
    m_Level = level;
}

void Logger::InitLogFile(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(m_LogMutex);
    m_LogFileOut = std::make_unique<std::ofstream>(path);
    if (!m_LogFileOut->is_open()) throw std::runtime_error("Failed to open log file: " + path);
}

std::string Logger::ToString_(LogLevel level)
{
    switch (level)
    {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARNING";
    case LogLevel::ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

std::string Logger::LevelToColor_(LogLevel level)
{
    switch (level)
    {
    case LogLevel::DEBUG:
        return "magenta";
    case LogLevel::INFO:
        return "cyan";
    case LogLevel::WARN:
        return "yellow";
    case LogLevel::ERROR:
        return "red";
    default:
        return "white";
    }
}

std::string Logger::Colored_(const std::string &text, const std::string &color, const std::string &style)
{
    std::string header, styleCode;
    if (color == "red")
        header = "31";
    else if (color == "green")
        header = "32";
    else if (color == "yellow")
        header = "33";
    else if (color == "blue")
        header = "34";
    else if (color == "magenta")
        header = "35";
    else if (color == "cyan")
        header = "36";
    else if (color == "white")
        header = "37";
    else
        header = "39";

    if (style == "bold")
        styleCode = "1;";
    else if (style == "underline")
        styleCode = "4;";
    else if (style == "dim")
        styleCode = "2;";
    else if (style == "blink")
        styleCode = "5;";
    else
        styleCode = "";

    return "\x1B[" + styleCode + header + "m" + text + "\x1B[0m";
}

std::string Logger::ReplaceRegex_(const std::string &input, const std::regex &pattern, std::function<std::string(const std::smatch &)> replacer)
{
    std::string output;
    std::sregex_iterator begin(input.begin(), input.end(), pattern), end;
    std::size_t lastPos = 0;
    for (auto it = begin; it != end; ++it)
    {
        output += input.substr(lastPos, it->position() - lastPos);
        output += replacer(*it);
        lastPos = it->position() + it->length();
    }
    output += input.substr(lastPos);
    return output;
}

std::string Logger::HighlightStatus_(std::string msg)
{
    const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> statusMap = {{"Status : Initializing", {"magenta", "bold"}},
                                                                                                 {"Status : Running", {"yellow", "bold"}},
                                                                                                 {"Status : Finalizing", {"cyan", "bold"}},
                                                                                                 {"Status : Skipped", {"blue", ""}},
                                                                                                 {"Status : Interrupted", {"red", ""}},
                                                                                                 {"Status : Failed", {"red", ""}},
                                                                                                 {"Status : Done", {"green", ""}}};
    for (const auto &[key, style] : statusMap)
    {
        auto pos = msg.find(key);
        if (pos != std::string::npos) msg.replace(pos, key.size(), Colored_(key, style.first, style.second));
    }
    return msg;
}

std::string Logger::HighlightFileNames_(std::string msg)
{
    std::regex filePattern(R"((\b[\w\-/]+\.(root|yaml|csv|json)\b))");
    return ReplaceRegex_(msg, filePattern, [&](const std::smatch &m) { return Colored_(m.str(), "green", "bold"); });
}

std::string Logger::HighlightExprs_(std::string msg)
{
    msg = ReplaceRegex_(msg, std::regex(R"((name :\s*)(\w+))"), [&](const std::smatch &m) { return m[1].str() + Colored_(m[2].str(), "cyan"); });
    msg = ReplaceRegex_(msg, std::regex(R"((expr :\s*)([^,;\n]+))"), [&](const std::smatch &m) { return m[1].str() + Colored_(m[2].str(), "yellow"); });
    return msg;
}

std::string Logger::HighlightParams_(std::string msg)
{
    std::regex kvPattern(R"((\b\w+\b)\s*=\s*([\w\.\-]+))");
    return ReplaceRegex_(msg, kvPattern, [&](const std::smatch &m) { return Colored_(m[1].str(), "magenta", "bold") + " = " + Colored_(m[2].str(), "white"); });
}

std::string Logger::HighlightWarnings_(std::string msg)
{
    const std::vector<std::string> keywords = {"Duplication of hash is detected",
                                               "Invalid bin format",
                                               "Missing snapshot",
                                               "Failed to open",
                                               "Continue",
                                               "No tree found for cuts! Please assign a tree first."
                                               "Unsupported type",
                                               "Cannot"};
    for (const auto &k : keywords)
    {
        auto pos = msg.find(k);
        if (pos != std::string::npos) msg.replace(pos, k.size(), Colored_(k, "red", "bold"));
    }
    return msg;
}

std::string Logger::HighlightMsg_(const std::string &msg)
{
    std::string m = msg;
    m = HighlightStatus_(m);
    m = HighlightFileNames_(m);
    m = HighlightExprs_(m);
    m = HighlightParams_(m);
    m = HighlightWarnings_(m);
    return m;
}

std::string Logger::ApplyColor_(LogLevel level, const std::string &module, const std::string &msg)
{
    std::string levelStr = Colored_("[" + ToString_(level) + "]", LevelToColor_(level));
    std::string modStr;

    if (module == "AnalysisManager")
        modStr = Colored_(module, "blue");
    else if (module == "PlotManager")
        modStr = Colored_(module, "blue", "bold");
    else if (module == "CONTROL")
        modStr = Colored_(module, "green");
    else
        modStr = Colored_(module, "magenta");

    return levelStr + " [" + modStr + "] " + HighlightMsg_(msg);
}

void Logger::Log(LogLevel level, const std::string &module, const std::string &msg)
{
    std::lock_guard<std::recursive_mutex> lock(m_LogMutex);
    if (level < m_Level) return;

    std::string raw = "[" + ToString_(level) + "] [" + module + "] " + msg;

    // stdout
    if (m_IsTerminal)
    {
        std::cout << ApplyColor_(level, module, msg) << std::endl;
    }
    else
    {
        std::cout << raw << std::endl;
    }

    // file
    if (m_LogFileOut && m_LogFileOut->is_open())
    {
        *m_LogFileOut << "[" << GetCurrentTime() << "] " << raw << std::endl;
    }
}

void Logger::PrintProgressBar(const std::string &name, double progress, double elapsed, double eta)
{
    std::lock_guard<std::recursive_mutex> lock(m_LogMutex);
    const int barWidth = 40;
    int pos = static_cast<int>(progress * barWidth);

    std::ostringstream oss;
    oss << "\r\033[K";

    oss << std::fixed << Colored_("[INFO]", "cyan") << " [" << Colored_("LOOP", "red", "bold") << "] [";

    for (int i = 0; i < barWidth; ++i)
    {
        if (i < pos)
            oss << Colored_("=", "green");
        else if (i == pos)
            oss << Colored_(">", "yellow");
        else
            oss << " ";
    }
    oss << "] ";

    oss << std::fixed << std::setprecision(1) << (progress * 100.0f) << "%";

    if (elapsed >= 0) oss << " | " << std::fixed << std::setprecision(1) << elapsed << "s";
    if (eta >= 0) oss << " | ETA: " << std::fixed << std::setprecision(1) << eta << "s";

    std::cout << oss.str() << std::flush;

    if (progress >= 1.0) std::cout << std::endl;
}

std::string Logger::GetCurrentTime()
{
    std::lock_guard<std::recursive_mutex> lock(m_LogMutex);
    auto now = std::chrono::system_clock::now();
    std::time_t nowC = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&nowC), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

} // namespace logger
