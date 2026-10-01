// tsproxy: "light copy" of a DVR clip with the goggle's own hardware.
//
// Decodes the clip with the video decoder of the SoC, drops frames down to a lower frame
// rate, and re-encodes with its video encoder at a lower bit rate, so the clip can be played
// over the WiFi (see mkapp/app/portal). There is no software codec on the goggle (the ffmpeg
// in the firmware has no encoder or decoder), so everything goes through the MPP:
//
//     file --(libav)--> VDEC --(GetImage)--> VENC (SetFrameRate, CBR) --(GetStream)--> file
//
// The SDK's own demux, tunnelled to the decoder, stops feeding it after a few images when nothing
// displays them (clock and buffer handshakes made for the player), so the clip is read here with
// libav and every compressed frame is handed to the decoder with AW_MPI_VDEC_SendStream.
//
// Output: a name ending in .mp4 gives a playable .mp4 (video re-encoded, the audio copied as it
// is, index at the start of the file); any other name gives the raw encoded stream, for tests.
//
// usage: tsproxy <in.ts|in.mp4> <out.mp4|out.bin> [kbps=8000] [fps_div=2] [log=/mnt/extsd/tsproxy.log] [timeout_s=300] [progress-file]
//        env TSPROXY_FASTSTART=2: classic faststart (the muxer shifts the file) instead of reserving the index
//        exit 0 when the whole clip went through, non-zero otherwise.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <plat_type.h>
#include <tsemaphore.h>

#include <mm_common.h>
#include <mm_comm_rc.h>
#include <mm_comm_venc.h>
#include <mpi_sys.h>
#include <mpi_vdec.h>
#include <mpi_venc.h>

#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

static FILE *g_log;
static struct timeval g_t0;

