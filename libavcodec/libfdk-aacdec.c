/*
 * AAC decoder wrapper
 * Copyright (c) 2012 Martin Storsjo
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

#include <fdk-aac/aacdecoder_lib.h>

#include "avcodec.h"
#include "libavutil/opt.h"
#include "internal.h"

typedef struct AACContext {
    const AVClass *class;
    AVFrame frame;
    HANDLE_AACDECODER handle;
    int initialized;
} AACContext;

static const AVOption aac_dec_options[] = {
    { NULL }
};

static const AVClass aac_dec_class = {
    "libaac", av_default_item_name, aac_dec_options, LIBAVUTIL_VERSION_INT
};

static int aac_get_stream_info(AVCodecContext *avctx)
{
    AACContext *s = avctx->priv_data;
    CStreamInfo *info = aacDecoder_GetStreamInfo(s->handle);

    if (!info) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get stream info\n");
        return AVERROR(EINVAL);
    }
    if (info->sampleRate <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Stream info not initialized\n");
        return AVERROR(EINVAL);
    }
    avctx->channels = info->numChannels;
    avctx->sample_rate = info->sampleRate;
    avctx->frame_size = info->frameSize;
    return 0;
}

static av_cold int aac_decode_close(AVCodecContext *avctx)
{
    AACContext *s = avctx->priv_data;
    if (s->handle)
        aacDecoder_Close(s->handle);
    return 0;
}

static av_cold int aac_decode_init(AVCodecContext *avctx)
{
    AACContext *s = avctx->priv_data;
    AAC_DECODER_ERROR err;
    int ret = AVERROR(EINVAL);

    s->handle = aacDecoder_Open(avctx->extradata_size ? TT_MP4_RAW : TT_MP4_ADTS, 1);

    if (avctx->extradata_size) {
        if ((err = aacDecoder_ConfigRaw(s->handle, &avctx->extradata, &avctx->extradata_size)) != AAC_DEC_OK) {
            av_log(avctx, AV_LOG_WARNING, "Unable to set extradata\n");
            goto error;
        }
    }

    avcodec_get_frame_defaults(&s->frame);
    avctx->coded_frame = &s->frame;

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;

    return 0;
error:
    return ret;
}

static int aac_decode_frame(AVCodecContext *avctx, void *data,
                             int *got_frame_ptr, AVPacket *avpkt)
{
    AACContext *s  = avctx->priv_data;
    int ret;
    AAC_DECODER_ERROR err;
    UINT valid = avpkt->size;
    uint8_t *buf, *tmpptr = NULL;
    int buf_size;

    err = aacDecoder_Fill(s->handle, &avpkt->data, &avpkt->size, &valid);
    if (err != AAC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Fill failed: %x\n", err);
        return AVERROR_INVALIDDATA;
    }

    if (avctx->channels) {
        s->frame.nb_samples = !s->initialized ? 2048 : avctx->frame_size;
        if ((ret = avctx->get_buffer(avctx, &s->frame)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
            return ret;
        }
        buf = s->frame.data[0];
        buf_size = 2*avctx->channels*s->frame.nb_samples;
    } else {
       buf_size = 50*1024;
       buf = tmpptr = av_malloc(buf_size);
       if (!buf)
           return AVERROR(ENOMEM);
    }

    err = aacDecoder_DecodeFrame(s->handle, (INT_PCM*) buf, buf_size, 0);
    if (err == AAC_DEC_NOT_ENOUGH_BITS) {
        av_free(tmpptr);
        return avpkt->size - valid;
    }
    if (err != AAC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Decode failed: %x\n", err);
        av_free(tmpptr);
        return AVERROR_INVALIDDATA;
    }

    if (!s->initialized) {
        if ((ret = aac_get_stream_info(avctx)) < 0) {
            av_free(tmpptr);
            return ret;
        }
        s->initialized = 1;
        s->frame.nb_samples = avctx->frame_size;
    }

    if (tmpptr) {
        s->frame.nb_samples = avctx->frame_size;
        if ((ret = avctx->get_buffer(avctx, &s->frame)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
            return ret;
        }
        memcpy(s->frame.data[0], tmpptr, 2*avctx->channels*avctx->frame_size);
    }

    *got_frame_ptr   = 1;
    *(AVFrame *)data = s->frame;
    av_free(tmpptr);
    return avpkt->size - valid;
}

AVCodec ff_libfdk_aac_decoder = {
    .name           = "libfdk_aac",
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = CODEC_ID_AAC,
    .priv_data_size = sizeof(AACContext),
    .init           = aac_decode_init,
    .decode         = aac_decode_frame,
    .close          = aac_decode_close,
    .capabilities   = CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("Fraunhofer FDK AAC"),
    .priv_class     = &aac_dec_class,
};
