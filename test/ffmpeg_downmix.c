/* Exercise the production FFmpeg matrix and conversion cache, not downmix.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
/* STANDALONE's allocator aliases omit this inline stream-header helper. */
#define astrdup strdup
#include "stream.h"
#include "debug.h"

/* Codec registration is unnecessary for a conversion-only executable. */
#undef STREAM_REGISTER_DEC_AUDIO
#define STREAM_REGISTER_DEC_AUDIO(fmt, dec, channels)
#include "../Source/codec_ffmpeg_audio.c"

const int Debug[DBG_MAX_ENTRIES] = {0};
static int checks;

static AVFrame *signal_frame(const char *layout, enum AVSampleFormat format,
    enum AVChannel source, int amplitude)
{
    AVFrame *f = av_frame_alloc();
    assert(f && av_channel_layout_from_string(&f->ch_layout, layout) == 0);
    f->sample_rate = 48000;
    f->format = format;
    f->nb_samples = 32;
    assert(av_frame_get_buffer(f, 0) == 0);
    assert(av_samples_set_silence(f->extended_data, 0, f->nb_samples,
        f->ch_layout.nb_channels, format) == 0);
    int channel = av_channel_layout_index_from_channel(&f->ch_layout, source);
    assert(channel >= 0);
    int planar = av_sample_fmt_is_planar(format);
    int bytes = av_get_bytes_per_sample(format);
    for (int i = 0; i < f->nb_samples; i++) {
        uint8_t *dest = f->extended_data[planar ? channel : 0] + bytes *
            (planar ? i : i * f->ch_layout.nb_channels + channel);
        switch (av_get_packed_sample_fmt(format)) {
        case AV_SAMPLE_FMT_S16: *(int16_t *)dest = amplitude; break;
        case AV_SAMPLE_FMT_S32: *(int32_t *)dest = amplitude * 65536; break;
        case AV_SAMPLE_FMT_FLT: *(float *)dest = amplitude / 32768.f; break;
        case AV_SAMPLE_FMT_DBL: *(double *)dest = amplitude / 32768.; break;
        default: assert(0);
        }
    }
    return f;
}

static void check_stereo(PRIV *p, AVFrame *f, int left, int right)
{
    UCHAR *output = NULL;
    int channels = 0, bits = 0;
    int bytes = convert(p, f, &output, &channels, &bits);
    assert(bytes == f->nb_samples * 4 && channels == 2 && bits == 16);
    int16_t *samples = (int16_t *)output;
    for (int i = 0; i < f->nb_samples; i++) {
        assert(abs(samples[2*i] - left) <= 1);
        assert(abs(samples[2*i+1] - right) <= 1);
        checks += 2;
    }
}

static void cleanup(PRIV *p)
{
    swr_free(&p->swr_ctx);
    av_channel_layout_uninit(&p->swr_in_layout);
    av_channel_layout_uninit(&p->swr_out_layout);
    afree(p->bsamples);
}

int main(void)
{
    const enum AVSampleFormat formats[] = {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32,
        AV_SAMPLE_FMT_DBLP, AV_SAMPLE_FMT_DBL
    };
    const char *layouts[] = {"5.1(side)", "5.1", "7.1"};
    for (unsigned layout = 0; layout < sizeof(layouts)/sizeof(*layouts); layout++) {
        for (unsigned fmt = 0; fmt < sizeof(formats)/sizeof(*formats); fmt++) {
            PRIV p = {.request_channels = 2};
            AVFrame *f = signal_frame(layouts[layout], formats[fmt], AV_CHAN_LOW_FREQUENCY, 8192);
            /* Toggle on an existing context: a stale matrix must fail this test. */
            for (int enabled = 0; enabled < 4; enabled++) {
                ffmpeg_audio_set_downmix_lfe_boost(enabled % 2);
                int expected = enabled % 2 ? 5793 : 4096;
                check_stereo(&p, f, expected, expected);
            }
            av_frame_free(&f);
            /* Center and side/back contributions remain independent of LFE boost. */
            f = signal_frame(layouts[layout], formats[fmt], AV_CHAN_FRONT_CENTER, 8192);
            check_stereo(&p, f, 5793, 5793);
            av_frame_free(&f);
            enum AVChannel surround = layout == 1 ? AV_CHAN_BACK_LEFT : AV_CHAN_SIDE_LEFT;
            f = signal_frame(layouts[layout], formats[fmt], surround, 8192);
            check_stereo(&p, f, 5793, 0);
            av_frame_free(&f);
            cleanup(&p);
        }
    }
    /* Mono and stereo are unchanged even with the boost selected. */
    PRIV p = {.request_channels = 2};
    AVFrame *f = signal_frame("mono", AV_SAMPLE_FMT_FLTP, AV_CHAN_FRONT_CENTER, 8192);
    check_stereo(&p, f, 8192, 8192);
    av_frame_free(&f);
    f = signal_frame("stereo", AV_SAMPLE_FMT_FLTP, AV_CHAN_FRONT_LEFT, -8192);
    check_stereo(&p, f, -8192, 0);
    av_frame_free(&f);
    /* Existing hard saturation is retained; no limiter or normalization is added. */
    f = signal_frame("5.1", AV_SAMPLE_FMT_FLTP, AV_CHAN_FRONT_CENTER, 32767);
    for (int i=0; i<f->nb_samples; i++) {
        ((float *)f->extended_data[0])[i] = 1.f;
        ((float *)f->extended_data[1])[i] = 1.f;
    }
    check_stereo(&p, f, 32767, 32767);
    av_frame_free(&f);
    cleanup(&p);
    /* Multichannel output must ignore the stereo-only preference. */
    PRIV multi = {0};
    f = signal_frame("5.1", AV_SAMPLE_FMT_FLTP, AV_CHAN_LOW_FREQUENCY, 8192);
    int16_t baseline[32*6];
    for (int enabled=0; enabled<2; enabled++) {
        ffmpeg_audio_set_downmix_lfe_boost(enabled);
        UCHAR *output; int channels, bits;
        int bytes = convert(&multi, f, &output, &channels, &bits);
        assert(bytes == sizeof(baseline) && channels == 6 && bits == 16);
        if (!enabled) memcpy(baseline, output, bytes);
        else assert(!memcmp(baseline, output, bytes));
        assert(multi.swr_lfe_boost == 0);
        checks++;
    }
    av_frame_free(&f);
    cleanup(&multi);
    puts("FFmpeg conversion: LFE toggle/reset, cached matrix, 5.1/7.1, eight PCM formats, center/surround, mono/stereo, saturation and multichannel checks passed.");
    printf("Passed %d checks.\n", checks);
    return 0;
}
