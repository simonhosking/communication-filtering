/*
 * Copyright (C) Commonwealth of Australia 2016
 *
 * This computer program is the property of the Australian Government;
 * it is released for defence purposes only and must not be disseminated
 * beyond the stated distribution without prior approval.
 *
 * Point of contact:
 * Peter Ross; <peter.ross@dsto.defence.gov.au>; +61 2 6144 9190.
 *
 */

#include <libsimutil/simutil.h>
#include <inttypes.h>
#include <stdio.h>
#include <errno.h>

#include "kernel.h"
#include <libsimutil/dis.h>
#include <libsimutil/scenario.h>
#include <libsimutil/timeval.h>
#include <libsimutil/snd.h>
#include <libsimutil/time.h>
#include <libsimutil/option.h>
#include <libsimutil/DlEntityId2.h>
#include <libsimutil/ringbuffer.h>
#include <libsimutil/signal.h>
#if HAVE_LIBSPEEXDSP
#include <speex/speex_resampler.h>
#endif
#if HAVE_WBVAD
#include "wb_vad.h"
#endif

#define SIGNAL_BUFFER   11520 /* maximum number of continguous samples we can manipulate */
#define VAD_SAMPLE_RATE 16000 /* AMR-WB sample rate (required for VAD) */

#define SEP ","   /* output column separator */

typedef struct {
    SimScenario scenario;
    int format_time;
    int hyperlink;
    char *write_prefix;
    int write_network;
    int write_decoded;
    int write_utterance;
#if HAVE_WBVAD
    int vad;
#endif
    float ptt_duration_threshold;
#if HAVE_WBVAD
    float utt_duration_threshold;
    float vad_threshold;
#endif
    int exclude;
    short out2[SIGNAL_BUFFER], autmp[SIGNAL_BUFFER];
} Context;

typedef struct {
    int last_state;
    struct timeval tv_ptt_on;
    char filename[2048], filename2[2048], filename3[2048];
    FILE *f, *f2, *f3;
    int encoding_type;
    int sample_rate;
    unsigned int samples;

    SimSignalDecoder *dec;

#if HAVE_LIBSPEEXDSP
    SpeexResamplerState *st;                    /** audio resampler */
#endif

    short buffer[65536];
    RingBuffer rbuf;

#if HAVE_WBVAD
    VadVars *vadstate;
    int content;
    int total;

    /* utterance tracking */
    struct timeval tv_utt_start;
    unsigned int utt_samples;
#endif
} DevicePrivate;


/**
 * write sun au header
 * @return -1 on error
 */
static int write_au_header(FILE *f, int encoding_type, int sample_rate) {
    snd_header_t hdr = {
        .magic = betoh32(SND_MAGIC),
        .hdr_size = betoh32(sizeof(snd_header_t)),
        .data_size = betoh32(0xFFFFFFFF),
        .sample_rate = betoh32(sample_rate),
        .channels = betoh32(1),
    };

    switch(encoding_type) {
    case DIS_AUDIO_MULAW:
        hdr.encoding = betoh32(SND_FORMAT_MULAW_8);
        break;
    case DIS_AUDIO_PCM_S16BE:
        hdr.encoding = betoh32(SND_FORMAT_LINEAR_16);
        break;
    default:
        fprintf(stderr, "warning: unsupported encoding_type %d\n", encoding_type);
        return -1;
    }

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;

    return 0;
}

static FILE * open_au_file(char * filename, size_t filename_sz, const Context * c, const SimDevice *device, const char *name_prefix, int encoding_type, int sample_rate)
{
    const DevicePrivate *u = device->user_data;
    FILE *f;
    snprintf(filename, filename_sz - 1,
        "%s%s_%"PRIu64"_%d_%d_%d_%d_%.03fs.au",
        c->write_prefix,
        name_prefix,
        device->transmitter.freq,
        device->id.m_site, device->id.m_host, device->id.m_num, device->id.m_device,
        timeval_to_double(u->tv_ptt_on));

    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "warning: fopen('%s') failed\n", filename);
    } else if (write_au_header(f, encoding_type, sample_rate) < 0) {
        fprintf(stderr, "warning: write_au_header failed\n");
        fclose(f);
    }

    return f;
}

