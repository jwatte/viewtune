
#include "stdafx.h"
#include "video.h"
#include <string>
#include <vector>
#include <list>
#include <map>
#include <ctype.h>
#include <assert.h>

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/Fl_Value_Input.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Roller.H>
#include <FL/Fl_Image.H>
#include <FL/fl_draw.H>

#define TARGET_WINDOWS 'W'
#define TARGET_LINUX 'L'

#if defined(_MSC_VER)
#define TARGET TARGET_WINDOWS
#else
#define TARGET TARGET_LINUX
#endif

#if TARGET == TARGET_WINDOWS
#pragma warning(disable: 4996)
#pragma comment(lib, "fltk.lib")
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

#define APPROXIMATE_FRAME_DURATION 0.011

class RiffFile;

Fl_Window *mainWindow;
std::vector<RiffFile *> gRiffFiles;

bool file_exists(std::string const &path) {
    return std::ifstream(path).good();
}

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

struct steer_packet {
    uint16_t code;
    int16_t steer;
    int16_t throttle;
};

std::vector<VideoFrame> gFrames;
double finalFrameTime;

std::map<uint64_t, DecodedFrame *> gDecodedFrames;
std::list<DecodedFrame *> gDecodedFreeList;

enum GetFrameMode {
    GetFrameModeClosest = 0,
    GetFrameModeEarlier = 1,
    GetFrameModeLater = 2,
    GetFrameModeFollowing = 3,
    GetFrameModePreceeding = 4
};

uint64_t determine_frame_time(uint64_t time, GetFrameMode mode) {
    uint64_t moved = time;
    if (gFrames.size() == 0) {
        return 0;
    }
    size_t top = gFrames.size();
    size_t bottom = 0;
    while (top > bottom+1) {
        size_t avg = (top + bottom) / 2;
        if (gFrames[avg].time > time) {
            top = avg;
        }
        else {
            bottom = avg;
        }
    }
    uint64_t bottomTime = gFrames[bottom].time;
    while (bottom > 0 && gFrames[bottom - 1].time == bottomTime) {
        --bottom;
    }
    if (bottomTime > time) {
        assert(bottom == 0);
        return bottomTime;
    }
    if (top == gFrames.size()) {
        return gFrames[top - 1].time;
    }
    uint64_t topTime = gFrames[top].time;
    //  top is strictly greater, bottom is less-or-equal
    switch (mode) {
    case GetFrameModeClosest:
        if (time - bottomTime <= topTime - time) {
            return bottomTime;
        }
        return topTime;
    case GetFrameModeEarlier:
        return bottomTime;
    case GetFrameModeLater:
        return (bottomTime == time) ? bottomTime : topTime;
    case GetFrameModeFollowing:
        return topTime;
    case GetFrameModePreceeding:
        if (bottomTime == time && bottom > 0) {
            return gFrames[bottom - 1].time;
        }
        return bottomTime;
    }
    assert(!"unknown get frame mode");
    return 0;
}

VideoFrame *get_keyframe_for_time(uint64_t frametime) {
    size_t bottom = 0;
    size_t top = gFrames.size();
    if (!top) {
        return nullptr;
    }
    while (top > bottom + 1) {
        size_t avg = (top + bottom) / 2;
        if (gFrames[avg].time > frametime) {
            top = avg;
        }
        else {
            bottom = avg;
        }
    }
    while (bottom > 0 && !gFrames[bottom].keyframe) {
        --bottom;
    }
    return &gFrames[bottom];
}

#define MAX_FRAME_CACHE_SIZE 350
#define FRAME_CACHE_HYSTERESIS 90

extern bool verbose;