static void logf_(const char *fmt, ...) {
    struct timeval now;
    gettimeofday(&now, NULL);
    long ms = (now.tv_sec - g_t0.tv_sec) * 1000 + (now.tv_usec - g_t0.tv_usec) / 1000;
    va_list ap;
    va_start(ap, fmt);
    if (g_log) {
        fprintf(g_log, "[%6ld ms] ", ms);
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)

static volatile int g_vdecEof;  // the decoder has seen the end of the clip
static VDEC_CHN g_vdec = MM_INVALID_CHN;
static volatile int g_inflight; // decoded images lent to the encoder, not given back yet
static volatile int g_encDone;  // the stream thread is told to finish
static unsigned long g_framesIn, g_framesFed, g_streamPacks;
static unsigned long long g_bytesOut;
static FILE *g_out;

static ERRORTYPE onMppEvent(void *cookie, MPP_CHN_S *pChn, MPP_EVENT_TYPE event, void *pEventData) {
    (void)cookie;
    (void)pEventData;
    if (pChn->mModId == MOD_ID_VDEC && event == MPP_EVENT_NOTIFY_EOF) {
        LOG("decoder: end of the clip");
        g_vdecEof = 1;
    }
    return SUCCESS;
}

// The encoder works on the decoder's own buffers (no copy): it tells us when it is done with a
// frame, and only then does the image go back to the decoder. Giving it back earlier, or never
// (the decoder keeps a few for itself), stops the decoder after a handful of images.
static ERRORTYPE onVencEvent(void *cookie, MPP_CHN_S *pChn, MPP_EVENT_TYPE event, void *pEventData) {
    (void)cookie;
    (void)pChn;
    if (event == MPP_EVENT_RELEASE_VIDEO_BUFFER && pEventData) {
        AW_MPI_VDEC_ReleaseImage(g_vdec, (VIDEO_FRAME_INFO_S *)pEventData);
        __sync_fetch_and_sub(&g_inflight, 1);
    }
    return SUCCESS;
}

static AVFormatContext *g_fmt;
static int g_vidx = -1, g_aidx = -1;

// ---- .mp4 output
static AVFormatContext *g_mux;
static AVStream *g_mv, *g_ma;                  // video (re-encoded) and audio (copied) streams of the output
static pthread_mutex_t g_muxLock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_muxReady;                // header written: packets can be sent
static AVPacket **g_aq;                        // audio packets that came before the header
static int g_aqN, g_aqCap;
static int64_t g_startUs;                      // start of the source: both tracks are shifted to begin at 0
static int g_dstFps = 30;
static const char *g_progress;
static int g_codec;

static void writeProgress(int percent) {
    if (!g_progress)
        return;
    FILE *f = fopen(g_progress, "w");
    if (f) {
        fprintf(f, "%d", percent);
        fclose(f);
    }
}

// Sends one audio packet to the output (or keeps it until the header is written).
static void sendAudio(AVPacket *src) {
    if (!g_mux || g_aidx < 0)
        return;
    AVPacket *c = av_packet_clone(src);
    if (!c)
        return;
    pthread_mutex_lock(&g_muxLock);
    if (!g_muxReady) {
        if (g_aqN == g_aqCap) {
            g_aqCap = g_aqCap ? g_aqCap * 2 : 256;
            g_aq = realloc(g_aq, g_aqCap * sizeof(*g_aq));
        }
        g_aq[g_aqN++] = c;
    } else {
        AVStream *as = g_fmt->streams[g_aidx];
        av_packet_rescale_ts(c, as->time_base, g_ma->time_base);
        c->pts -= av_rescale_q(g_startUs, (AVRational){1, 1000000}, g_ma->time_base);
        c->dts -= av_rescale_q(g_startUs, (AVRational){1, 1000000}, g_ma->time_base);
        c->stream_index = g_ma->index;
        av_interleaved_write_frame(g_mux, c);
        av_packet_free(&c);
    }
    pthread_mutex_unlock(&g_muxLock);
}
static volatile int g_feedDone;
static unsigned long g_packetsSent;

// The decoder takes Annex-B (start codes). mp4 packets are length-prefixed and keep their
// parameter sets (VPS/SPS/PPS) in the extradata: rebuild both. Transport streams are Annex-B already.
static int g_nalLen = 0;                         // 0: already Annex-B
static uint8_t *g_params;                        // parameter sets in Annex-B, put before every key frame
static int g_paramsLen;

static void appendSc(uint8_t **buf, int *len, const uint8_t *data, int n) {
    *buf = realloc(*buf, *len + 4 + n);
    memcpy(*buf + *len, "\0\0\0\1", 4);
    memcpy(*buf + *len + 4, data, n);
    *len += 4 + n;
}

static void parseExtradata(const AVCodecParameters *par) {
    const uint8_t *e = par->extradata;
    int n = par->extradata_size;
    if (n < 7 || (n >= 3 && e[0] == 0 && e[1] == 0 && (e[2] == 1 || (e[2] == 0 && e[3] == 1))))
        return;                                  // empty or Annex-B already
    if (par->codec_id == AV_CODEC_ID_HEVC && n > 23) {          // hvcC
        g_nalLen = (e[21] & 3) + 1;
        int arrays = e[22], pos = 23;
        for (int a = 0; a < arrays && pos + 3 <= n; a++) {
            int cnt = (e[pos + 1] << 8) | e[pos + 2];
            pos += 3;
            for (int k = 0; k < cnt && pos + 2 <= n; k++) {
                int l = (e[pos] << 8) | e[pos + 1];
                pos += 2;
                if (pos + l > n) break;
                appendSc(&g_params, &g_paramsLen, e + pos, l);
                pos += l;
            }
        }
    } else if (par->codec_id == AV_CODEC_ID_H264) {             // avcC
        g_nalLen = (e[4] & 3) + 1;
        int pos = 5, cnt = e[pos++] & 0x1f;
        for (int k = 0; k < cnt && pos + 2 <= n; k++) {
            int l = (e[pos] << 8) | e[pos + 1];
            pos += 2;
            if (pos + l > n) break;
            appendSc(&g_params, &g_paramsLen, e + pos, l);
            pos += l;
        }
        if (pos < n) {
            cnt = e[pos++];
            for (int k = 0; k < cnt && pos + 2 <= n; k++) {
                int l = (e[pos] << 8) | e[pos + 1];
                pos += 2;
                if (pos + l > n) break;
                appendSc(&g_params, &g_paramsLen, e + pos, l);
                pos += l;
            }
        }
    }
}

// Reads the clip and sends every frame to the decoder, then tells it the stream is over.
static void *feedThread(void *arg) {
    VDEC_CHN vdec = *(VDEC_CHN *)arg;
    AVPacket *pkt = av_packet_alloc();
    uint8_t *buf = NULL;
    int cap = 0;
    AVStream *vs = g_fmt->streams[g_vidx];

    while (!g_encDone && av_read_frame(g_fmt, pkt) >= 0) {
        if (pkt->stream_index == g_aidx) {
            sendAudio(pkt);
            av_packet_unref(pkt);
            continue;
        }
        if (pkt->stream_index != g_vidx) {
            av_packet_unref(pkt);
            continue;
        }
        int len = 0;
        uint8_t *out = NULL;
        if (g_nalLen == 0) {                                      // Annex-B: as it is
            out = pkt->data;
            len = pkt->size;
        } else {
            if (pkt->flags & AV_PKT_FLAG_KEY && g_paramsLen) {
                if (cap < g_paramsLen) { cap = g_paramsLen + pkt->size + 1024; buf = realloc(buf, cap); }
                memcpy(buf, g_params, g_paramsLen);
                len = g_paramsLen;
            }
            int pos = 0;
            while (pos + g_nalLen <= pkt->size) {
                int l = 0;
                for (int b = 0; b < g_nalLen; b++)
                    l = (l << 8) | pkt->data[pos + b];
                pos += g_nalLen;
                if (l <= 0 || pos + l > pkt->size) break;
                if (cap < len + 4 + l) { cap = len + 4 + l + 4096; buf = realloc(buf, cap); }
                memcpy(buf + len, "\0\0\0\1", 4);
                memcpy(buf + len + 4, pkt->data + pos, l);
                len += 4 + l;
                pos += l;
            }
            out = buf;
        }
        VDEC_STREAM_S st;
        memset(&st, 0, sizeof(st));
        st.pAddr = out;
        st.mLen = (unsigned int)len;
        st.mPTS = (uint64_t)av_rescale_q(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, vs->time_base, (AVRational){1, 1000000});
        st.mbEndOfFrame = TRUE;
        st.mbEndOfStream = FALSE;
        ERRORTYPE ret;
        int tries = 0;
        while ((ret = AW_MPI_VDEC_SendStream(vdec, &st, 1000)) != SUCCESS && !g_encDone && ++tries < 30)
            ;                                                     // the decoder's input is full: wait for it
        if (ret != SUCCESS && g_packetsSent < 3)
            LOG("send stream: %x", ret);
        g_packetsSent++;
        if (g_packetsSent % 25 == 0 && g_fmt->duration > 0 && pkt->pts != AV_NOPTS_VALUE) {
            int64_t us = av_rescale_q(pkt->pts, vs->time_base, (AVRational){1, 1000000}) - g_startUs;
            int pct = (int)(us * 100 / g_fmt->duration);
            writeProgress(pct < 0 ? 0 : pct > 99 ? 99 : pct);
        }
        av_packet_unref(pkt);
    }
    LOG("clip read: %lu packets sent to the decoder", g_packetsSent);
    VDEC_STREAM_S eos;
    memset(&eos, 0, sizeof(eos));
    eos.mbEndOfFrame = TRUE;
    eos.mbEndOfStream = TRUE;
    AW_MPI_VDEC_SendStream(vdec, &eos, 1000);
    AW_MPI_VDEC_SetStreamEof(vdec, TRUE);
    free(buf);
    av_packet_free(&pkt);
    g_feedDone = 1;
    return NULL;
}

// Frames per second of the video: what the container says, else counted over the first packets.
// Frame rate of the clip, measured on the time stamps of its first 300 images: the rate declared by the
// container (avg_frame_rate) is only a guess for a .ts and is wrong on some clips.
static int g_declFps, g_measFps;                       // for --probe: what the container says / what we measured

static int clipFps(AVFormatContext *fmt, int vidx) {
    AVStream *vs = fmt->streams[vidx];
    AVRational r = vs->avg_frame_rate;
    int decl = r.den > 0 ? (int)((r.num + r.den / 2) / r.den) : 0;
    AVPacket *pk = av_packet_alloc();
    int64_t first = AV_NOPTS_VALUE, lastTs = AV_NOPTS_VALUE;
    int n = 0;
    while (n < 300 && av_read_frame(fmt, pk) >= 0) {
        if (pk->stream_index == vidx) {
            int64_t t = pk->dts != AV_NOPTS_VALUE ? pk->dts : pk->pts;
            if (t != AV_NOPTS_VALUE) {
                if (first == AV_NOPTS_VALUE)
                    first = t;
                lastTs = t;
                n++;
            }
        }
        av_packet_unref(pk);
    }
    av_packet_free(&pk);
    av_seek_frame(fmt, vidx, 0, AVSEEK_FLAG_BACKWARD);
    int meas = 0;
    if (n >= 10 && lastTs > first) {
        double secs = (lastTs - first) * av_q2d(vs->time_base);
        meas = (int)((n - 1) / secs + 0.5);
    }
    g_declFps = decl;
    g_measFps = meas;
    if (meas >= 5 && meas <= 240)
        return meas;
    return decl >= 5 && decl <= 240 ? decl : 0;
}

// NAL unit types: key frame? parameter set?
static int isKey(const uint8_t *d, int n) {
    for (int k = 0; k + 4 < n && k < 4096; k++) {
        if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) {
            int t = g_codec == PT_H265 ? (d[k + 3] >> 1) & 0x3f : d[k + 3] & 0x1f;
            if (g_codec == PT_H265 ? (t >= 16 && t <= 21) : t == 5)
                return 1;
        }
    }
    return 0;
}