static void print_time(const struct timeval *tv, int format) {
    if (format) {
        struct tm tm;
        char tmp[1024];
        gmtime_r(&tv->tv_sec, &tm);
        strftime(tmp, sizeof(tmp)-1, "%H:%M:%S", &tm);
        printf("%s.%03d" SEP, tmp, (int)(tv->tv_usec / 1000));
	//printf("(%d %d)[%"PRIx64":%"PRIx64"]  ", sizeof(tv->tv_sec), sizeof(tv->tv_usec), (uint64_t)tv->tv_sec, (uint64_t)tv->tv_usec);
    } else {
        printf("%.03f" SEP, timeval_to_float(*tv));
    }
}

static void print_result(const SimScenario *s, const SimDevice *device, float duration_threshold, const char *filename, const struct timeval *start_time, const struct timeval *stop_time, uint64_t samples, int sample_rate, float vad_score, int truncated)
{
    const Context * c = s->user_data;
    int has_content = 1;
    struct timeval diff;
    timeval_subtract(stop_time, start_time, &diff);
    float ptt_duration = timeval_to_float(diff);

    if (!(ptt_duration > duration_threshold))
        has_content = 0;

#if HAVE_WBVAD
    if (c->vad && !(vad_score > c->vad_threshold))
        has_content = 0;
#endif

    if (!c->exclude || has_content) {

        /* dis id, frequency */
        printf("\"%s\"" SEP
               "\"%d:%d:%d\"" SEP
               "%d" SEP
               "\"%s\"" SEP
               "%"PRIu64 SEP,
            SIM_SAFE_CSTR0(ebv_find_entity_id(&g_ebv, ENUM_SAE, device->id.m_site, device->id.m_host, device->id.m_num)),
            device->id.m_site, device->id.m_host, device->id.m_num,
            device->id.m_device,
            SIM_SAFE_CSTR0(ebv_find_enum_value(&g_ebv, ENUM_FREQUENCY, device->transmitter.freq)),
            device->transmitter.freq
        );

        /* ptt on/off */
        print_time(start_time, c->format_time);
        print_time(&device->transmitter.timestamp, c->format_time);

        /* sample duration */
        printf(
               "%.03f" SEP
               "%.03f" SEP
               "%.03f" SEP,
            ptt_duration,
            samples ? (double)samples / sample_rate : 0.0,
            vad_score
        );

        /* filename */
        if (filename) {
            if (c->hyperlink)
                printf("=HYPERLINK(\"");
            printf("%s", filename);
            if (c->hyperlink)
                printf("\")");
        }

        printf(
            SEP
            "%d" SEP
            "%d" "\n",
            truncated,
            has_content);
    }
}

#if HAVE_WBVAD
static void close_utterance(SimScenario *s, SimDevice *device, const struct timeval *stop_time, int truncated)
{
    const Context * c = s->user_data;
    DevicePrivate *u = device->user_data;
    print_result(s, device, c->utt_duration_threshold, u->f3 ? u->filename3 : NULL, &u->tv_utt_start, stop_time, u->utt_samples, VAD_SAMPLE_RATE, 1.0, truncated);
    if (u->f3) {
        fclose(u->f3);
        u->f3 = NULL;
    };
    u->utt_samples = 0;
    timeval_zero(&u->tv_utt_start);
}
#endif

static void close_file(SimScenario *s, SimDevice *device, const struct timeval *stop_time, int truncated)
{
    Context * c = s->user_data;
    DevicePrivate *u = device->user_data;
    float vad_score =
#if HAVE_WBVAD
        u->total ? u->content / (float)u->total :
#endif
        0.0;

    print_result(s, device, c->ptt_duration_threshold, u->f ? u->filename : NULL, &u->tv_ptt_on, stop_time, u->samples, u->sample_rate, vad_score, truncated);

    if (u->f) {
        fclose(u->f);
        u->f = NULL;
    }

    if (u->f2) {
        fclose(u->f2);
        u->f2 = NULL;
    }

#if HAVE_WBVAD
    if (timeval_isnonzero(&u->tv_utt_start))
        close_utterance(s, device, stop_time, truncated);
#endif

#if HAVE_LIBSPEEXDSP
    if (u->st)
        speex_resampler_reset_mem(u->st);
#endif
}

