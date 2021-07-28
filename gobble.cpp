#include "stdafx.h"
#include "video.h"
#include "riffs.h"
#include "workqueue.h"
#include <string>
#include <vector>
#include <list>
#include <map>
#include <ctype.h>
#include <assert.h>
#include <algorithm>
#include <math.h>
#include <unistd.h>

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/Fl_Value_Input.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Roller.H>
#include <FL/Fl_Image.H>
#include <FL/fl_draw.H>


int numChunksToDecode;
int numChunksDecoded;

extern bool verbose;


class KeyframeWork : public Work {
    public:
        KeyframeWork(RiffFile *rf, uint64_t offset, uint64_t end)
            : file_(rf)
            , offset_(offset)
            , end_(end)
        {
        }
        ~KeyframeWork()
        {
            __sync_fetch_and_add(&numChunksDecoded, 1);
        }
        char const *name() {
            sprintf(buf, "offset %lld", (long long)offset_);
            return buf;
        }
        char buf[100];
        RiffFile *file_;
        uint64_t offset_;
        uint64_t end_;
        std::vector<VideoFrame> frames_;

        void work() {
            uint64_t pts = 0;
            uint64_t pos = offset_;
            std::vector<char> v;
            float steer = 0;
            float throttle = 0;
            while (pos < end_) {
                v.clear();
                ChunkHeader ch;
                if (!file_->data_header_at(pos, ch, v, 256)) {
                    break;
                }
                if (!strncmp(ch.type, "time", 4)) {
                    size_t offset = 8;
                    while (offset <= v.size() - 6) {
                        steer_packet sp;
                        memcpy(&sp, &v[offset], 6);
                        switch (sp.code) {
                        case 'S':
                            //  steer
                            steer = (sp.steer == -32768) ? 0 : sp.steer / 16383.0f;
                            throttle = (sp.throttle == -32768) ? 0 : sp.throttle / 16383.0f;
                            offset += 6;
                            break;
                        case 'i':
                            //  ibus
                            offset += 22;
                            break;
                        case 'T':
                            //  trim
                            offset += 10;
                            break;
                        default:
                            //  unknown
                            offset = v.size();
                            break;
                        }
                    }
                 }
                else if (!strncmp(ch.type, "pdts", 4)) {
                    if (v.size() >= 8) {
                        uint64_t p;
                        memcpy(&p, &v[0], 8);
                        if (p != 0 && p != 0x8000000000000000ULL) {
                            pts = p;
                        }
                    }
                }
                else if (!strncmp(ch.type, "h264", 4)) {
                    VideoFrame vf = { 0 };
                    vf.pts = pts;
                    vf.time = pts;
                    vf.offset = pos;
                    vf.file = file_;
                    vf.steer = steer;
                    vf.throttle = throttle;
                    vf.size = ch.size;
                    vf.index = frames_.size();
                    vf.keyframe = (vf.index == 0);
                    frames_.push_back(vf);
                }
                else if (!strncmp(ch.type, "info", 4)) {
                    //  skip the info chunk
                }
                else {
                    fprintf(stderr, "unknown chunk type: %.4s at offset %lld\n", 
                            ch.type, (long long)pos);
                }
                if (ch.size > 8 * 1024 * 1024) {
                    fprintf(stderr, "too large (%ld) chunk type: %.4s at offset %lld\n",
                            (long)ch.size, ch.type, (long long)pos);
                }
                pos = pos + 8 + ((ch.size + 3) & -4);
            }
            //  decode each frame
            if (frames_.size()) {
                DecodedFrame result;
                decoder_t *d = new_decoder();
                VideoFrame *vf = &frames_[0];
                uint64_t vftime = vf->pts;
                while ((vf = decode_frame_and_advance(d, vf, &result, &KeyframeWork::next_frame, this)) != nullptr) {
                    //  TODO:
                    //  do a thing
                    //
                    vftime = vf->pts;
                }
                (void)vftime;
            }
        }
        static VideoFrame *next_frame(VideoFrame *, void *);
};

VideoFrame *KeyframeWork::next_frame(VideoFrame *fr, void *co) {
    KeyframeWork *k = (KeyframeWork *)co;
    if (fr->index + 1 < k->frames_.size()) {
        return &k->frames_[fr->index+1];
    }
    return nullptr;
};

class RiffFileWork : public Work {
    public:
        RiffFileWork(RiffFile *rf) : file_(rf) {
            n = file_->path_.string();
            (void)n.c_str();
        }
        RiffFile *file_;
        char const *name() {
            return n.c_str();
        }
        std::string n;
        void work() {
            uint64_t startPos = 0;
            uint64_t lastTimePos = 0;
            uint64_t pos = 0;
            uint64_t opos = 0;
            std::vector<char> v;
            while (true) {
                v.clear();
                ChunkHeader ch;
                bool b = file_->header_at(pos, ch, opos);
                if (!b) {
                    break;
                }
                if (!strncmp(ch.type, "time", 4)) {
                    lastTimePos = pos;
                }
                if (!strncmp(ch.type, "h264", 4)) {
                    if (!file_->data_at(pos, v, 16)) {
                        fprintf(stderr, "Error reading data at %lld\n", (long long)pos);
                        break;
                    }
                    static const char kf[5] = { 0x00, 0x00, 0x00, 0x01, 0x27 };
                    if (!memcmp(&v[0], kf, 5)) {
                        __sync_fetch_and_add(&numChunksToDecode, 1);
                        add_work(new KeyframeWork(file_, startPos, pos));
                        startPos = lastTimePos;
                    }
                }
                pos = opos;
            }
            if (startPos < file_->size_) {
                add_work(new KeyframeWork(file_, startPos, file_->size_));
            }
        }
};

void split_riff_files() {
    for (auto const &rf : gRiffFiles) {
        add_work(new RiffFileWork(rf));
    }
}


int main(int argc, char const *argv[]) {
    int nt = 0;
    if (argv[1] && argv[2] && ((nt = atoi(argv[1])) > 0)) {
        ++argv;
        --argc;
    }
    if (!argv[1] || !strstr(argv[1], ".riff")) {
        fprintf(stderr, "usage: gobble some-file.riff\n");
        exit(1);
    }
    load_all_riffs(argv[1]);
    fprintf(stderr, "loaded %ld riffs\n", (long)gRiffFiles.size());
    start_work_queue(nt ? nt : 16);
    split_riff_files();
    usleep(100000);
    while (!numChunksToDecode || (numChunksToDecode > numChunksDecoded)) {
        usleep(100000);
        if (!verbose) {
            fprintf(stderr, "%7d / %7d\r", numChunksDecoded, numChunksToDecode);
        }
    }
    if (!verbose) {
        fprintf(stderr, "waiting for work to complete\n");
    }
    wait_for_all_work_to_complete();
    stop_work_queue();
    return 0;
}

