#include "stdafx.h"
#include "video.h"
#include <vector>
#include <stdio.h>

extern "C" {

#pragma warning(disable: 4244)
#pragma warning(disable: 4996)
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/buffer.h>
#include <libavutil/log.h>

}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")

bool verbose = true;
bool firstTime = true;

extern std::vector<VideoFrame> gFrames;


class Decoder {
public:
    bool begin_decode(VideoFrame *frame);
    VideoFrame *decode_frame_and_advance(VideoFrame *frame, DecodedFrame *result);

    Decoder();
    ~Decoder();

    static AVCodec *codec;
    AVCodecContext *ctx = 0;
    AVFrame *frame = 0;
    AVCodecParserContext *parser = 0;
    AVPacket avp = { 0 };
    int frameno = 0;
    uint64_t ptsbase = 0;
    uint64_t dtsbase = 0;
    std::vector<char> readBuf;
};

AVCodec *Decoder::codec = nullptr;

Decoder::Decoder() {
}

Decoder::~Decoder() {
    //  todo: deallocate libav
}

bool Decoder::begin_decode(VideoFrame *indata) {
    readBuf.clear();
    if (firstTime) {
        avcodec_register_all();
        firstTime = false;
        if (verbose) {
            av_log_set_level(99);
        }
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        fprintf(stderr, "avcodec_find_decoder(): h264 not found\n");
        return false;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        fprintf(stderr, "avcodec_alloc_context3(): failed to allocate\n");
        return false;
    }
    ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2(): failed to open\n");
        return false;
    }
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "av_frame_alloc(): alloc failed\n");
        return false;
    }
    parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        fprintf(stderr, "av_parser_init(): h264 failed\n");
        return false;
    }
    memset(&avp, 0, sizeof(avp));
    av_init_packet(&avp);

    //  loop
    frameno = 0;
    ptsbase = 0;
    dtsbase = 0;
    avp.data = NULL;
    avp.size = 0;

    return true;
}

VideoFrame *Decoder::decode_frame_and_advance(VideoFrame *indata, DecodedFrame *result) {
    bool kf = false;
    uint64_t t = indata->time;
parse_more:
    if (!indata) {
        return nullptr;
    }
    indata->file->data_at(indata->offset, readBuf);
    if (indata->keyframe) {
        kf = true;
    }
    avp.pts = indata->pts;
    avp.dts = indata->pts;
    int lenParsed = av_parser_parse2(parser, ctx, &avp.data, &avp.size,
        (unsigned char *)&readBuf[0], readBuf.size(), avp.pts, avp.dts, avp.pos);
    if (verbose) {
        fprintf(stderr, "av_parser_parse2(): offset %lld lenParsed %d size %d pointer %p readbuf 0x%p\n",
            (long long)indata->offset, lenParsed, avp.size, avp.data, &readBuf[0]);
    }
    if (lenParsed) {
        if (indata->index + 1 >= gFrames.size()) {
            indata = nullptr;
        }
        else {
            indata = &gFrames[indata->index + 1];
        }
    }
    if (avp.size) {
        int lenSent = avcodec_send_packet(ctx, &avp);
        if (lenSent < 0) {
            if (verbose) {
                fprintf(stderr, "avcodec_send_packet(): error %d at concatoffset %ld\n",
                    lenSent, (long)avp.pos);
            }
        }
        avp.pos += avp.size;
        int err = avcodec_receive_frame(ctx, frame);
        if (err == 0) {
            //  got a frame!
            result->time = t;
            result->width = 640;
            result->height = 480;
            if (!result->yuv_planar) {
                result->yuv_planar = new unsigned char[640 * 480 + 320 * 240 * 2];
            }
            for (int r = 0; r != 480; ++r) {
                memcpy(result->yuv_planar + result->width * r, frame->data[0] + frame->linesize[0] * r, 640);
            }
            unsigned char *du = result->yuv_planar + 640 * 480;
            for (int r = 0; r != 240; ++r) {
                memcpy(du + 320 * r, frame->data[1] + frame->linesize[1] * r, 320);
            }
            unsigned char *dv = result->yuv_planar + 640 * 480 + 320 * 240;
            for (int r = 0; r != 240; ++r) {
                memcpy(dv + 320 * r, frame->data[2] + frame->linesize[2] * r, 320);
            }
            result->set_decoded(t, 640, 480, result->yuv_planar, kf);
            ++frameno;
            if (ctx->refcounted_frames) {
                av_frame_unref(frame);
            }
            if (lenParsed > 0) {
                readBuf.erase(readBuf.begin(), readBuf.begin() + lenParsed);
            }
            else {
                readBuf.clear();    //  didn't advance frame pointers
            }
            goto ret;
        }
        else if (err == AVERROR(EAGAIN)) {
            //  nothing for now
        }
        else if (err == AVERROR_EOF) {
            //  nothing for now
        }
        else {
            //  not a header
            if (indata->size > 128) {
                if (verbose) {
                    fprintf(stderr, "avcodec_receive_frame() error %d offset %ld file %s\n",
                        err, (long)indata->offset, indata->file->path_.string().c_str());
                }
                // return false;
            }
        }
    }
    if (lenParsed > 0) {
        readBuf.erase(readBuf.begin(), readBuf.begin() + lenParsed);
        goto parse_more;
    }
    else {
        readBuf.clear();    //  didn't advance frame pointers
    }
    //  it didn't consume any data, yet it didn't return a frame?
    fprintf(stderr, "ERROR in parser: lenParsed is 0 but no frame found index %d offset %lld file %s\n",
        indata->index, (long long)indata->offset, indata->file->path_.string().c_str());
    return nullptr;
ret:
    return indata;
}


Decoder *gDecoder;

void begin_decode(VideoFrame *frame) {
    delete gDecoder;
    gDecoder = new Decoder();
    gDecoder->begin_decode(frame);
}

VideoFrame *decode_frame_and_advance(VideoFrame *frame, DecodedFrame *result) {
    return gDecoder->decode_frame_and_advance(frame, result);
}