static void process_signal_pdu(SimScenario *s, const sig_pdu_t *p, size_t buf_size, const struct timeval *tv)
{
    Context * c = s->user_data;
    SimDevice key, *device;
    DevicePrivate *u;

    if (buf_size < sizeof(sig_pdu_t))
        return;

    int encoding_class = (betoh16(p->encoding_scheme) & DIS_ENCODING_CLASS_MASK) >> DIS_ENCODING_CLASS_SHIFT;
    int encoding_type = (betoh16(p->encoding_scheme) & DIS_ENCODING_TYPE_MASK) >> DIS_ENCODING_TYPE_SHIFT;
    int sample_rate = betoh32(p->sample_rate);
    if (encoding_class != DIS_ENCODING_CLASS_AUDIO)
        return;

    SimEntityId2_read_dis(&key.id, &p->id, betoh16(p->radio_id));
    device = av_tree_find(s->devices, &key, SimRadio_compare, NULL);
    if (!device)
        return;

    if (device->transmitter.state != DIS_TX_STATE_ON) {
        fprintf(stderr, "warning: signal pdu's corresponding trasmitter state not(on). ignoring.\n");
        return;
    }

    u = device->user_data;
    const uint8_t * data;
    unsigned int samples;

    if (!u->dec) {
        u->encoding_type = encoding_type;
        u->sample_rate = sample_rate;

        u->dec = sim_sigdec_create();
        if (!u->dec) {
            fprintf(stderr, "error: sim_sigdec_create failed\n");
            return;
        }

#if HAVE_WBVAD
        if (c->vad) {
            if (rb_init(&u->rbuf, u->buffer, sizeof(u->buffer)/sizeof(short), sizeof(short)) < 0) {
                fprintf(stderr, "error: rb_init failed\n");
                sim_sigdec_destroy(u->dec);
                u->dec = NULL;
                return;
            }
            wb_vad_init(&u->vadstate);
        }
#endif
    }

    if (c->write_network && (encoding_type != u->encoding_type || sample_rate != u->sample_rate))
        fprintf(stderr, "warning: encoding type or sample rate changed. expect to hear garbage in network audio file.\n");

    int  data_size = MIN(betoh16(p->data_length) / 8, buf_size - sizeof(sig_pdu_t));
    data = (const uint8_t *)(p + 1);
    samples = betoh16(p->samples);
    u->samples += samples;

    /* write network audio */

    if (c->write_network && !u->f)
        u->f = open_au_file(u->filename, sizeof(u->filename), c, device, "network", encoding_type, sample_rate);

    if (u->f)
        if (fwrite(data, data_size, 1, u->f) != 1)
            fprintf(stderr, "warning: fwrite network audio failed\n");

    /* open decoded audio file */

    if (c->write_decoded && !u->f2)
        u->f2 = open_au_file(u->filename2, sizeof(u->filename2), c, device, "pcm_s16", DIS_AUDIO_PCM_S16BE, VAD_SAMPLE_RATE);

    /* decode audio */

    int16_t *out2;
    int out2_length;
    if (u->f2
#if HAVE_WBVAD
        || c->vad
#endif
        ) {
            int16_t out[SIGNAL_BUFFER];
            int out_length = sim_sigdec_decode(u->dec, encoding_type, 0, out, MIN(samples, SIGNAL_BUFFER), data, data_size, sample_rate);
            if (out_length != (int)samples)
                fprintf(stderr, "warning: number of samples decoded (%d) does not match samples reported in pdu (%d)\n", out_length, samples);
            if (out_length <= 0)
                return;

#if HAVE_LIBSPEEXDSP
            if (!u->st) {
                if (sample_rate != VAD_SAMPLE_RATE) {
                    u->st = speex_resampler_init(1, sample_rate, VAD_SAMPLE_RATE, 7 /* resampler_quality */, NULL);
                }
            } else {
                speex_resampler_set_rate(u->st, sample_rate, VAD_SAMPLE_RATE);
            }

            if (u->st) {
                spx_uint32_t spx_in_length = out_length;
                spx_uint32_t spx_out_length = SIGNAL_BUFFER;
                out2 = c->out2;
                speex_resampler_process_int(u->st, 0, out, &spx_in_length, out2, &spx_out_length);
                out2_length = spx_out_length;
            } else
#endif
            {
                out2 = out;
                out2_length = samples;
            }
    }

    /* write decoded audio */

    if (u->f2) {
        /* convert to big-endian, and write to 'decompressed' file */
        be16_to_m16_array(c->autmp, out2, out2_length);
        if (fwrite(c->autmp, out2_length*sizeof(int16_t), 1, u->f2) != 1)
            fprintf(stderr, "warning: fwrite decoded audio failed\n");
    }

    /* perform vad */

#if HAVE_WBVAD
    if (c->vad) {
        rb_write(&u->rbuf, out2, out2_length);

        while (rb_read_available(&u->rbuf) >= FRAME_LEN) {
            short *data1, *data2;
            int size1, size2;
            float frame[FRAME_LEN];
            int i, result;
            rb_read_regions2(&u->rbuf, FRAME_LEN, (void**)(void*)&data1, &size1, (void**)(void*)&data2, &size2);

            /* convert to 'floating point' format wb_vad */
            for (i = 0; i < size1; i++)
                frame[i] = data1[i];
            for (i = 0; i < size2; i++)
                frame[size1 + i] = data2[i];

            result = wb_vad(u->vadstate, frame);
            if (result)
                u->content++;
            u->total++;

            if (c->write_utterance && device->transmitter.freq >= DIS_SIMPLE_INTERCOM_FREQUENCY_MIN && device->transmitter.freq <= DIS_SIMPLE_INTERCOM_FREQUENCY_MAX) {
                if (result) {
                    if (!timeval_isnonzero(&u->tv_utt_start)) {
                        u->tv_utt_start = *tv;
                        u->f3 = open_au_file(u->filename3, sizeof(u->filename3), c, device, "utterance", DIS_AUDIO_PCM_S16BE, VAD_SAMPLE_RATE);
                    }
                    if (u->f3) {
                        be16_to_m16_array(c->autmp, data1, size1);
                        if (size2)
                            be16_to_m16_array(c->autmp + size1, data2, size2);
                        if (fwrite(c->autmp, (size1 + size2)*sizeof(int16_t), 1, u->f3) != 1)
                            fprintf(stderr, "warning: fwrite decoded audio failed\n");
                    }
                    u->utt_samples += size1 + size2;
                } else if (!result && timeval_isnonzero(&u->tv_utt_start)) {
                    close_utterance(s, device, tv, 0);
                }
            }

            rb_read_advance(&u->rbuf, size1 + size2);
        }
    }
#endif
    tv = tv; /* remove unused compiler warning */
}

