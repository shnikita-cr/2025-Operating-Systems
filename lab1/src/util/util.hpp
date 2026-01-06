#pragma once

#pragma once

#include <filesystem>
#include <string>
#include <atomic>

extern std::atomic<bool> g_reload;
extern std::atomic<bool> g_stop;

// Guard rails to avoid catastrophic deletion
bool is_safe_folder(const std::filesystem::path &p);

// Delete ALL contents (not the folder itself). Returns true on success.
bool remove_contents(const std::filesystem::path &folder, std::string &err);

// Sleep for given seconds but wake early on stop/reload
void sleep_interruptible(unsigned seconds);