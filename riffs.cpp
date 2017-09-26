#include "stdafx.h"
#include "riffs.h"
#include "video.h"
#include <algorithm>

#if TARGET == TARGET_WINDOWS
#pragma warning(disable: 4996)
#pragma comment(lib, "fltk.lib")
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

std::vector<RiffFile *> gRiffFiles;
std::vector<VideoFrame> gFrames;

bool matches_except_for_digits(std::string const &a, std::string const &b) {
    size_t p;
    size_t la = a.length();
    size_t lb = b.length();
    for (p = 0; p != la && p != lb; ++p) {
        if (a[p] != b[p]) {
            break;
        }
    }
    size_t pa = la;
    size_t pb = lb;
    for (; pa != p && pb != p; --pa, --pb) {
        if (a[pa - 1] != b[pb - 1]) {
            break;
        }
    }
    for (size_t q = p; q != pa; ++q) {
        if (!isdigit(a[q])) {
            return false;
        }
    }
    for (size_t q = p; q != pb; ++q) {
        if (!isdigit(b[q])) {
            return false;
        }
    }
    return true;
}

void load_all_riffs(std::string const &path) {
    std::string prefix(path);
    size_t pos = prefix.find_last_of('/');
    if (pos == std::string::npos) {
        prefix = ".";
    }
    else {
        prefix = prefix.substr(0, pos);
    }
    fs::path root(prefix);
    uint64_t offset = 0;
    for (auto const &fn : fs::directory_iterator(prefix)) {
        fs::path fp(fn);
        std::string s(fp.string());
        //  the input string is in FLTK format, which is always forward slashes
        std::replace(s.begin(), s.end(), SEPARATOR, '/');
        if (matches_except_for_digits(s, path)) {
            gRiffFiles.push_back(new RiffFile(fp, offset));
            offset += gRiffFiles.back()->size_;
        }
    }
}


