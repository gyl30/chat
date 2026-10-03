#include "file_io.hpp"

#include <filesystem>
#include <iostream>
#include <sys/resource.h>
#include <csignal>

namespace
{
void check(bool value, char const* description)
{
    if (!value) { std::cerr << "FAIL " << description << '\n'; std::exit(1); }
}
}

int main()
{
    char pattern[] = "/tmp/chat-tui-files-XXXXXX";
    auto* directory = ::mkdtemp(pattern);
    check(directory != nullptr, "temporary directory");
    auto root = std::filesystem::path(directory);
    struct cleanup
    {
        std::filesystem::path root;
        ~cleanup() { std::filesystem::remove_all(root); }
    } clean{root};
    auto path = (root / "中文 ; $(literal).txt").string();
    check(chat::tui::save_local_file(path, "hello").has_value(), "save literal Unicode path");
    auto data = chat::tui::read_local_file(path, 5);
    check(data && *data == "hello", "read at exact size limit");
    check(!chat::tui::read_local_file(path, 4), "reject oversize file");
    check(!chat::tui::save_local_file(path, "replacement"), "never overwrite existing file");
    data = chat::tui::read_local_file(path, 5);
    check(data && *data == "hello", "existing content unchanged");

    auto fifo = (root / "pipe").string();
    check(::mkfifo(fifo.c_str(), 0600) == 0, "create FIFO fixture");
    check(!chat::tui::read_local_file(fifo, 100), "reject FIFO without waiting for writer");
    check(!chat::tui::read_local_file(root.string(), 100), "reject directory");
    check(!chat::tui::read_local_file(std::string("file\0suffix", 11), 100), "reject embedded NUL");
    check(!chat::tui::save_local_file("", "x"), "reject empty save path");
    auto symlink = (root / "link").string();
    check(::symlink(path.c_str(), symlink.c_str()) == 0, "create symlink fixture");
    check(!chat::tui::save_local_file(symlink, "replacement"), "save cannot follow existing symlink");

    struct rlimit original{};
    check(::getrlimit(RLIMIT_FSIZE, &original) == 0, "read file size limit");
    auto limited = original;
    limited.rlim_cur = 4096;
    auto const previous_signal = std::signal(SIGXFSZ, SIG_IGN);
    check(previous_signal != SIG_ERR, "install controlled write failure signal handler");
    check(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "set controlled write failure limit");
    auto partial = (root / "partial").string();
    auto saved = chat::tui::save_local_file(partial, std::string(8192, 'x'));
    check(::setrlimit(RLIMIT_FSIZE, &original) == 0, "restore file size limit");
    std::signal(SIGXFSZ, previous_signal);
    check(!saved, "report partial write error");
    check(!std::filesystem::exists(partial), "remove only newly created partial file");
    std::cout << "PASS local file limit, FIFO, no-overwrite and partial-write cleanup\n";
}