// The parameter sets (VPS/SPS/PPS, or SPS/PPS) of an Annex-B buffer: what the mp4 index needs.
static int paramSets(const uint8_t *d, int n, uint8_t *out) {
    int len = 0;
    for (int k = 0; k + 4 < n; k++) {
        if (!(d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1))
            continue;
        int t = g_codec == PT_H265 ? (d[k + 3] >> 1) & 0x3f : d[k + 3] & 0x1f;
        int isps = g_codec == PT_H265 ? (t >= 32 && t <= 34) : (t == 7 || t == 8);
        if (!isps)
            continue;
        int e = k + 3;                                  // up to the next start code
        while (e + 3 < n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (d[e + 2] == 0 && d[e + 3] == 1))))
            e++;
        if (e + 3 >= n)
            e = n;
        memcpy(out + len, "\0\0\0\1", 4);
        memcpy(out + len + 4, d + k + 3, e - k - 3);
        len += 4 + e - k - 3;
    }
    return len;
}

static uint8_t g_hdr[512];
static int g_hdrLen;                                    // parameter sets from the encoder, if it gave them apart
static int64_t g_firstPtsUs = -1;                       // pts of the first decoded image
static unsigned long g_muxFrames;

// Writes the .mp4 header once the first encoded frame gives the parameter sets, then the audio kept so far.
static int muxStart(const uint8_t *first, int n, int W, int H) {
    uint8_t *ps = malloc(n + g_hdrLen + 64);
    int len = 0;
    if (g_hdrLen) {
        memcpy(ps, g_hdr, g_hdrLen);
        len = paramSets(g_hdr, g_hdrLen, ps);
    }
    if (len == 0)
        len = paramSets(first, n, ps);
    LOG("mp4 header: %d bytes of parameter sets", len);
    if (len == 0) {
        free(ps);
        return -1;
    }
    AVCodecParameters *vp = g_mv->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = g_codec == PT_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    vp->codec_tag = g_codec == PT_H265 ? MKTAG('h', 'v', 'c', '1') : 0;   // Safari and iOS refuse the muxer's own hev1
    vp->width = W;
    vp->height = H;
    vp->extradata = av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(vp->extradata, ps, len);
    vp->extradata_size = len;
    free(ps);
    g_mv->time_base = (AVRational){1, 90000};
    g_mv->avg_frame_rate = (AVRational){g_dstFps, 1};

    AVDictionary *opt = NULL;
    const char *fs = getenv("TSPROXY_FASTSTART");
    if (fs && strcmp(fs, "2") == 0) {
        av_dict_set(&opt, "movflags", "faststart", 0);
    } else {                                           // room for the index at the start, written there at the end
        double secs = g_fmt->duration > 0 ? g_fmt->duration / (double)AV_TIME_BASE : 60;
        char sz[24];
        snprintf(sz, sizeof(sz), "%lld", (long long)(secs * 8000) + 262144);
        av_dict_set(&opt, "moov_size", sz, 0);
    }
    int r = avformat_write_header(g_mux, &opt);
    av_dict_free(&opt);
    if (r < 0) {
        LOG("cannot write the mp4 header: %d", r);
        return -1;
    }
    pthread_mutex_lock(&g_muxLock);
    g_muxReady = 1;
    AVStream *as = g_aidx >= 0 ? g_fmt->streams[g_aidx] : NULL;
    for (int k = 0; k < g_aqN; k++) {
        AVPacket *c = g_aq[k];
        av_packet_rescale_ts(c, as->time_base, g_ma->time_base);
        c->pts -= av_rescale_q(g_startUs, (AVRational){1, 1000000}, g_ma->time_base);
        c->dts -= av_rescale_q(g_startUs, (AVRational){1, 1000000}, g_ma->time_base);
        c->stream_index = g_ma->index;
        av_interleaved_write_frame(g_mux, c);
        av_packet_free(&c);
    }
    g_aqN = 0;
    pthread_mutex_unlock(&g_muxLock);
    return 0;
}

