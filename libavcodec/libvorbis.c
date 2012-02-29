/*
 * copyright (c) 2002 Mark Hills <mark@pogo.org.uk>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Vorbis encoding support via libvorbisenc.
 * @author Mark Hills <mark@pogo.org.uk>
 */

#include <vorbis/vorbisenc.h>

#include "libavutil/opt.h"
#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"
#include "vorbis.h"

#undef NDEBUG
#include <assert.h>

/* Number of samples the user should send in each call.
 * This value is used because it is the LCD of all possible frame sizes, so
 * an output packet will always start at the same point as one of the input
 * packets.
 */
#define OGGVORBIS_FRAME_SIZE 64

#define BUFFER_SIZE (1024 * 64)

typedef struct OggVorbisContext {
    AVClass *av_class;                  /**< class for AVOptions            */
    vorbis_info vi;                     /**< vorbis_info used during init   */
    vorbis_dsp_state vd;                /**< DSP state used for analysis    */
    vorbis_block vb;                    /**< vorbis_block used for analysis */
    uint8_t buffer[BUFFER_SIZE];        /**< output packet buffer           */
    int buffer_index;                   /**< current buffer position        */
    int eof;                            /**< end-of-file flag               */
    int dsp_initialized;                /**< vd has been initialized        */
    vorbis_comment vc;                  /**< VorbisComment info             */
    ogg_packet op;                      /**< ogg packet                     */
    double iblock;                      /**< impulse block bias option      */
} OggVorbisContext;

