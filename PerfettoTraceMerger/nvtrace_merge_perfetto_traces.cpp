/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <NvTracePerfettoTraceCombiner.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

    void PrintUsage(std::ostream& output)
    {
        output << "Usage: nvtrace_merge_perfetto_traces --output <archive.perfetto.zip> "
                  "<input.perfetto.zip>...\n"
                  "       Response files are accepted with @inputs syntax.\n";
    }

    bool TokenizeResponseFile(
        const nv::trace::ZipFilesystem::path& path,
        std::vector<std::string>& result,
        std::string& error)
    {
        std::ifstream input(path);
        if (!input)
        {
            error = "cannot open response file: " + path.string();
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::string token;
        bool quoted = false;
        char quote = 0;
        for (size_t index = 0; index < text.size(); ++index)
        {
            const char character = text[index];
            if (quoted)
            {
                if (character == quote)
                {
                    quoted = false;
                }
                else if (character == '\\' && index + 1 < text.size() && text[index + 1] == quote)
                {
                    token.push_back(text[++index]);
                }
                else
                {
                    token.push_back(character);
                }
            }
            else if (character == '"' || character == '\'')
            {
                quoted = true;
                quote = character;
            }
            else if (std::isspace(static_cast<unsigned char>(character)))
            {
                if (!token.empty())
                {
                    result.push_back(token);
                    token.clear();
                }
            }
            else
            {
                token.push_back(character);
            }
        }
        if (quoted)
        {
            error = "unterminated quote in response file: " + path.string();
            return false;
        }
        if (!token.empty())
        {
            result.push_back(token);
        }
        return true;
    }

    bool ExpandArgument(
        const std::string& argument,
        std::vector<std::string>& output,
        unsigned depth,
        std::string& error)
    {
        if (argument.size() <= 1 || argument[0] != '@')
        {
            output.push_back(argument);
            return true;
        }
        if (depth >= 8)
        {
            error = "response-file nesting exceeds eight levels";
            return false;
        }
        std::vector<std::string> nestedArguments;
        const bool tokenized = TokenizeResponseFile(argument.substr(1), nestedArguments, error);
        if (!tokenized)
        {
            return false;
        }
        for (const std::string& nested : nestedArguments)
        {
            const bool expanded = ExpandArgument(nested, output, depth + 1, error);
            if (!expanded)
            {
                return false;
            }
        }
        return true;
    }

    bool ParseArguments(
        const std::vector<std::string>& arguments,
        nv::trace::ZipFilesystem::path& output,
        std::vector<nv::trace::ZipFilesystem::path>& inputs,
        bool& help,
        std::string& error)
    {
        help = false;
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            const std::string& argument = arguments[index];
            if (argument == "--help" || argument == "-h")
            {
                help = true;
            }
            else if (argument == "--output" || argument == "-o")
            {
                if (++index == arguments.size())
                {
                    error = argument + " requires a path";
                    return false;
                }
                output = arguments[index];
            }
            else if (argument.compare(0, 9, "--output=") == 0)
            {
                output = argument.substr(9);
            }
            else if (!argument.empty() && argument[0] == '-')
            {
                error = "unknown option: " + argument;
                return false;
            }
            else
            {
                inputs.push_back(argument);
            }
        }
        if (!help && (output.empty() || inputs.empty()))
        {
            error = "--output and at least one input archive are required";
            return false;
        }
        return true;
    }

    int Run(int argc, char** ppArgv)
    {
        std::vector<std::string> arguments;
        std::string error;
        for (int index = 1; index < argc; ++index)
        {
            const bool expanded = ExpandArgument(ppArgv[index], arguments, 0, error);
            if (!expanded)
            {
                std::cerr << "Error: " << error << '\n';
                return EXIT_FAILURE;
            }
        }

        nv::trace::ZipFilesystem::path output;
        std::vector<nv::trace::ZipFilesystem::path> inputs;
        bool help = false;
        const bool parsed = ParseArguments(arguments, output, inputs, help, error);
        if (!parsed)
        {
            std::cerr << "Error: " << error << '\n';
            PrintUsage(std::cerr);
            return EXIT_FAILURE;
        }
        if (help)
        {
            PrintUsage(std::cout);
            return EXIT_SUCCESS;
        }

        nv::trace::PerfettoTraceCombiner combiner;
        const bool initialized = combiner.Initialize();
        if (!initialized)
        {
            std::cerr << "Error: failed to initialize Perfetto trace combiner\n";
            return EXIT_FAILURE;
        }
        std::string warnings;
        const bool combined = combiner.Combine(inputs, output, &error, &warnings);
        const bool hasWarnings = !warnings.empty();
        if (hasWarnings)
        {
            std::cerr << warnings;
        }
        if (!combined)
        {
            std::cerr << "Error: " << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "Combined " << inputs.size() << " Perfetto trace archives into " << output.string() << '\n';
        return EXIT_SUCCESS;
    }

} // namespace

int main(int argc, char** ppArgv)
{
    return Run(argc, ppArgv);
}
