#include "diag/core/UartRepl.h"

#include "diag/core/Format.h"
#include "diag/core/UartBridge.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

uint64_t parse_unsigned(const std::string& text, uint64_t maximum)
{
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument("expected an unsigned number");
    }
    size_t used = 0;
    const auto value = std::stoull(text, &used, 0);
    if (used != text.size() || value > maximum) {
        throw std::invalid_argument("number is out of range");
    }
    return value;
}

bool no_more(std::istringstream& words)
{
    std::string extra;
    return !(words >> extra);
}

void print_failure(std::ostream& output, const char* operation,
                   const UartResponse& response)
{
    output << operation << " ERROR";
    if (!response.error.empty()) output << ": " << response.error;
    output << '\n';
}

}

namespace uart {

void run_repl(UartBridge& bridge, std::istream& input, std::ostream& output,
              PromptStateHandler on_prompt_state, CommandHandler on_command)
{
    std::string line;
    for (;;) {
        output << "uart> " << std::flush;
        if (on_prompt_state) on_prompt_state(true);
        const auto has_input = bool(std::getline(input, line));
        if (on_prompt_state) on_prompt_state(false);
        if (!has_input) {
            output << '\n';
            return;
        }

        std::istringstream words(line);
        std::string command;
        if (!(words >> command)) continue;
        std::transform(command.begin(), command.end(), command.begin(),
            [](unsigned char value) { return char(std::toupper(value)); });

        if (command == "EXIT") {
            if (!no_more(words)) output << "ERROR: EXIT takes no arguments\n";
            else return;
            continue;
        }
        if (command == "HELP") {
            if (!no_more(words)) {
                output << "ERROR: HELP takes no arguments\n";
            } else {
                output << "RD <addr>\nWR <addr> <value>\nEXIT\n";
            }
            continue;
        }
        if (command != "RD" && command != "WR") {
            output << "ERROR: unknown command " << command << '\n';
            continue;
        }

        try {
            std::string address_text;
            if (!(words >> address_text)) {
                throw std::invalid_argument(command + " requires an address");
            }
            const auto address = parse_unsigned(
                address_text, std::numeric_limits<uint64_t>::max());

            if (command == "RD") {
                if (!no_more(words)) throw std::invalid_argument("RD accepts one address");
                if (on_command) on_command();
                const auto response = bridge.read(address);
                if (response.result == UartResult::Ok) {
                    output << "RD " << common::format::hex(address) << " = "
                           << common::format::hex(response.value, 8) << '\n';
                } else {
                    print_failure(output, "RD", response);
                }
                continue;
            }
            if (command == "WR") {
                std::string value_text;
                if (!(words >> value_text) || !no_more(words)) {
                    throw std::invalid_argument("WR requires one address and one value");
                }
                const auto value = uint32_t(parse_unsigned(
                    value_text, std::numeric_limits<uint32_t>::max()));
                if (on_command) on_command();
                const auto response = bridge.write(address, value);
                if (response.result == UartResult::Ok) {
                    output << "WR " << common::format::hex(address) << " = "
                           << common::format::hex(value, 8) << " OK\n";
                } else {
                    print_failure(output, "WR", response);
                }
                continue;
            }
        } catch (const std::exception& error) {
            output << "ERROR: " << error.what() << '\n';
        }
    }
}

}
