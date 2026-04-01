#include "pch.h"
#include "App/CliOptions.h"

CliOptions parseCliOptions(int argc, char** argv)
{
    CliOptions options;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";

        if (arg == "--list")
        {
            if (options.mode != AppMode::Run)
            {
                options.valid = false;
                options.error = "Only one mode can be selected at a time.";
                return options;
            }
            options.mode = AppMode::List;
            continue;
        }

        if (arg == "--setup")
        {
            if (options.mode != AppMode::Run)
            {
                options.valid = false;
                options.error = "Only one mode can be selected at a time.";
                return options;
            }
            options.mode = AppMode::Setup;
            continue;
        }

        if (arg == "--help" || arg == "-h")
        {
            if (options.mode != AppMode::Run)
            {
                options.valid = false;
                options.error = "Only one mode can be selected at a time.";
                return options;
            }
            options.mode = AppMode::Help;
            continue;
        }

        if (arg == "--rolls-only")
        {
            options.rollsOnlyLogs = true;
            continue;
        }

        options.valid = false;
        options.error = "Unknown argument: " + arg;
        return options;
    }

    return options;
}