static void device_update_cb(SimScenario *s, SimDevice *device)
{
    DevicePrivate *u = device->user_data;
    if (u->last_state != DIS_TX_STATE_ON && device->transmitter.state == DIS_TX_STATE_ON) { /* off-> on */
        u->tv_ptt_on = device->transmitter.timestamp;
        // reset counters
        u->samples = 0;
#if HAVE_WBVAD
        u->content = u->total = 0;
        u->utt_samples = 0;
        timeval_zero(&u->tv_utt_start);
#endif
    } if (u->last_state == DIS_TX_STATE_ON && device->transmitter.state != DIS_TX_STATE_ON) { /* on->off */
        close_file(s, device, &device->transmitter.timestamp, 0);
    }
    u->last_state = device->transmitter.state;
}

static void device_destroy_cb(SimScenario *s, SimDevice *device)
{
    DevicePrivate *u = device->user_data;
    if (u->last_state == DIS_TX_STATE_ON)
         close_file(s, device, &device->transmitter.timestamp /* FIXME: want actual expirartion time */, 1);
    if (u->dec)
        sim_sigdec_destroy(u->dec);
#if HAVE_LIBSPEEXDSP
    if (u->st)
        speex_resampler_destroy(u->st);
#endif
#if HAVE_WBVAD
    wb_vad_exit(&u->vadstate);
#endif
}

