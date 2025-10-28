#include "config.hpp"
#include <fstream>
#include <sstream>

using namespace std;

static bool split_two(const std::string &line, std::string &a, std::string &b) {
    a.clear();
    b.clear();
    istringstream is(line);
    if (!(is >> a)) return false;
    if (!(is >> b)) return false;
    string extra;
    return !(is >> extra); // ensure two tokens
}

bool load_config(const std::filesystem::path &path, Config &out, std::string &err) {
    out.entries.clear();
    err.clear();
    ifstream in(path);
    if (!in.is_open()) {
        err = "Cannot open config: " + path.string();
        return false;
    }

    string line;
    size_t ln = 0;
    while (std::getline(in, line)) {
        ++ln;
        if (line.empty()) {
            err = "Empty line not allowed at line " + to_string(ln);
            return false;
        }
        string folder, ign;
        if (!split_two(line, folder, ign)) {
            err = "Expected: <folder> <ignfile> at line " + to_string(ln);
            return false;
        }
        std::filesystem::path f(folder);
        if (ign.find('/') != string::npos || ign.find('\\') != string::npos) {
            err = "ignfile must be a file name without path at line " + to_string(ln);
            return false;
        }
        out.entries.push_back({f, ign});
    }
    if (out.entries.empty()) {
        err = "Config has no entries";
        return false;
    }
    return true;
}