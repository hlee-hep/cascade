#include "TextProducerModule.hh"

#include "Logger.hh"

#include <fstream>

TextProducerModule::TextProducerModule()
{
    SetBaseName("@BASENAME@");
    SetCodeHash("@VERSION_HASH@");
    Parameters().Register<std::string>("output", "message.json", "Output path relative to the execution output directory");
    Parameters().Register<std::string>("message", "hello from C++", "Message written to the output");
    Parameters().Register<int>("repeat", 3, "Number of message repetitions");
}

void TextProducerModule::Description() const
{
    LOG_INFO(BaseName(), "Writes a small JSON document through Cascade's output transaction.");
}

void TextProducerModule::Init()
{
    if (Parameters().Get<int>("repeat") < 1) throw std::invalid_argument("repeat must be positive");
}

void TextProducerModule::Execute()
{
    std::ofstream output(StageOutput(Parameters().Get<std::string>("output")));
    if (!output) throw std::runtime_error("cannot open staged text output");
    output << "{\n  \"messages\": [\n";
    for (int index = 0; index < Parameters().Get<int>("repeat"); ++index)
    {
        if (index) output << ",\n";
        output << "    \"" << Parameters().Get<std::string>("message") << " #" << index + 1 << "\"";
    }
    output << "\n  ]\n}\n";
    if (!output) throw std::runtime_error("cannot write staged text output");
}

void TextProducerModule::Finalize() {}
