// Copyright 2019 Joe Drago. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "avif/internal.h"

#include "rav1e.h"

#include <string.h>

struct avifCodecInternal
{
    avifCodecConfigurationBox config;
};

static void rav1eCodecDestroyInternal(avifCodec * codec)
{
    avifFree(codec->internal);
}

static avifBool rav1eCodecOpen(struct avifCodec * codec, uint32_t firstSampleIndex)
{
    (void)firstSampleIndex; // Codec is encode-only, this isn't used
    (void)codec;
    return AVIF_TRUE;
}

static avifBool rav1eCodecEncodeImage(avifCodec * codec, avifImage * image, avifEncoder * encoder, avifRWData * obu, avifBool alpha)
{
    avifBool success = AVIF_FALSE;

    RaConfig * rav1eConfig = NULL;
    RaContext * rav1eContext = NULL;
    RaFrame * rav1eFrame = NULL;

    int yShift = 0;
    RaChromaSampling chromaSampling;
    RaPixelRange rav1eRange;
    if (alpha) {
        rav1eRange = RA_PIXEL_RANGE_FULL;
        chromaSampling = RA_CHROMA_SAMPLING_CS400;
    } else {
        rav1eRange = (image->yuvRange == AVIF_RANGE_FULL) ? RA_PIXEL_RANGE_FULL : RA_PIXEL_RANGE_LIMITED;
        switch (image->yuvFormat) {
            case AVIF_PIXEL_FORMAT_YUV444:
                chromaSampling = RA_CHROMA_SAMPLING_CS444;
                break;
            case AVIF_PIXEL_FORMAT_YUV422:
                chromaSampling = RA_CHROMA_SAMPLING_CS422;
                break;
            case AVIF_PIXEL_FORMAT_YUV420:
                chromaSampling = RA_CHROMA_SAMPLING_CS420;
                yShift = 1;
                break;
            case AVIF_PIXEL_FORMAT_YV12:
                return AVIF_FALSE;
            default:
                return AVIF_FALSE;
        }
    }

    avifPixelFormatInfo formatInfo;
    avifGetPixelFormatInfo(image->yuvFormat, &formatInfo);

    rav1eConfig = rav1e_config_default();
    if (rav1e_config_set_pixel_format(
            rav1eConfig, (uint8_t)image->depth, chromaSampling, RA_CHROMA_SAMPLE_POSITION_UNKNOWN, rav1eRange) < 0) {
        goto cleanup;
    }

    if (rav1e_config_parse_int(rav1eConfig, "width", image->width) == -1) {
        goto cleanup;
    }
    if (rav1e_config_parse_int(rav1eConfig, "height", image->height) == -1) {
        goto cleanup;
    }
    if (rav1e_config_parse_int(rav1eConfig, "threads", encoder->maxThreads) == -1) {
        goto cleanup;
    }

    int minQuantizer = AVIF_CLAMP(encoder->minQuantizer, 0, 63);
    int maxQuantizer = AVIF_CLAMP(encoder->maxQuantizer, 0, 63);
    if (alpha) {
        minQuantizer = AVIF_QUANTIZER_LOSSLESS;
        maxQuantizer = AVIF_QUANTIZER_LOSSLESS;
    }
    minQuantizer = (minQuantizer * 255) / 63; // Rescale quantizer values as rav1e's QP range is [0,255]
    maxQuantizer = (maxQuantizer * 255) / 63;
    if (rav1e_config_parse_int(rav1eConfig, "min_quantizer", minQuantizer) == -1) {
        goto cleanup;
    }
    if (rav1e_config_parse_int(rav1eConfig, "quantizer", maxQuantizer) == -1) {
        goto cleanup;
    }

    // Profile 0.  8-bit and 10-bit 4:2:0 and 4:0:0 only.
    // Profile 1.  8-bit and 10-bit 4:4:4
    // Profile 2.  8-bit and 10-bit 4:2:2
    //            12-bit  4:0:0, 4:2:2 and 4:4:4
    uint8_t seqProfile = 0;
    if (image->depth == 12) {
        // Only seqProfile 2 can handle 12 bit
        seqProfile = 2;
    } else {
        // 8-bit or 10-bit

        if (alpha) {
            seqProfile = 0;
        } else {
            switch (image->yuvFormat) {
                case AVIF_PIXEL_FORMAT_YUV444:
                    seqProfile = 1;
                    break;
                case AVIF_PIXEL_FORMAT_YUV422:
                    seqProfile = 2;
                    break;
                case AVIF_PIXEL_FORMAT_YUV420:
                    seqProfile = 0;
                    break;
                case AVIF_PIXEL_FORMAT_YV12:
                    seqProfile = 0;
                    break;
                case AVIF_PIXEL_FORMAT_NONE:
                default:
                    break;
            }
        }
    }

    // TODO: Choose correct value from Annex A.3 table: https://aomediacodec.github.io/av1-spec/av1-spec.pdf
    uint8_t seqLevelIdx0 = 31;
    if ((image->width <= 8192) && (image->height <= 4352) && ((image->width * image->height) <= 8912896)) {
        // Image is 5.1 compatible
        seqLevelIdx0 = 13; // 5.1
    }

    memset(&codec->internal->config, 0, sizeof(avifCodecConfigurationBox));
    codec->internal->config.seqProfile = seqProfile;
    codec->internal->config.seqLevelIdx0 = seqLevelIdx0;
    codec->internal->config.seqTier0 = 0;
    codec->internal->config.highBitdepth = (image->depth > 8) ? 1 : 0;
    codec->internal->config.twelveBit = (image->depth == 12) ? 1 : 0;
    codec->internal->config.monochrome = alpha ? 1 : 0;
    codec->internal->config.chromaSubsamplingX = (uint8_t)formatInfo.chromaShiftX;
    codec->internal->config.chromaSubsamplingY = (uint8_t)formatInfo.chromaShiftY;

    if (encoder->tileRowsLog2 != 0) {
        int tileRowsLog2 = AVIF_CLAMP(encoder->tileRowsLog2, 0, 6);
        if (rav1e_config_parse_int(rav1eConfig, "tile_rows", 1 << tileRowsLog2) == -1) {
            goto cleanup;
        }
    }
    if (encoder->tileColsLog2 != 0) {
        int tileColsLog2 = AVIF_CLAMP(encoder->tileColsLog2, 0, 6);
        if (rav1e_config_parse_int(rav1eConfig, "tile_cols", 1 << tileColsLog2) == -1) {
            goto cleanup;
        }
    }

    if (image->profileFormat == AVIF_PROFILE_FORMAT_NCLX) {
        rav1e_config_set_color_description(rav1eConfig,
                                           (RaMatrixCoefficients)image->nclx.matrixCoefficients,
                                           (RaColorPrimaries)image->nclx.colourPrimaries,
                                           (RaTransferCharacteristics)image->nclx.transferCharacteristics);
    }

    rav1eContext = rav1e_context_new(rav1eConfig);
    if (!rav1eContext) {
        goto cleanup;
    }
    rav1eFrame = rav1e_frame_new(rav1eContext);

    int byteWidth = (image->depth > 8) ? 2 : 1;
    if (alpha) {
        rav1e_frame_fill_plane(rav1eFrame, 0, image->alphaPlane, image->alphaRowBytes * image->height, image->alphaRowBytes, byteWidth);
    } else {
        uint32_t uvHeight = image->height >> yShift;
        if (uvHeight < 1) {
            uvHeight = 1;
        }
        rav1e_frame_fill_plane(rav1eFrame, 0, image->yuvPlanes[0], image->yuvRowBytes[0] * image->height, image->yuvRowBytes[0], byteWidth);
        rav1e_frame_fill_plane(rav1eFrame, 1, image->yuvPlanes[1], image->yuvRowBytes[1] * uvHeight, image->yuvRowBytes[1], byteWidth);
        rav1e_frame_fill_plane(rav1eFrame, 2, image->yuvPlanes[2], image->yuvRowBytes[2] * uvHeight, image->yuvRowBytes[2], byteWidth);
    }

    RaEncoderStatus encoderStatus = rav1e_send_frame(rav1eContext, rav1eFrame);
    if (encoderStatus != 0) {
        goto cleanup;
    }
    encoderStatus = rav1e_send_frame(rav1eContext, NULL); // flush
    if (encoderStatus != 0) {
        goto cleanup;
    }

    RaPacket * pkt = NULL;
    encoderStatus = rav1e_receive_packet(rav1eContext, &pkt);
    if (encoderStatus != 0) {
        goto cleanup;
    }

    if (pkt && pkt->data && (pkt->len > 0)) {
        avifRWDataSet(obu, pkt->data, pkt->len);
        success = AVIF_TRUE;
    }
cleanup:
    if (rav1eFrame) {
        rav1e_frame_unref(rav1eFrame);
        rav1eFrame = NULL;
    }
    if (rav1eContext) {
        rav1e_context_unref(rav1eContext);
        rav1eContext = NULL;
    }
    if (rav1eConfig) {
        rav1e_config_unref(rav1eConfig);
        rav1eConfig = NULL;
    }
    return success;
}

static void rav1eCodecGetConfigurationBox(avifCodec * codec, avifCodecConfigurationBox * outConfig)
{
    memcpy(outConfig, &codec->internal->config, sizeof(avifCodecConfigurationBox));
}

const char * avifCodecVersionRav1e(void)
{
    return "0"; // https://github.com/xiph/rav1e/issues/1801
}

avifCodec * avifCodecCreateRav1e(void)
{
    avifCodec * codec = (avifCodec *)avifAlloc(sizeof(avifCodec));
    memset(codec, 0, sizeof(struct avifCodec));
    codec->open = rav1eCodecOpen;
    codec->encodeImage = rav1eCodecEncodeImage;
    codec->getConfigurationBox = rav1eCodecGetConfigurationBox;
    codec->destroyInternal = rav1eCodecDestroyInternal;

    codec->internal = (struct avifCodecInternal *)avifAlloc(sizeof(struct avifCodecInternal));
    memset(codec->internal, 0, sizeof(struct avifCodecInternal));
    return codec;
}