static const AVOption options[] = {
    { "iblock", "Sets the impulse block bias", offsetof(OggVorbisContext, iblock), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, -15, 0, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { NULL }
};

static const AVCodecDefault defaults[] = {
    { "b",  "0" },
    { NULL },
};

static const AVClass class = { "libvorbis", av_default_item_name, options, LIBAVUTIL_VERSION_INT };


static int vorbis_error_to_averror(int ov_err)
{
    switch (ov_err) {
    case OV_EFAULT: return AVERROR_BUG;
    case OV_EINVAL: return AVERROR(EINVAL);
    case OV_EIMPL:  return AVERROR(EINVAL);
    default:        return AVERROR_UNKNOWN;
    }
}

static av_cold int oggvorbis_init_encoder(vorbis_info *vi,
                                          AVCodecContext *avctx)
{
    OggVorbisContext *s = avctx->priv_data;
    double cfreq;
    int ret;

    if (avctx->flags & CODEC_FLAG_QSCALE || !avctx->bit_rate) {
        /* variable bitrate
         * NOTE: we use the oggenc range of -1 to 10 for global_quality for
         *       user convenience, but libvorbis uses -0.1 to 1.0.
         */
        float q = avctx->global_quality / (float)FF_QP2LAMBDA;
        /* default to 3 if the user did not set quality or bitrate */
        if (!(avctx->flags & CODEC_FLAG_QSCALE))
            q = 3.0;
        if ((ret = vorbis_encode_setup_vbr(vi, avctx->channels,
                                           avctx->sample_rate,
                                           q / 10.0)))
            goto error;
    } else {
        int minrate = avctx->rc_min_rate > 0 ? avctx->rc_min_rate : -1;
        int maxrate = avctx->rc_max_rate > 0 ? avctx->rc_max_rate : -1;

        /* average bitrate */
        if ((ret = vorbis_encode_setup_managed(vi, avctx->channels,
                                               avctx->sample_rate, maxrate,
                                               avctx->bit_rate, minrate)))
            goto error;

        /* variable bitrate by estimate, disable slow rate management */
        if (minrate == -1 && maxrate == -1)
            if ((ret = vorbis_encode_ctl(vi, OV_ECTL_RATEMANAGE2_SET, NULL)))
                goto error;
    }

    /* cutoff frequency */
    if (avctx->cutoff > 0) {
        cfreq = avctx->cutoff / 1000.0;
        if ((ret = vorbis_encode_ctl(vi, OV_ECTL_LOWPASS_SET, &cfreq)))
            goto error;
    }

    /* impulse block bias */
    if (s->iblock) {
        if ((ret = vorbis_encode_ctl(vi, OV_ECTL_IBLOCK_SET, &s->iblock)))
            goto error;
    }

    if ((ret = vorbis_encode_setup_init(vi)))
        goto error;

    return 0;
error:
    return vorbis_error_to_averror(ret);
}

/* How many bytes are needed for a buffer of length 'l' */
static int xiph_len(int l)
{
    return 1 + l / 255 + l;
}

static av_cold int oggvorbis_encode_close(AVCodecContext *avctx)
{
    OggVorbisContext *s = avctx->priv_data;

    /* notify vorbisenc this is EOF */
    if (s->dsp_initialized)
        vorbis_analysis_wrote(&s->vd, 0);

    vorbis_block_clear(&s->vb);
    vorbis_dsp_clear(&s->vd);
    vorbis_info_clear(&s->vi);

    av_freep(&avctx->coded_frame);
    av_freep(&avctx->extradata);

    return 0;
}

static av_cold int oggvorbis_encode_init(AVCodecContext *avctx)
{
    OggVorbisContext *s = avctx->priv_data;
    ogg_packet header, header_comm, header_code;
    uint8_t *p;
    unsigned int offset;
    int ret;

    vorbis_info_init(&s->vi);
    if ((ret = oggvorbis_init_encoder(&s->vi, avctx))) {
        av_log(avctx, AV_LOG_ERROR, "oggvorbis_encode_init: init_encoder failed\n");
        goto error;
    }
    if ((ret = vorbis_analysis_init(&s->vd, &s->vi))) {
        ret = vorbis_error_to_averror(ret);
        goto error;
    }
    s->dsp_initialized = 1;
    if ((ret = vorbis_block_init(&s->vd, &s->vb))) {
        ret = vorbis_error_to_averror(ret);
        goto error;
    }

    vorbis_comment_init(&s->vc);
    vorbis_comment_add_tag(&s->vc, "encoder", LIBAVCODEC_IDENT);

    if ((ret = vorbis_analysis_headerout(&s->vd, &s->vc, &header, &header_comm,
                                         &header_code))) {
        ret = vorbis_error_to_averror(ret);
        goto error;
    }

    avctx->extradata_size = 1 + xiph_len(header.bytes)      +
                                xiph_len(header_comm.bytes) +
                                header_code.bytes;
    p = avctx->extradata = av_malloc(avctx->extradata_size +
                                     FF_INPUT_BUFFER_PADDING_SIZE);
    if (!p) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    p[0]    = 2;
    offset  = 1;
    offset += av_xiphlacing(&p[offset], header.bytes);
    offset += av_xiphlacing(&p[offset], header_comm.bytes);
    memcpy(&p[offset], header.packet, header.bytes);
    offset += header.bytes;
    memcpy(&p[offset], header_comm.packet, header_comm.bytes);
    offset += header_comm.bytes;
    memcpy(&p[offset], header_code.packet, header_code.bytes);
    offset += header_code.bytes;
    assert(offset == avctx->extradata_size);

    vorbis_comment_clear(&s->vc);

    avctx->frame_size = OGGVORBIS_FRAME_SIZE;

    avctx->coded_frame = avcodec_alloc_frame();
    if (!avctx->coded_frame) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    return 0;
error:
    oggvorbis_encode_close(avctx);
    return ret;
}

static int oggvorbis_encode_frame(AVCodecContext *avctx, unsigned char *packets,
                                  int buf_size, void *data)
{
    OggVorbisContext *s = avctx->priv_data;
    ogg_packet op;
    float *audio = data;
    int pkt_size, ret;

    /* send samples to libvorbis */
    if (data) {
        const int samples = avctx->frame_size;
        float **buffer;
        int c, channels = s->vi.channels;

        buffer = vorbis_analysis_buffer(&s->vd, samples);
        for (c = 0; c < channels; c++) {
            int i;
            int co = (channels > 8) ? c :
                     ff_vorbis_encoding_channel_layout_offsets[channels - 1][c];
            for (i = 0; i < samples; i++)
                buffer[c][i] = audio[i * channels + co];
        }
        if ((ret = vorbis_analysis_wrote(&s->vd, samples)) < 0)
            return vorbis_error_to_averror(ret);
    } else {
        if (!s->eof)
            if ((ret = vorbis_analysis_wrote(&s->vd, 0)) < 0)
                return vorbis_error_to_averror(ret);
        s->eof = 1;
    }

    /* retrieve available packets from libvorbis */
    while ((ret = vorbis_analysis_blockout(&s->vd, &s->vb)) == 1) {
        if ((ret = vorbis_analysis(&s->vb, NULL)) < 0)
            break;
        if ((ret = vorbis_bitrate_addblock(&s->vb)) < 0)
            break;

        /* add any available packets to the output packet buffer */
        while ((ret = vorbis_bitrate_flushpacket(&s->vd, &op)) == 1) {
            if (s->buffer_index + sizeof(ogg_packet) + op.bytes > BUFFER_SIZE) {
                av_log(avctx, AV_LOG_ERROR, "libvorbis: buffer overflow.");
                return -1;
            }
            memcpy(s->buffer + s->buffer_index, &op, sizeof(ogg_packet));
            s->buffer_index += sizeof(ogg_packet);
            memcpy(s->buffer + s->buffer_index, op.packet, op.bytes);
            s->buffer_index += op.bytes;
        }
        if (ret < 0)
            break;
    }
    if (ret < 0)
        return vorbis_error_to_averror(ret);

    /* output then next packet from the output buffer, if available */
    pkt_size = 0;
    if (s->buffer_index) {
        ogg_packet *op2 = (ogg_packet *)s->buffer;
        op2->packet     = s->buffer + sizeof(ogg_packet);

        pkt_size = op2->bytes;
        // FIXME: we should use the user-supplied pts and duration
        avctx->coded_frame->pts = ff_samples_to_time_base(avctx,
                                                          op2->granulepos);
        if (pkt_size > buf_size) {
            av_log(avctx, AV_LOG_ERROR, "libvorbis: buffer overflow.");
            return -1;
        }

        memcpy(packets, op2->packet, pkt_size);
        s->buffer_index -= pkt_size + sizeof(ogg_packet);
        memmove(s->buffer, s->buffer + pkt_size + sizeof(ogg_packet),
                s->buffer_index);
    }

    return pkt_size;
}

AVCodec ff_libvorbis_encoder = {
    .name           = "libvorbis",
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = CODEC_ID_VORBIS,
    .priv_data_size = sizeof(OggVorbisContext),
    .init           = oggvorbis_encode_init,
    .encode         = oggvorbis_encode_frame,
    .close          = oggvorbis_encode_close,
    .capabilities   = CODEC_CAP_DELAY,
    .sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_FLT,
                                                      AV_SAMPLE_FMT_NONE },
    .long_name      = NULL_IF_CONFIG_SMALL("libvorbis Vorbis"),
    .priv_class     = &class,
    .defaults       = defaults,
};
