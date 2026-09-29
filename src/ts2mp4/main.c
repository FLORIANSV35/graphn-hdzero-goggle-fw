// Standalone remux tool: .ts -> .mp4, stream copy (no re-encode).
//
// Runs as its own process, launched on demand from the UI app (HDZGOGGLE)
// via system_exec(), because ffmpeg is only linked into this and the record
// executable -- see the "No ffmpeg is linked into any app binary" note in
// CMakeLists.txt. Mirrors ffmpeg's own remuxing.c example: copy each stream's
// codec parameters as-is and let the mp4 muxer reformat the H.264/H.265
// Annex-B packets from the .ts into the length-prefixed form mp4 needs.
//
// usage: ts2mp4 <input.ts> <output.mp4>
// exit 0 on success, non-zero (with a message on stderr) otherwise.

#include <stdio.h>
#include <stdlib.h>

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>

// The DVR always records at this resolution/audio format (record_definitions.h
// VI_WIDTH/VI_HEIGHT/AI_sampleRATE/AI_CHANNELS on the goggle side) -- used
// below as a fallback for whatever the demuxer couldn't determine itself.
#define DVR_VIDEO_WIDTH    1280
#define DVR_VIDEO_HEIGHT   720
#define DVR_AUDIO_RATE     48000
#define DVR_AUDIO_CHANNELS 2

static void print_ff_error(const char *prefix, int err) {
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "%s: %s\n", prefix, buf);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.ts> <output.mp4>\n", argv[0]);
        return 2;
    }

    const char *in_path = argv[1];
    const char *out_path = argv[2];

    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    int *stream_map = NULL;
    AVPacket *pkt = NULL;
    int ret = 0;

    // The DVR .ts doesn't repeat SPS/PPS/codec params often enough for the
    // default probe window, so avformat_find_stream_info() below was failing
    // to determine width/height/sample rate ("Could not find codec
    // parameters ... unspecified size"), which then made the mp4 muxer
    // reject the header ("dimensions not set"). Widen the probe explicitly.
    AVDictionary *open_opts = NULL;
    av_dict_set(&open_opts, "probesize", "50000000", 0);      // 50MB (default 5MB)
    av_dict_set(&open_opts, "analyzeduration", "10000000", 0); // 10s (default ~5s, sometimes reported as 0 for ts)

    ret = avformat_open_input(&ifmt_ctx, in_path, NULL, &open_opts);
    av_dict_free(&open_opts);
    if (ret < 0) {
        print_ff_error("could not open input", ret);
        return 1;
    }

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) {
        print_ff_error("could not read stream info", ret);
        avformat_close_input(&ifmt_ctx);
        return 1;
    }

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", out_path);
    if (!ofmt_ctx) {
        fprintf(stderr, "could not allocate mp4 output context\n");
        avformat_close_input(&ifmt_ctx);
        return 1;
    }

    stream_map = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_map) {
        fprintf(stderr, "out of memory\n");
        ret = -1;
        goto cleanup;
    }

    int out_index = 0;
    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        // Only video/audio -- drop anything else (data/subtitle streams the
        // DVR .ts doesn't carry anyway) rather than risk the mp4 muxer
        // rejecting an unsupported stream type outright.
        if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            stream_map[i] = -1;
            continue;
        }

        AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "could not allocate output stream\n");
            ret = -1;
            goto cleanup;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            print_ff_error("could not copy codec parameters", ret);
            goto cleanup;
        }
        out_stream->codecpar->codec_tag = 0; // let the mp4 muxer pick its own tag

        // Even with a wide probe, this demuxer can't reliably pull
        // width/height (video) or sample rate/channels (audio) out of this
        // stream's SPS/ADTS headers ("unspecified size" / "unspecified
        // sample rate" in ffmpeg's own log) -- the mp4 muxer then refuses to
        // write a header ("dimensions not set"). Fall back to the DVR's
        // fixed recording format whenever the probe came back empty; leave
        // it alone if the probe did manage to fill it in.
        if (out_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (out_stream->codecpar->width <= 0 || out_stream->codecpar->height <= 0) {
                out_stream->codecpar->width = DVR_VIDEO_WIDTH;
                out_stream->codecpar->height = DVR_VIDEO_HEIGHT;
            }
        } else if (out_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (out_stream->codecpar->sample_rate <= 0)
                out_stream->codecpar->sample_rate = DVR_AUDIO_RATE;
            if (out_stream->codecpar->channels <= 0) {
                out_stream->codecpar->channels = DVR_AUDIO_CHANNELS;
                out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
            }
        }

        stream_map[i] = out_index++;
    }

    if (out_index == 0) {
        fprintf(stderr, "no video/audio stream found in input\n");
        ret = -1;
        goto cleanup;
    }

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            print_ff_error("could not open output file", ret);
            goto cleanup;
        }
    }

    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        print_ff_error("could not write mp4 header", ret);
        goto cleanup;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "out of memory\n");
        ret = -1;
        goto cleanup;
    }

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index < 0 || (unsigned)pkt->stream_index >= ifmt_ctx->nb_streams ||
            stream_map[pkt->stream_index] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream *in_stream = ifmt_ctx->streams[pkt->stream_index];
        int const out_stream_index = stream_map[pkt->stream_index];
        AVStream *out_stream = ofmt_ctx->streams[out_stream_index];

        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->stream_index = out_stream_index;
        pkt->pos = -1;

        ret = av_interleaved_write_frame(ofmt_ctx, pkt); // takes ownership of pkt's data either way
        if (ret < 0) {
            print_ff_error("error muxing packet", ret);
            break;
        }
    }

    if (ret >= 0 || ret == AVERROR_EOF) {
        int const trailer_ret = av_write_trailer(ofmt_ctx);
        if (trailer_ret < 0) {
            print_ff_error("could not write mp4 trailer", trailer_ret);
            ret = trailer_ret;
        } else {
            ret = 0;
        }
    }

cleanup:
    if (pkt)
        av_packet_free(&pkt);
    if (ifmt_ctx)
        avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx) {
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE) && ofmt_ctx->pb)
            avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }
    free(stream_map);

    return (ret < 0) ? 1 : 0;
}
