#pragma once

#include <string>

enum class AppMode
{
    Run,
    List,
    Setup,
    Help,
};

struct CliOptions
{
    AppMode mode = AppMode::Run;
    bool rollsOnlyLogs = false;
    bool valid = true;
    std::string error;
};

CliOptions parseCliOptions(int argc, char** argv);