// Takes the encoded packs out of the encoder: into the .mp4, or appended to the raw file.
static void *streamThread(void *arg) {
    VENC_CHN ch = *(VENC_CHN *)arg;
    VENC_STREAM_S stream;
    VENC_PACK_S pack;
    int idle = 0;
    uint8_t *frame = NULL;
    int cap = 0;

    while (!g_encDone || idle < 3) {
        memset(&stream, 0, sizeof(stream));
        stream.mpPack = &pack;                      // same way the recorder asks for one pack at a time
        stream.mPackCount = 1;
        ERRORTYPE ret = AW_MPI_VENC_GetStream(ch, &stream, 500);
        if (ret != SUCCESS) {
            if (g_encDone)
                idle++;
            continue;
        }
        idle = 0;
        int n = 0;
        if (stream.mpPack) {
            int need = (int)(stream.mpPack->mLen0 + stream.mpPack->mLen1);
            if (cap < need) { cap = need + 65536; frame = realloc(frame, cap); }
            if (stream.mpPack->mLen0) { memcpy(frame, stream.mpPack->mpAddr0, stream.mpPack->mLen0); n = stream.mpPack->mLen0; }
            if (stream.mpPack->mLen1) { memcpy(frame + n, stream.mpPack->mpAddr1, stream.mpPack->mLen1); n += stream.mpPack->mLen1; }
        }
        AW_MPI_VENC_ReleaseStream(ch, &stream);
        if (n > 0) {
            g_bytesOut += n;
            g_streamPacks++;
            if (g_mux) {
                if (!g_muxReady && muxStart(frame, n, g_mv->codecpar->width, g_mv->codecpar->height) < 0) {
                    g_encDone = 1;
                    break;
                }
                AVPacket *pk = av_packet_alloc();
                av_new_packet(pk, n);
                memcpy(pk->data, frame, n);
                int64_t step = 90000 / g_dstFps;
                int64_t base = (g_firstPtsUs > g_startUs ? g_firstPtsUs - g_startUs : 0) * 90000 / 1000000;
                pk->pts = pk->dts = base + (int64_t)g_muxFrames * step;
                pk->duration = step;
                pk->stream_index = g_mv->index;
                pk->flags = isKey(frame, n) ? AV_PKT_FLAG_KEY : 0;
                g_muxFrames++;
                pthread_mutex_lock(&g_muxLock);
                av_interleaved_write_frame(g_mux, pk);
                pthread_mutex_unlock(&g_muxLock);
                av_packet_free(&pk);
            } else {
                fwrite(frame, 1, n, g_out);
            }
        }
    }
    free(frame);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc == 3 && strcmp(argv[1], "--probe") == 0) {           // tsproxy --probe clip: prints "fps=N" (0 if unknown)
        AVFormatContext *f = NULL;
        int fps = 0;
        if (avformat_open_input(&f, argv[2], NULL, NULL) >= 0 && avformat_find_stream_info(f, NULL) >= 0) {
            int vi = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
            if (vi >= 0)
                fps = clipFps(f, vi);
        }
        printf("fps=%d\ndeclared=%d\nmeasured=%d\n", fps, g_declFps, g_measFps);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.ts|in.mp4> <out.h264|out.h265> [kbps=8000] [fps_div=2] [log] [timeout_s=300]\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    int kbps = argc > 3 ? atoi(argv[3]) : 8000;
    int fps_div = argc > 4 ? atoi(argv[4]) : 0;         // 0: automatic
    const char *log_path = argc > 5 ? argv[5] : "/mnt/extsd/tsproxy.log";
    int timeout_s = argc > 6 ? atoi(argv[6]) : 300;       // a hung MPP call must not hang the goggle: SIGALRM ends us
    g_progress = argc > 7 ? argv[7] : NULL;               // gets "0".."100" while it works, for the page
    if (kbps < 500) kbps = 500;

    gettimeofday(&g_t0, NULL);
    g_log = fopen(log_path, "a");
    if (timeout_s > 0)
        alarm(timeout_s);
    LOG("=== tsproxy %s -> %s, %d kbps, fps/%d", in_path, out_path, kbps, fps_div);

    int rc = 1;
    VDEC_CHN vdec = MM_INVALID_CHN;
    VENC_CHN venc = MM_INVALID_CHN;
    pthread_t thr, feed;
    int thrStarted = 0, feedStarted = 0;
    ERRORTYPE ret;

    // ---- the clip, read with libav
    if (avformat_open_input(&g_fmt, in_path, NULL, NULL) < 0 || avformat_find_stream_info(g_fmt, NULL) < 0) {
        LOG("cannot open the clip");
        goto done;
    }
    g_vidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (g_vidx < 0) {
        LOG("no video in the clip");
        goto done;
    }
    const AVCodecParameters *par = g_fmt->streams[g_vidx]->codecpar;
    int W = par->width > 0 ? par->width : 1280;
    int H = par->height > 0 ? par->height : 720;
    int srcFps = clipFps(g_fmt, g_vidx);
    if (srcFps < 5)
        srcFps = 60;                                   // unknown: the DVR records at 60 (or 90) frames per second
    if (fps_div <= 0)                                  // automatic: bring it to 30 images per second at most
        fps_div = (srcFps + 29) / 30;
    if (fps_div < 1)
        fps_div = 1;
    int dstFps = srcFps / fps_div > 0 ? srcFps / fps_div : 1;
    PAYLOAD_TYPE_E codec = par->codec_id == AV_CODEC_ID_HEVC ? PT_H265 : par->codec_id == AV_CODEC_ID_H264 ? PT_H264 : PT_BUTT;
    LOG("clip: %dx%d, %d fps, %s, %.1f s -> %d fps", W, H, srcFps, avcodec_get_name(par->codec_id), g_fmt->duration / 1e6, dstFps);
    if (codec == PT_BUTT) {
        LOG("not an H.264 / H.265 clip");
        goto done;
    }
    parseExtradata(par);
    g_aidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, g_vidx, NULL, 0);
    g_startUs = g_fmt->start_time != AV_NOPTS_VALUE ? g_fmt->start_time : 0;
    g_codec = codec;
    g_dstFps = dstFps;
    LOG("audio: %s", g_aidx >= 0 ? avcodec_get_name(g_fmt->streams[g_aidx]->codecpar->codec_id) : "none");
    LOG("packets are %s, %d bytes of parameter sets", g_nalLen ? "length-prefixed (mp4)" : "Annex-B", g_paramsLen);

    // ---- MPP system, same as the recorder and the player
    MPP_SYS_CONF_S conf;
    memset(&conf, 0, sizeof(conf));
    conf.nAlignWidth = 32;
    AW_MPI_SYS_SetConf(&conf);
    ret = AW_MPI_SYS_Init();
    LOG("sys init: %x", ret);
    if (ret != SUCCESS)
        goto done;

    // ---- decoder: output in the format the encoder takes (as the recorder gives it)
    VDEC_CHN_ATTR_S vattr;
    memset(&vattr, 0, sizeof(vattr));
    vattr.mPicWidth = W;
    vattr.mPicHeight = H;
    vattr.mInitRotation = 0;
    vattr.mOutputPixelFormat = MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    vattr.mType = codec;
    vattr.mVdecVideoAttr.mSupportBFrame = 0;
    vattr.mVdecVideoAttr.mMode = VIDEO_MODE_FRAME;
    for (vdec = 0; vdec < VDEC_MAX_CHN_NUM; vdec++) {
        ret = AW_MPI_VDEC_CreateChn(vdec, &vattr);
        if (ret == SUCCESS)
            break;
        if (ret != ERR_VDEC_EXIST)
            LOG("vdec channel %d: %x", vdec, ret);
    }
    if (vdec >= VDEC_MAX_CHN_NUM) {
        vdec = MM_INVALID_CHN;
        LOG("no decoder channel");
        goto done;
    }
    g_vdec = vdec;
    LOG("decoder channel %d", vdec);
    MPPCallbackInfo cb;
    cb.cookie = NULL;
    cb.callback = (MPPCallbackFuncType)&onMppEvent;
    AW_MPI_VDEC_RegisterCallback(vdec, &cb);

    // ---- encoder
    VENC_CHN_ATTR_S eattr;
    memset(&eattr, 0, sizeof(eattr));
    eattr.VeAttr.Type = codec;
    eattr.VeAttr.MaxKeyInterval = dstFps;           // an I frame every second
    eattr.VeAttr.SrcPicWidth = W;
    eattr.VeAttr.SrcPicHeight = H;
    eattr.VeAttr.PixelFormat = MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    eattr.VeAttr.Field = VIDEO_FIELD_FRAME;
    if (codec == PT_H264) {
        eattr.VeAttr.AttrH264e.bByFrame = TRUE;
        eattr.VeAttr.AttrH264e.Profile = 1;          // main: what a phone decodes everywhere
        eattr.VeAttr.AttrH264e.PicWidth = W;
        eattr.VeAttr.AttrH264e.PicHeight = H;
        eattr.RcAttr.mRcMode = VENC_RC_MODE_H264CBR;
        eattr.RcAttr.mAttrH264Cbr.mSrcFrmRate = srcFps;
        eattr.RcAttr.mAttrH264Cbr.mBitRate = kbps * 1000;
    } else {
        eattr.VeAttr.AttrH265e.mbByFrame = TRUE;
        eattr.VeAttr.AttrH265e.mProfile = 0;         // main
        eattr.VeAttr.AttrH265e.mPicWidth = W;
        eattr.VeAttr.AttrH265e.mPicHeight = H;
        eattr.RcAttr.mRcMode = VENC_RC_MODE_H265CBR;
        eattr.RcAttr.mAttrH265Cbr.mSrcFrmRate = srcFps;
        eattr.RcAttr.mAttrH265Cbr.mBitRate = kbps * 1000;
    }
    for (venc = 0; venc < VENC_MAX_CHN_NUM; venc++) {
        ret = AW_MPI_VENC_CreateChn(venc, &eattr);
        if (ret == SUCCESS)
            break;
        LOG("venc channel %d: %x", venc, ret);
    }
    if (venc >= VENC_MAX_CHN_NUM) {
        venc = MM_INVALID_CHN;
        LOG("no encoder channel");
        goto done;
    }
    LOG("encoder channel %d", venc);
    MPPCallbackInfo vcb;
    vcb.cookie = NULL;
    vcb.callback = (MPPCallbackFuncType)&onVencEvent;
    AW_MPI_VENC_RegisterCallback(venc, &vcb);
    VENC_FRAME_RATE_S fr;
    fr.SrcFrmRate = srcFps;
    fr.DstFrmRate = dstFps;
    ret = AW_MPI_VENC_SetFrameRate(venc, &fr);
    LOG("frame rate %d -> %d: %x", srcFps, dstFps, ret);

    VencHeaderData hdr;
    memset(&hdr, 0, sizeof(hdr));
    ret = (codec == PT_H264) ? AW_MPI_VENC_GetH264SpsPpsInfo(venc, &hdr) : AW_MPI_VENC_GetH265SpsPpsInfo(venc, &hdr);
    LOG("parameter sets: %x, %u bytes", ret, hdr.nLength);
    if (ret == SUCCESS && hdr.nLength && hdr.pBuffer && hdr.nLength <= sizeof(g_hdr)) {
        memcpy(g_hdr, hdr.pBuffer, hdr.nLength);
        g_hdrLen = (int)hdr.nLength;
    }

    size_t olen = strlen(out_path);
    if (olen > 4 && strcmp(out_path + olen - 4, ".mp4") == 0) {          // a playable .mp4
        if (avformat_alloc_output_context2(&g_mux, NULL, "mp4", out_path) < 0) {
            LOG("cannot create the mp4 output");
            goto done;
        }
        g_mv = avformat_new_stream(g_mux, NULL);
        if (g_aidx >= 0) {
            g_ma = avformat_new_stream(g_mux, NULL);
            avcodec_parameters_copy(g_ma->codecpar, g_fmt->streams[g_aidx]->codecpar);
            g_ma->codecpar->codec_tag = 0;
        }
        if (avio_open(&g_mux->pb, out_path, AVIO_FLAG_WRITE) < 0) {
            LOG("cannot write %s: %s", out_path, strerror(errno));
            goto done;
        }
        g_mv->codecpar->width = W;                                       // the header is written with the first frame
        g_mv->codecpar->height = H;
    } else {
        g_out = fopen(out_path, "wb");
        if (!g_out) {
            LOG("cannot write %s: %s", out_path, strerror(errno));
            goto done;
        }
        if (g_hdrLen) {
            fwrite(g_hdr, 1, g_hdrLen, g_out);
            g_bytesOut += g_hdrLen;
        }
    }

    ret = AW_MPI_VENC_StartRecvPic(venc);
    LOG("encoder start: %x", ret);
    if (ret != SUCCESS)
        goto done;
    pthread_create(&thr, NULL, streamThread, &venc);
    thrStarted = 1;

    // ---- go
    ret = AW_MPI_VDEC_StartRecvStream(vdec);
    LOG("decoder start: %x", ret);
    pthread_create(&feed, NULL, feedThread, &vdec);
    feedStarted = 1;

    // Pull the decoded images and hand them to the encoder.
    int noImage = 0;
    int shown = 0;
    for (;;) {
        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));
        ret = AW_MPI_VDEC_GetImage(vdec, &frame, 200);
        if (ret != SUCCESS) {
            noImage++;
            if (noImage % 10 == 0) {                   // what the decoder is doing while nothing comes out
                VDEC_CHN_STAT_S st;
                memset(&st, 0, sizeof(st));
                if (AW_MPI_VDEC_Query(vdec, &st) == SUCCESS)
                    LOG("decoder: received %u, decoded %u, waiting %u frames (%u bytes), %u pictures to output, lent %d, sent %lu",
                        st.mRecvStreamFrames, st.mDecodeStreamFrames, st.mLeftStreamFrames, st.mLeftStreamBytes, st.mLeftPics, g_inflight, g_packetsSent);
            }
            // finished: everything was sent and the decoder said it reached the end (or nothing came for a while)
            if ((g_vdecEof && noImage >= 5) || (g_feedDone && noImage >= 30))
                break;
            if (noImage > 100) {                      // 20 s with nothing at all: it is stuck
                LOG("no image for 20 s, giving up");
                break;
            }
            continue;
        }
        noImage = 0;
        g_framesIn++;
        if (g_firstPtsUs < 0)
            g_firstPtsUs = (int64_t)frame.VFrame.mpts;
        if (shown < 3) {
            shown++;
            LOG("image %lu: %ux%u format %d, stride %u, pts %llu us", g_framesIn, frame.VFrame.mWidth, frame.VFrame.mHeight,
                (int)frame.VFrame.mPixelFormat, frame.VFrame.mStride[0], (unsigned long long)frame.VFrame.mpts);
        }
        for (int w = 0; g_inflight >= 4 && w < 400; w++)     // do not lend more than a few at once
            usleep(5000);
        __sync_fetch_and_add(&g_inflight, 1);
        ret = AW_MPI_VENC_SendFrame(venc, &frame, 2000);
        if (ret == SUCCESS) {
            g_framesFed++;
        } else {                                              // not taken: it comes back to the decoder now
            __sync_fetch_and_sub(&g_inflight, 1);
            AW_MPI_VDEC_ReleaseImage(vdec, &frame);
            if (g_framesIn - g_framesFed < 5)
                LOG("send to the encoder: %x", ret);
        }

        if (g_framesIn % 600 == 0)
            LOG("progress: %lu images in, %lu fed, %llu bytes out", g_framesIn, g_framesFed, g_bytesOut);
    }
    for (int w = 0; g_inflight > 0 && w < 200; w++)           // let the encoder give the last images back
        usleep(25000);
    LOG("images still lent to the encoder: %d", g_inflight);
    LOG("decoding over: %lu images, %lu fed to the encoder", g_framesIn, g_framesFed);
    rc = g_framesFed > 0 ? 0 : 1;

