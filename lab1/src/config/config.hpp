#pragma once

#include <string>
#include <vector>
#include <filesystem>

struct ConfigEntry {
    std::filesystem::path folder;
    std::string ignfile; // filename only
};

struct Config {
    std::vector<ConfigEntry> entries;

    bool load_config(const std::filesystem::path &path, Config &out, std::string &err);
};
