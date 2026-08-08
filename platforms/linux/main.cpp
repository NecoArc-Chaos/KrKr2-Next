#include <gtk/gtk.h>

#include <chrono>
#include <string>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "environ/Application.h"
#include "environ/EngineBootstrap.h"
#include "base/impl/SysInitImpl.h"

int main(int argc, char **argv) {
    spdlog::set_level(spdlog::level::debug);

    static auto core_logger = spdlog::stdout_color_mt("core");
    static auto tjs2_logger = spdlog::stdout_color_mt("tjs2");
    static auto plugin_logger = spdlog::stdout_color_mt("plugin");
    spdlog::set_default_logger(core_logger);

    if(argc < 2) {
        spdlog::error("Usage: {} <game path>", argv[0]);
        return 2;
    }

    const std::string game_path = argv[1];
    _argc = argc;
    _argv = argv;

    int gtk_argc = argc;
    char **gtk_argv = argv;
    if(!gtk_init_check(&gtk_argc, &gtk_argv)) {
        spdlog::warn(
            "GTK could not connect to a display; dialogs are unavailable");
    }

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
