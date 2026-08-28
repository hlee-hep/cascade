#include "TextFormatting.hh"

#include <string>

std::string NumberedMessage(const std::string &message, int index)
{
    return message + " #" + std::to_string(index);
}
