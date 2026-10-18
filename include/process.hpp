#pragma once
#include "resource.hpp"
namespace blocker {
struct CommandResult { int status; std::string output; };
CommandResult run(const std::vector<std::string>&);
void checked(const std::vector<std::string>&);
std::string xml(const std::string&);
}