DecodedFrame *get_frame_at(uint64_t t, GetFrameMode mode = GetFrameModeClosest) {
    DecodedFrame *frame = nullptr;
    uint64_t frameTime = determine_frame_time(t, mode);
    auto found(gDecodedFrames.lower_bound(frameTime));
    if ((found != gDecodedFrames.end()) && (found->first == frameTime)) {
        return found->second;
    }
    if (gDecodedFrames.size() >= MAX_FRAME_CACHE_SIZE) {
        do {
            gDecodedFreeList.push_back(gDecodedFrames.begin()->second);
            gDecodedFrames.erase(gDecodedFrames.begin());
        } while ((gDecodedFrames.size() > (MAX_FRAME_CACHE_SIZE - FRAME_CACHE_HYSTERESIS)) &&
            !gDecodedFrames.begin()->second->keyframe);
    }
    VideoFrame *keyframe = get_keyframe_for_time(frameTime);
    begin_decode(keyframe);
    VideoFrame *curFrame = keyframe;
    DecodedFrame *ret = nullptr;
    while (true) {
        DecodedFrame *df = nullptr;
        if (gDecodedFreeList.empty()) {
            df = new DecodedFrame();
        }
        else {
            df = gDecodedFreeList.front();
            gDecodedFreeList.pop_front();
        }
        size_t previx = curFrame->index;
        uint64_t prevoff = curFrame->offset;
        size_t prevsz = curFrame->size;
        uint64_t prevtime = curFrame->time;
        curFrame = decode_frame_and_advance(curFrame, df);
        if (!curFrame) {
            gDecodedFreeList.push_back(df);
            fprintf(stderr, "decode_frame_and_advance() failed\n");
            break;
        }
        if (verbose) {
            fprintf(stderr, "decoded frame %ld at offset %lld size %ld with time %lld for time %lld\n", (long)previx, prevoff, (long)prevsz, df->time, prevtime);
        }
        gDecodedFrames[df->time] = df;
        if (df->time >= frameTime && !ret) {
            ret = df;
        }
        if (curFrame->keyframe) {
            break;
        }
    }
    return ret;
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

bool gTimeoutSet = false;

void set_timeout(void *) {
    gTimeoutSet = true;
}

void analyze_all_riffs() {
    Fl_Double_Window progress(600, 50, "Loading Progress");
    Fl_Output output(10, 5, 580, 40, "");
    progress.end();
    progress.show();
    ChunkHeader hdr;
    VideoFrame vf = { 0 };
    for (auto const &rp : gRiffFiles) {
        output.value(rp->path_.string().c_str());
        progress.redraw();
        Fl::wait(0.1);
        uint64_t pos = 0;
        uint64_t nextpos = 0;
        vf.file = rp;
        std::vector<char> data;
        while (rp->header_at(pos, hdr, nextpos)) {
            data.resize(0);
            //  data for keyframe frame info start with 0000 0001 28
            //  data for pframes start with 0000 0001 21
            //  the Pi encoder seems to write the keyframe headers in a 
            //  distinct packet from the payload data, so packet size is 
            //  also a seemingly reliable indicator.
            if (!strncmp(hdr.type, "info", 4)) {
                //  ignore
            }
            else if (!strncmp(hdr.type, "pdts", 4)) {
                struct pdts {
                    uint64_t pts;
                    uint64_t dts;
                };
                if (rp->data_at(pos, data, 1024)) {
                    if (data.size() >= sizeof(pdts)) {
                        vf.pts = ((pdts *)&data[0])->pts;
                        vf.time = vf.pts;
                    }
                }
            }
            else if (!strncmp(hdr.type, "time", 4)) {
                if (rp->data_at(pos, data, 1024)) {
                    /* time is not very regular -- pts is better
                    if (data.size() >= 8) {
                        memcpy(&vf.time, &data[0], 8);
                        if (startTime == 1) {
                            startTime = vf.time;
                        }
                        vf.time -= startTime;
                    }
                    */
                    size_t offset = 8;
                    while (offset <= data.size() - 6) {
                        steer_packet sp;
                        memcpy(&sp, &data[offset], 6);
                        switch (sp.code) {
                        case 'S':
                            //  steer
                            vf.steer = (sp.steer == -32768) ? 0 : sp.steer / 16383.0f;
                            vf.throttle = (sp.throttle == -32768) ? 0 : sp.throttle / 16383.0f;
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
                            offset = data.size();
                            break;
                        }
                    }
                }
            }
            else if (!strncmp(hdr.type, "h264", 4)) {
                if (rp->data_at(pos, data, 1024) && data.size() > 16) {
                    char kf[5] = { 0x00, 0x00, 0x00, 0x01, 0x27 };
                    if (!memcmp(kf, &data[0], sizeof(kf))) {
                        vf.keyframe = true;
                    }
                    else {
                        vf.keyframe = false;
                    }
                    vf.offset = pos;
                    vf.size = hdr.size;
                    vf.index = (uint32_t)gFrames.size();
                    gFrames.push_back(vf);
                    finalFrameTime = vf.pts * 1e-6 + APPROXIMATE_FRAME_DURATION;
                }
            }
            else {
                //  unknown
            }
            pos = nextpos;
        }
    }
    int64_t ptsOffset = 0;
    if (gFrames.size() > 1) {
        ptsOffset = std::max(gFrames[0].pts, gFrames[1].pts);
    }
    uint64_t prev = 0;
    for (auto &frm : gFrames) {
        if (frm.pts && frm.pts < 0x8000000000000000ULL) {
            if (frm.pts - ptsOffset < prev) {
                fprintf(stderr, "ERROR: PTS %lld is before previous PTS %lld\n", (uint64_t)frm.pts, (uint64_t)prev);
            }
            if (frm.pts < (uint64_t)ptsOffset) {
                fprintf(stderr, "ERROR: PTS %lld is before start of file %lld\n", (uint64_t)frm.pts, (uint64_t)ptsOffset);
            }
            if (frm.pts > ptsOffset + prev + 1000000000) {
                fprintf(stderr, "WARNING: PTS %lld jumps into the future from %lld\n", (uint64_t)frm.pts, (uint64_t)prev);
            }
            frm.pts -= ptsOffset;
            frm.time -= ptsOffset;
            prev = frm.pts;
        }
        else {
            frm.pts = prev;
            frm.time = prev;
        }
    }
    char str[256];
    sprintf(str, "loaded %ld h264 packets from %ld files", (long)gFrames.size(), (long)gRiffFiles.size());
    fprintf(stderr, "%s\n", str);
    output.value(str);
    progress.redraw();
    gTimeoutSet = false;
    Fl::add_timeout(7, set_timeout, 0);
    while (!gTimeoutSet) {
        Fl::wait(0.1);
        if (!progress.visible()) {
            Fl::remove_timeout(set_timeout, 0);
            break;
        }
    }
}


int winWidth = 1280;
int winHeight = 640;
int oneRow = 20;
int scrubberWidth = 100;
int titleBarHeight = 10;
int colorLabelWidth = 20;


class Fl_FrameImage : public Fl_Image {

public:

    Fl_FrameImage(int w, int h, int d) : Fl_Image(w, h, d), df_(nullptr) {}

    void frame(DecodedFrame *df) {
        df_ = df;
        if (!df_) {
            ptrs_.clear();
            data(nullptr, 0);
        }
        else {
            ptrs_.resize(df->height);
            unsigned char const *pp = df->decode_rgb();
            for (size_t i = 0; i != df->height; ++i) {
                ptrs_[i] = pp + i * 3 * df->width;
            }
            data((char const * const *)&ptrs_[0], (int)df->height);
        }
    }

    DecodedFrame *df_;
    std::vector<unsigned char const *> ptrs_;
};


class Fl_VideoFrame : public Fl_Widget {

public:

    Fl_VideoFrame(int x, int y, int w, int h, char const *l) : Fl_Widget(x, y, w, h, l) {
        frame_ = nullptr;
    }

    void draw() override {
        if (!frame_ || !frame_->yuv_planar) {
            fl_rectf(x(), y(), w(), h(), 128, 128, 128);
        }
        else {
            int ww = w();
            if (w() > frame_->width) {
                ww = frame_->width;
                fl_rectf(x() + frame_->width, y(), w() - frame_->width, h(), 128, 128, 128);
            }
            int hh = h();
            if (h() > frame_->height) {
                hh = frame_->height;
                fl_rectf(x(), frame_->height, w(), h() - frame_->height, 128, 128, 128);
            }
            fl_draw_image(frame_->yuv_planar, x(), y(), ww, hh, 1, frame_->width);
        }
    }

    DecodedFrame *frame_;
};

class Fl_Scrubber : public Fl_Valuator {

public:

    Fl_Scrubber(int x, int y, int w, int h, char const *l) : Fl_Valuator(x, y, w, h, l) {
        xlast_ = 0;
        ylast_ = 0;
    }

    int handle(int event) override {
        int ret = Fl_Valuator::handle(event);
        switch (event) {
        case FL_PUSH:
            xlast_ = Fl::event_x();
            ylast_ = Fl::event_y();
            ret = 1;
            break;
        case FL_RELEASE:
        case FL_DRAG:
            if (type() == FL_HORIZONTAL) {
                value(Fl::event_x() - xlast_);
            }
            else {
                value(Fl::event_y() - ylast_);
            }
            do_callback();
            xlast_ = Fl::event_x();
            ylast_ = Fl::event_y();
            break;
        }
        return ret;
    }

    void draw() override {
        fl_rectf(x(), y(), w(), h(), 200, 200, 200);
        if (type() == FL_HORIZONTAL) {
            for (int xx = x(), xz = x() + w(); xx != xz; ++xx) {
                if (!((xx - xlast_) & 0xf)) {
                    fl_line(xx, y(), xx, y() + h());
                }
            }
        }
        else {
            for (int yy = y(), yz = y() + h(); yy != yz; ++yy) {
                if (!((yy - ylast_) & 0xf)) {
                    fl_line(yy, x(), yy, x() + w());
                }
            }
        }
    }

    int xlast_;
    int ylast_;
};


Fl_Value_Slider *shuttle;
Fl_Scrubber *scrubber;
Fl_VideoFrame *frame;
Fl_Output *outY;
Fl_Output *outU;
Fl_Output *outV;
Fl_Output *outR;
Fl_Output *outG;
Fl_Output *outB;

Fl_Value_Input *outTime;

static double targetTime = 0.0;
static double actualTime = -1.0;

static double const DELTA_VALUE = 0.01f;

void shuttle_callback(Fl_Widget *, void *) {
    targetTime = shuttle->value();
}

void scrub_callback(Fl_Widget *, void *) {
    double v = scrubber->value() * DELTA_VALUE;
    double s = shuttle->value();
    if (v < 0) {
        if (s + v < shuttle->minimum()) {
            shuttle->value(0);
        }
        else {
            shuttle->value(s + v);
        }
    }
    else if (v > 0) {
        if (s + v > shuttle->maximum()) {
            shuttle->value(shuttle->maximum());
        }
        else {
            shuttle->value(s + v);
        }
    }
    targetTime = shuttle->value();
}

void build_gui() {
    shuttle = new Fl_Value_Slider(0, titleBarHeight + winHeight - oneRow, winWidth - scrubberWidth, oneRow, "");
    shuttle->type(FL_HORIZONTAL);
    shuttle->minimum(0);
    shuttle->maximum(finalFrameTime); // todo: get_max_timestamp()
    shuttle->callback(shuttle_callback, nullptr);
    scrubber = new Fl_Scrubber(winWidth - scrubberWidth, titleBarHeight + winHeight - oneRow, scrubberWidth, oneRow, "");
    scrubber->type(FL_HORIZONTAL);
    scrubber->bounds(-1, 1);
    scrubber->step(APPROXIMATE_FRAME_DURATION);
    scrubber->callback(scrub_callback, 0);
    frame = new Fl_VideoFrame(0, 0, 640, 480, "");
    outY = new Fl_Output(640 + colorLabelWidth, titleBarHeight, 30, oneRow, "Y");
    outU = new Fl_Output(640 + colorLabelWidth, titleBarHeight+oneRow, 30, oneRow, "U");
    outV = new Fl_Output(640 + colorLabelWidth, titleBarHeight+oneRow*2, 30, oneRow, "V");
    outR = new Fl_Output(640 + colorLabelWidth, titleBarHeight+oneRow*3, 30, oneRow, "R");
    outG = new Fl_Output(640 + colorLabelWidth, titleBarHeight+oneRow*4, 30, oneRow, "G");
    outB = new Fl_Output(640 + colorLabelWidth, titleBarHeight+oneRow*5, 30, oneRow, "B");
    outTime = new Fl_Value_Input(640 + colorLabelWidth*2, titleBarHeight + oneRow * 6, 120, oneRow, "Time");
}

void select_frame_time(uint64_t time) {
    shuttle->value(time * 1e-6);
    shuttle->do_callback();
}

void on_idle(void *) {
    if (targetTime != actualTime) {
        uint64_t time = (uint64_t)ceil(targetTime * 1e6);
        DecodedFrame *df = get_frame_at(time);
        if (!df) {
            fprintf(stderr, "ERROR: Could not find frame at time %lld\n", (uint64_t)time);
            return;
        }
        actualTime = df->time * 1e-6;
        targetTime = actualTime;
        frame->frame_ = df;
        frame->redraw();
        outTime->value(actualTime);
    }
}

int main(int argc, char const *argv[])
{
    std::string path;
    if (!argv[1]) {
        char const *cpath = fl_file_chooser("Choose a riff file", "*.riff", NULL);
        if (!cpath) {
            exit(1);
        }
        path = cpath;
    } else {
        path = argv[1];
    }

    if (!path.size() || !file_exists(path)) {
        exit(1);
    }

    load_all_riffs(path);
    analyze_all_riffs();

    Fl_Double_Window win(winWidth, winHeight+titleBarHeight, "Viewer");
    build_gui();
    win.end();
    win.show();
    mainWindow = &win;

    select_frame_time(0);
    shuttle_callback(shuttle, nullptr);

    Fl::add_idle(on_idle, nullptr);
    int ret = Fl::run();

    mainWindow = NULL;
    return ret;
}

