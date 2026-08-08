#include <windows.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "environ/Application.h"
#include "environ/EngineBootstrap.h"
#include "base/impl/SysInitImpl.h"

namespace {
std::string utf8_from_wide(const wchar_t *value) {
    if(!value || !*value) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                         -1, nullptr, 0, nullptr, nullptr);
    if(size <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    if(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                           result.data(), size, nullptr, nullptr) == 0) {
        return {};
    }
    result.resize(static_cast<size_t>(size - 1));
    return result;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    spdlog::set_level(spdlog::level::debug);

    static auto core_logger = spdlog::stdout_color_mt("core");
    static auto tjs2_logger = spdlog::stdout_color_mt("tjs2");
    static auto plugin_logger = spdlog::stdout_color_mt("plugin");
    spdlog::set_default_logger(core_logger);

    if(argc < 2) {
        spdlog::error("Usage: krkr2.exe <game path>");
        return 2;
    }

    std::vector<std::string> utf8_arguments;
    utf8_arguments.reserve(static_cast<size_t>(argc));
    for(int index = 0; index < argc; ++index) {
        std::string argument = utf8_from_wide(argv[index]);
        if(argument.empty() && argv[index] && *argv[index]) {
            spdlog::error("Command-line argument {} is not valid UTF-16", index);
            return 2;
        }
        utf8_arguments.emplace_back(std::move(argument));
    }
    std::vector<char *> narrow_argv;
    narrow_argv.reserve(utf8_arguments.size() + 1);
    for(std::string &argument : utf8_arguments)
        narrow_argv.push_back(argument.data());
    narrow_argv.push_back(nullptr);
    _argc = argc;
    _argv = narrow_argv.data();

    const std::string &game_path = utf8_arguments[1];

    if(!TVPEngineBootstrap::Initialize(960, 640)) {
        spdlog::error("Failed to initialize engine bootstrap");
        return 1;
    }

    const bool started = Application->StartApplication(ttstr(game_path));
    while(started && !TVPTerminated) {
        Application->Run();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    TVPEngineBootstrap::Shutdown();
    return started ? 0 : 1;
}
