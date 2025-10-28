#include "util.hpp"
#include <chrono>
#include <thread>

using namespace std;
namespace fs = std::filesystem;

atomic<bool> g_reload{false};
atomic<bool> g_stop{false};

bool is_safe_folder(const fs::path &p) {
    if (p.empty()) return false;
    auto abs = fs::weakly_canonical(p);
    // Disallow root-like paths
    if (abs == "/" || abs == "\\" || abs.root_path() == abs) return false;
    return true;
}

bool remove_contents(const fs::path &folder, std::string &err) {
    err.clear();
    if (!fs::exists(folder)) return true; // nothing to do
    if (!fs::is_directory(folder)) {
        err = "Not a directory: " + folder.string();
        return false;
    }
    if (!is_safe_folder(folder)) {
        err = "Refusing to operate on unsafe path: " + folder.string();
        return false;
    }
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        fs::remove_all(it->path(), ec);
        if (ec) {
            err = "Failed to remove: " + it->path().string() + " — " + ec.message();
            return false;
        }
    }
    return true;
}

void sleep_interruptible(unsigned seconds) {
    using namespace chrono_literals;
    auto until = chrono::steady_clock::now() + chrono::seconds(seconds);
    while (chrono::steady_clock::now() < until) {
        if (g_stop.load() || g_reload.load()) return;
        this_thread::sleep_for(200ms);
    }
}