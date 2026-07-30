#include "TextProducerModule.hh"

#include "Logger.hh"

#include <fstream>

TextProducerModule::TextProducerModule()
{
    m_Basename = "@BASENAME@";
    m_CodeVersionHash = "@VERSION_HASH@";
    m_Param.Register<std::string>("output", "message.json", "Output path relative to the execution output directory");
    m_Param.Register<std::string>("message", "hello from C++", "Message written to the output");
    m_Param.Register<int>("repeat", 3, "Number of message repetitions");
}

void TextProducerModule::Description() const
{
    LOG_INFO(m_Basename, "Writes a small JSON document through Cascade's output transaction.");
}

void TextProducerModule::Init()
{
    if (m_Param.Get<int>("repeat") < 1) throw std::invalid_argument("repeat must be positive");
}

void TextProducerModule::Execute()
{
    std::ofstream output(StageOutput(m_Param.Get<std::string>("output")));
    if (!output) throw std::runtime_error("cannot open staged text output");
    output << "{\n  \"messages\": [\n";
    for (int index = 0; index < m_Param.Get<int>("repeat"); ++index)
    {
        if (index) output << ",\n";
        output << "    \"" << m_Param.Get<std::string>("message") << " #" << index + 1 << "\"";
    }
    output << "\n  ]\n}\n";
    if (!output) throw std::runtime_error("cannot write staged text output");
}

void TextProducerModule::Finalize() {}