static int sndout_open(lcontext_t *c, int argc, const char **argv)
{
    Context * s = c->priv_data;
    const option_t opts[] = {
        { .name="h",         OPT_HELP,   {"usage: sndout [options]"}, "show help" },
        { .name="format-time",       OPT_INT,    {&s->format_time}, "format time as HH:MM:SS.ms" },
        { .name="hyperlink",         OPT_INT,    {&s->hyperlink}, "output filenames with hyperlink" },
        { .name="write-prefix",      OPT_STRDUP, {&s->write_prefix}, "output filename prefix" },
        { .name="write-network",  OPT_INT,    {&s->write_network}, "write network audio bitstream (.au)" },
        { .name="write-decoded",  OPT_INT,    {&s->write_decoded}, "write decoded audio bitsteam (.au)" },
#if HAVE_WBVAD
        { .name="write-utterance",  OPT_INT,    {&s->write_utterance}, "write utterance audio bitsteam (.au)" },
        { .name="vad",               OPT_INT,    {&s->vad}, "perform voice activity detection" },
#endif
        { .name="duration-threshold", OPT_FLOAT,   {&s->ptt_duration_threshold}, "ptt duration threshold (seconds)" },
#if HAVE_WBVAD
        { .name="utterance-duration-threshold", OPT_FLOAT,   {&s->utt_duration_threshold}, "utterance duration threshold (seconds)" },
        { .name="vad-threshold",      OPT_FLOAT,   {&s->vad_threshold}, "vad score threshold" },
#endif
        { .name="exclude",            OPT_INT,   {&s->exclude}, "exclude events that have no meaningful content from CSV output" },
        { NULL, }
    };

    s->format_time      = 1;
    s->hyperlink        = 1;
    s->write_prefix     = strdup("");
    s->write_network    = 1;
    s->write_decoded    = 0;
    s->write_utterance  = 1;
#if HAVE_WBVAD
    s->vad              = 1;
    s->vad_threshold    = 0.0f;
#endif
    s->ptt_duration_threshold = 0.1; /* 100ms */
#if HAVE_WBVAD
    s->utt_duration_threshold = 0.1; /* 100ms */
#endif
    s->exclude          = 0;

    if (parse_options(argc, argv, opts, 0, 1, s) < 0)
        return -1;

    if (!HAVE_LIBSPEEXDSP && (s->write_decoded
#if HAVE_WBVAD
        || s->vad
#endif
        ))
        fprintf(stderr, "warning: no dsp; write-decoded files and vad will be incorrect\n");

    scenario_init(&s->scenario);
    s->scenario.user_data = s;
    s->scenario.device_user_data_size = sizeof(DevicePrivate);
    s->scenario.device_update_callback = device_update_cb;
    s->scenario.device_destroy_callback = device_destroy_cb;


    sim_signal_init();

    printf(
        "DIS-ID-NAME" SEP
        "DIS-ID" SEP
        "RADIO-ID" SEP
        "FREQUENCY-NAME" SEP
        "FREQUENCY" SEP
        "PTT-ON" SEP
        "PTT-OFF" SEP
        "PTT-DURATION" SEP
        "DAC-SAMPLES" SEP
        "VAD" SEP
        "FILENAME" SEP
        "TRUNCATED?" SEP
        "CONTENT?"
        "\n");
    return 0;
}

static int sndout_close(lcontext_t *c)
{
    Context *s = c->priv_data;
    scenario_free(&s->scenario);
    sim_free(s->write_prefix);
    return 0;
}

static int sndout_process(lcontext_t *c, void *buf, int buf_len, lmeta_t *meta)
{
    if ((size_t)buf_len < sizeof(dis_hdr_t))
        return buf_len;

    Context *s = c->priv_data;
    const dis_hdr_t * hdr = buf;
    if (hdr->type == DIS_PDU_SIGNAL) {
        process_signal_pdu(&s->scenario, buf, buf_len, &SIM_TIMEVAL(meta->time));
    } else if (hdr->type == DIS_PDU_TX) {
        scenario_process_dis_pdu(&s->scenario, buf, buf_len, &SIM_TIMEVAL(meta->time));
    }
    return buf_len;
}

const lmodule_t sndout_output = {
    .name           = "sndout",
    .desc           = "output dis radio events; comprehensive summary (csv) and event audio files (au)",
    .flags          = LFLAG_OUTPUT|LFLAG_ARGV|LFLAG_ENUM,
    .priv_data_size = sizeof(Context),
    .u.open_argv    = sndout_open,
    .process        = sndout_process,
    .close          = sndout_close,
};