done:
    g_encDone = 1;
    if (feedStarted)
        pthread_join(feed, NULL);
    if (thrStarted)
        pthread_join(thr, NULL);
    if (g_out)
        fclose(g_out);
    if (g_mux) {
        if (g_muxReady) {
            int r = av_write_trailer(g_mux);
            if (r < 0) {
                LOG("cannot finish the mp4 (%d): the index did not fit, run again with TSPROXY_FASTSTART=2", r);
                rc = 1;
            }
        } else {
            LOG("no encoded frame reached the mp4");
            rc = 1;
        }
        if (g_mux->pb)
            avio_closep(&g_mux->pb);
        avformat_free_context(g_mux);
    }
    if (venc != MM_INVALID_CHN) {
        AW_MPI_VENC_StopRecvPic(venc);
        AW_MPI_VENC_ResetChn(venc);
        AW_MPI_VENC_DestroyChn(venc);
    }
    if (vdec != MM_INVALID_CHN) {
        AW_MPI_VDEC_StopRecvStream(vdec);
        AW_MPI_VDEC_DestroyChn(vdec);
    }
    AW_MPI_SYS_Exit();
    if (g_fmt)
        avformat_close_input(&g_fmt);

    double secs = 0;
    if (g_log) {
        struct timeval now;
        gettimeofday(&now, NULL);
        secs = (now.tv_sec - g_t0.tv_sec) + (now.tv_usec - g_t0.tv_usec) / 1e6;
    }
    if (rc == 0)
        writeProgress(100);
    LOG("result: %lu images in, %lu fed, %lu packs out, %llu bytes, took %.1f s (rc %d)", g_framesIn, g_framesFed, g_streamPacks, g_bytesOut, secs, rc);
    if (g_log)
        fclose(g_log);
    return rc;
}
