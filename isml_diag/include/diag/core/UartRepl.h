#pragma once

#include <functional>
#include <iosfwd>

class UartBridge;

namespace uart {

using PromptStateHandler = std::function<void(bool waiting_for_input)>;
using CommandHandler = std::function<void()>;

void run_repl(UartBridge& bridge, std::istream& input, std::ostream& output,
              PromptStateHandler on_prompt_state = {},
              CommandHandler on_command = {});

}
