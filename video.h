#if !defined(video_h)
#define video_h

#include <stdint.h>

#include <filesystem>
#include <iostream>
#include <fstream>

struct ChunkHeader {
    char type[4];
    uint32_t size;
};

struct FileHeader {
    char type[4];
    uint32_t zero;
    char subtype[4];
};

namespace fs = std::experimental::filesystem;

class RiffFile {
public:
    RiffFile(fs::path const &path, uint64_t offset) : path_(path), offset_(offset) {
        file_.open(path_.string(), std::ifstream::binary);
        if (!file_.good()) {
            throw std::runtime_error("Could not open file: " + path.string());
        }
        file_.seekg(0, std::ios_base::end);
        size_ = file_.tellg();
        if (size_ < 12) {
            size_ = 0;
        }
        else {
            size_ -= 12;
        }
    }

    bool header_at(uint64_t pos, ChunkHeader &ret, uint64_t &opos) {
        if (pos >= size_) {
            opos = size_;
            return false;
        }
        file_.seekg(pos + 12, std::ios_base::beg);
        file_.read((char *)&ret, 8);
        if (!file_.good()) {
            opos = size_;
            return false;
        }
        opos = pos + 8 + ((ret.size + 3) & -4);
        return true;
    }

    bool data_at(uint64_t hdrpos, std::vector<char> &data, size_t max_size = 0) {
        file_.seekg(hdrpos + 12, std::ios_base::beg);
        ChunkHeader hdr = { 0 };
        file_.read((char *)&hdr, sizeof(hdr));
        if (max_size == 0) {
            max_size = hdr.size;
        }
        else if (max_size > hdr.size) {
            max_size = hdr.size;
        }
        if (!file_.good() || (max_size > 8 * 1024 * 1024)) {
            fprintf(stderr, "%s: block %.4s at %lld size %ld is too big to read\n",
                path_.string().c_str(), hdr.type, (long long)hdrpos, (long)hdr.size);
            return false;
        }
        size_t initsize = data.size();
        data.resize(initsize + max_size);
        if (max_size == 0) {
            return true;
        }
        file_.read((char *)&data[initsize], max_size);
        if (!file_.good()) {
            fprintf(stderr, "%s: block %.4s at %lld size %ld was truncated\n",
                path_.string().c_str(), hdr.type, (long long)hdrpos, (long)hdr.size);
            return false;
        }
        return true;
    }

    fs::path path_;
    std::ifstream file_;
    uint64_t size_;
    uint64_t offset_;
};


struct VideoFrame {
    uint64_t pts;
    uint64_t time;
    uint64_t offset;
    RiffFile *file;
    float steer;
    float throttle;
    uint32_t size;
    uint32_t index;
    bool keyframe;
};

struct DecodedFrame {
public:
    DecodedFrame() : yuv_planar(0), rgb_interleaved(0), cropped(0), time(0), width(0), height(0), keyframe(false) {}
    ~DecodedFrame() { clear(); }
    void set_decoded(uint64_t t, uint16_t w, uint16_t h, unsigned char *yuv, bool kf) {
        if (yuv == yuv_planar) {
            //  re-use buffer on decoding
            yuv_planar = nullptr;
        }
        clear();
        yuv_planar = yuv;
        time = t;
        width = w;
        height = h;
        keyframe = kf;
    }
    unsigned char const *decode_rgb() {
        if (!rgb_interleaved) {
            rgb_interleaved = new unsigned char[width * height * 3];

        }
        return rgb_interleaved;
    }
    unsigned char const *crop() {
        if (!cropped) {
            cropped = new unsigned char[width * height * 2];

        }
        return cropped;
    }
    unsigned char *yuv_planar;
    unsigned char *rgb_interleaved;
    unsigned char *cropped;
    uint64_t time;
    uint16_t width;
    uint16_t height;
    bool keyframe;
    void clear() {
        delete[] yuv_planar;
        yuv_planar = nullptr;
        delete[] rgb_interleaved;
        rgb_interleaved = nullptr;
        delete[] cropped;
        cropped = nullptr;
    }
};


void begin_decode(VideoFrame *frame);
VideoFrame *decode_frame_and_advance(VideoFrame *frame, DecodedFrame *result);


#endif  //  video_H
