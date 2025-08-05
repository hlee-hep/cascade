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
    std::lock_guard<std::recursive_mutex> lock(log_mutex);
    g_level = level;
}

void Logger::InitLogFile(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(log_mutex);
    log_file_out = std::make_unique<std::ofstream>(path);
    if (!log_file_out->is_open()) throw std::runtime_error("Failed to open log file: " + path);
}

std::string Logger::ToString(LogLevel level)
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

std::string Logger::LevelToColor(LogLevel level)
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

std::string Logger::colored(const std::string &text, const std::string &color, const std::string &style)
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

std::string Logger::ReplaceRegex(const std::string &input, const std::regex &pattern, std::function<std::string(const std::smatch &)> replacer)
{
    std::string output;
    std::sregex_iterator begin(input.begin(), input.end(), pattern), end;
    std::size_t last_pos = 0;
    for (auto it = begin; it != end; ++it)
    {
        output += input.substr(last_pos, it->position() - last_pos);
        output += replacer(*it);
        last_pos = it->position() + it->length();
    }
    output += input.substr(last_pos);
    return output;
}

std::string Logger::HighlightStatus(std::string msg)
{
    const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> status_map = {{"Status : Initializing", {"magenta", "bold"}},
                                                                                                 {"Status : Running", {"yellow", "bold"}},
                                                                                                 {"Status : Finalizing", {"cyan", "bold"}},
                                                                                                 {"Status : Skipped", {"blue", ""}},
                                                                                                 {"Status : Interrupted", {"red", ""}},
                                                                                                 {"Status : Failed", {"red", ""}},
                                                                                                 {"Status : Done", {"green", ""}}};
    for (const auto &[key, style] : status_map)
    {
        auto pos = msg.find(key);
        if (pos != std::string::npos) msg.replace(pos, key.size(), colored(key, style.first, style.second));
    }
    return msg;
}

std::string Logger::HighlightFileNames(std::string msg)
{
    std::regex file_pattern(R"((\b[\w\-/]+\.(root|yaml|csv|json)\b))");
    return ReplaceRegex(msg, file_pattern, [&](const std::smatch &m) { return colored(m.str(), "green", "bold"); });
}

std::string Logger::HighlightExprs(std::string msg)
{
    msg = ReplaceRegex(msg, std::regex(R"((name :\s*)(\w+))"), [&](const std::smatch &m) { return m[1].str() + colored(m[2].str(), "cyan"); });
    msg = ReplaceRegex(msg, std::regex(R"((expr :\s*)([^,;\n]+))"), [&](const std::smatch &m) { return m[1].str() + colored(m[2].str(), "yellow"); });
    return msg;
}

std::string Logger::HighlightParams(std::string msg)
{
    std::regex kv_pattern(R"((\b\w+\b)\s*=\s*([\w\.\-]+))");
    return ReplaceRegex(msg, kv_pattern, [&](const std::smatch &m) { return colored(m[1].str(), "magenta", "bold") + " = " + colored(m[2].str(), "white"); });
}

std::string Logger::HighlightWarnings(std::string msg)
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
        if (pos != std::string::npos) msg.replace(pos, k.size(), colored(k, "red", "bold"));
    }
    return msg;
}

std::string Logger::HighlightMsg(const std::string &msg)
{
    std::string m = msg;
    m = HighlightStatus(m);
    m = HighlightFileNames(m);
    m = HighlightExprs(m);
    m = HighlightParams(m);
    m = HighlightWarnings(m);
    return m;
}

std::string Logger::ApplyColor(LogLevel level, const std::string &module, const std::string &msg)
{
    std::string level_str = colored("[" + ToString(level) + "]", LevelToColor(level));
    std::string mod_str;

    if (module == "AnalysisManager")
        mod_str = colored(module, "blue");
    else if (module == "PlotManager")
        mod_str = colored(module, "blue", "bold");
    else if (module == "CONTROL")
        mod_str = colored(module, "green");
    else
        mod_str = colored(module, "magenta");

    return level_str + " [" + mod_str + "] " + HighlightMsg(msg);
}

void Logger::Log(LogLevel level, const std::string &module, const std::string &msg)
{
    std::lock_guard<std::recursive_mutex> lock(log_mutex);
    if (level < g_level) return;

    std::string raw = "[" + ToString(level) + "] [" + module + "] " + msg;

    // stdout
    if (is_terminal)
    {
        std::cout << ApplyColor(level, module, msg) << std::endl;
    }
    else
    {
        std::cout << raw << std::endl;
    }

    // file
    if (log_file_out && log_file_out->is_open())
    {
        *log_file_out << "[" << GetCurrentTime() << "] " << raw << std::endl;
    }
}

void Logger::PrintProgressBar(const std::string &name, double progress, double elapsed, double eta)
{
    std::lock_guard<std::recursive_mutex> lock(log_mutex);
    const int barWidth = 40;
    int pos = static_cast<int>(progress * barWidth);

    std::ostringstream oss;
    oss << "\r\033[K";

    oss << std::fixed << colored("[INFO]", "cyan") << " [" << colored("LOOP", "red", "bold") << "] [";

    for (int i = 0; i < barWidth; ++i)
    {
        if (i < pos)
            oss << colored("=", "green");
        else if (i == pos)
            oss << colored(">", "yellow");
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
    std::lock_guard<std::recursive_mutex> lock(log_mutex);
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

} // namespace logger