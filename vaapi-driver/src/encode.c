/* VA-API encode dispatch for the Qualcomm V4L2 M2M encoder. */
#include <string.h>
#include <stdlib.h>

#include "driver.h"

#define DMD_ENC_FRAME_INTERVAL_US 33333ULL
static int dmd_encode_store_output_locked(struct dmd_driver *drv,
                                          struct dmd_context *c,
                                          uint64_t pts_us,
                                          const void *data, size_t size)
{
    for (int i = 0; i < DMD_MAX_SURFACES; i++) {
        struct dmd_encode_pending *pending = &c->encode_pending[i];
        if (!pending->in_use || pending->pts_us != pts_us) continue;
        struct dmd_buffer *coded = dmd_find_buffer_locked(drv,
                                                           pending->coded_id);
        if (!coded || coded->type != VAEncCodedBufferType)
            return -1;
        if (size > coded->size) return -1;
        memcpy(coded->data, data, size);
        coded->coded_segment.size = (uint32_t)size;
        coded->coded_segment.bit_offset = 0;
        coded->coded_segment.status = 0;
        coded->coded_segment.reserved = 0;
        coded->coded_segment.buf = coded->data;
        coded->coded_segment.next = NULL;
        coded->coded_ready = 1;
        pending->in_use = 0;
        return 0;
    }
    return -1;
}

struct dmd_encode_drain_context {
    struct dmd_driver *drv;
    VAContextID context;
};

static int dmd_encode_store_drain_output(void *opaque, uint64_t pts_us,
                                         const void *data, size_t size)
{
    struct dmd_encode_drain_context *drain = opaque;
    struct dmd_driver *drv = drain->drv;
    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, drain->context);
    int rc = c ? dmd_encode_store_output_locked(drv, c, pts_us, data, size)
               : -1;
    pthread_mutex_unlock(&drv->lock);
    return rc;
}

VAStatus dmd_encode_begin(VADriverContextP ctx, VAContextID context,
                          VASurfaceID render_target)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv) return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    struct dmd_surface *s = dmd_find_surface_locked(drv, render_target);
    if (!c || c->entrypoint != VAEntrypointEncSlice) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!s || s->format != VA_RT_FORMAT_YUV420 || !s->data) {
        dmd_log("encode BeginPicture: invalid surface id=%u found=%d format=0x%x data=%p\n",
                (unsigned)render_target, s != NULL,
                s ? s->format : 0, s ? (void *)s->data : NULL);
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->width > c->picture_width || s->height > c->picture_height ||
        s->buf_width < dmd_align_up(c->picture_width, DMD_WIDTH_ALIGN) ||
        s->buf_height < dmd_align_up(c->picture_height, DMD_HEIGHT_ALIGN)) {
        dmd_log("encode BeginPicture: size mismatch id=%u surface=%ux%u context=%ux%u\n",
                (unsigned)render_target, s->width, s->height,
                c->picture_width, c->picture_height);
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->state == DMD_SURFACE_PENDING) {
        pthread_mutex_unlock(&drv->lock);
        VAStatus ready = dmd_surface_wait(drv, render_target);
        if (ready != VA_STATUS_SUCCESS)
            return ready;
        pthread_mutex_lock(&drv->lock);
        c = dmd_find_context_locked(drv, context);
        s = dmd_find_surface_locked(drv, render_target);
        if (!c || c->entrypoint != VAEntrypointEncSlice || !s ||
            s->format != VA_RT_FORMAT_YUV420 || !s->data ||
            s->width > c->picture_width || s->height > c->picture_height ||
            s->buf_width < dmd_align_up(c->picture_width, DMD_WIDTH_ALIGN) ||
            s->buf_height < dmd_align_up(c->picture_height, DMD_HEIGHT_ALIGN)) {
            dmd_log("encode BeginPicture: waited surface invalid id=%u found=%d size=%ux%u context=%ux%u format=0x%x data=%p\n",
                    (unsigned)render_target, s != NULL,
                    s ? s->width : 0, s ? s->height : 0,
                    c ? c->picture_width : 0, c ? c->picture_height : 0,
                    s ? s->format : 0, s ? (void *)s->data : NULL);
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }
    if (c->current_target != VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    c->current_target = render_target;
    c->have_enc_pic = 0;
    pthread_mutex_unlock(&drv->lock);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_encode_render(VADriverContextP ctx, VAContextID context,
                           VABufferID *buffers, int num_buffers)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv || (num_buffers && !buffers) || num_buffers < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c || c->entrypoint != VAEntrypointEncSlice) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (c->current_target == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    VAStatus status = VA_STATUS_SUCCESS;
    for (int i = 0; i < num_buffers; i++) {
        struct dmd_buffer *b = dmd_find_buffer_locked(drv, buffers[i]);
        if (!b || b->context != context) {
            status = VA_STATUS_ERROR_INVALID_BUFFER;
            break;
        }
        if (c->codec == DMD_CODEC_H264) {
            if (b->type == VAEncSequenceParameterBufferType &&
                b->size >= sizeof(c->enc_h264_seq)) {
                memcpy(&c->enc_h264_seq, b->data, sizeof(c->enc_h264_seq));
                c->have_enc_seq = 1;
            } else if (b->type == VAEncPictureParameterBufferType &&
                       b->size >= sizeof(c->enc_h264_pic)) {
                memcpy(&c->enc_h264_pic, b->data, sizeof(c->enc_h264_pic));
                c->have_enc_pic = 1;
                if ((c->enc_rate_control & VA_RC_CQP) &&
                    c->enc_h264_pic.pic_init_qp >= 1 &&
                    c->enc_h264_pic.pic_init_qp <= 51)
                    c->enc_qp = c->enc_h264_pic.pic_init_qp;
            }
        } else if (c->codec == DMD_CODEC_HEVC) {
            if (b->type == VAEncSequenceParameterBufferType &&
                b->size >= sizeof(c->enc_hevc_seq)) {
                memcpy(&c->enc_hevc_seq, b->data, sizeof(c->enc_hevc_seq));
                c->have_enc_seq = 1;
            } else if (b->type == VAEncPictureParameterBufferType &&
                       b->size >= sizeof(c->enc_hevc_pic)) {
                memcpy(&c->enc_hevc_pic, b->data, sizeof(c->enc_hevc_pic));
                c->have_enc_pic = 1;
                if ((c->enc_rate_control & VA_RC_CQP) &&
                    c->enc_hevc_pic.pic_init_qp >= 1 &&
                    c->enc_hevc_pic.pic_init_qp <= 51)
                    c->enc_qp = c->enc_hevc_pic.pic_init_qp;
            }
        }
        if (b->type == VAEncMiscParameterBufferType &&
            b->size >= sizeof(VAEncMiscParameterBuffer) +
                       sizeof(VAEncMiscParameterRateControl)) {
            const VAEncMiscParameterBuffer *misc = b->data;
            if (misc->type == VAEncMiscParameterTypeRateControl) {
                const VAEncMiscParameterRateControl *rc =
                    (const VAEncMiscParameterRateControl *)misc->data;
                unsigned qp = rc->initial_qp ? rc->initial_qp
                            : rc->quality_factor ? rc->quality_factor
                            : rc->ICQ_quality_factor;
                if (qp >= 1 && qp <= 51)
                    c->enc_qp = qp;
            }
        }
    }
    pthread_mutex_unlock(&drv->lock);
    return status;
}

VAStatus dmd_encode_end(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv) return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c || c->entrypoint != VAEntrypointEncSlice) {
        dmd_log("encode EndPicture: invalid context %u\n", (unsigned)context);
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (c->current_target == VA_INVALID_ID || !c->have_enc_pic ||
        !c->have_enc_seq) {
        dmd_log("encode EndPicture: incomplete picture target=%u have_pic=%d have_seq=%d\n",
                (unsigned)c->current_target, c->have_enc_pic, c->have_enc_seq);
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    VABufferID coded_id = c->codec == DMD_CODEC_H264
        ? c->enc_h264_pic.coded_buf : c->enc_hevc_pic.coded_buf;
    struct dmd_buffer *coded = dmd_find_buffer_locked(drv, coded_id);
    struct dmd_surface *surface = dmd_find_surface_locked(drv, c->current_target);
    if (!coded || coded->type != VAEncCodedBufferType || !surface) {
        dmd_log("encode EndPicture: coded buffer/surface missing coded=%d type=%d surface=%d\n",
                coded != NULL, coded ? (int)coded->type : -1,
                surface != NULL);
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    struct dmd_v4l2_enc *enc = c->encoder;
    int context_index = (int)(c - drv->contexts);
    while (drv->io_busy[context_index])
        pthread_cond_wait(&drv->io_done, &drv->lock);
    drv->io_busy[context_index] = 1;
    unsigned bitrate = 0, gop = 0;
    unsigned visible_width = c->picture_width;
    unsigned visible_height = c->picture_height;
    uint32_t rate_control = c->enc_rate_control;
    unsigned int qp = c->enc_qp;
    int force_key = 0;
    if (c->codec == DMD_CODEC_H264) {
        bitrate = c->enc_h264_seq.bits_per_second;
        gop = c->enc_h264_seq.intra_period;
        if (c->enc_h264_seq.frame_cropping_flag) {
            unsigned coded_width = c->enc_h264_seq.picture_width_in_mbs * 16u;
            unsigned coded_height = c->enc_h264_seq.picture_height_in_mbs *
                16u * (c->enc_h264_seq.seq_fields.bits.frame_mbs_only_flag ? 1u : 2u);
            unsigned crop_unit_x = 2u, crop_unit_y =
                c->enc_h264_seq.seq_fields.bits.frame_mbs_only_flag ? 2u : 4u;
            unsigned crop_x = (c->enc_h264_seq.frame_crop_left_offset +
                               c->enc_h264_seq.frame_crop_right_offset) * crop_unit_x;
            unsigned crop_y = (c->enc_h264_seq.frame_crop_top_offset +
                               c->enc_h264_seq.frame_crop_bottom_offset) * crop_unit_y;
            if (coded_width > crop_x && coded_height > crop_y) {
                visible_width = coded_width - crop_x;
                visible_height = coded_height - crop_y;
            }
        }
        force_key = c->enc_h264_pic.pic_fields.bits.idr_pic_flag ||
                    c->enc_h264_pic.frame_num == 0;
    } else {
        bitrate = c->enc_hevc_seq.bits_per_second;
        gop = c->enc_hevc_seq.intra_period;
        if (c->enc_hevc_seq.pic_width_in_luma_samples &&
            c->enc_hevc_seq.pic_height_in_luma_samples) {
            visible_width = c->enc_hevc_seq.pic_width_in_luma_samples;
            visible_height = c->enc_hevc_seq.pic_height_in_luma_samples;
        }
        force_key = c->enc_hevc_pic.pic_fields.bits.idr_pic_flag;
    }
    VASurfaceID target = c->current_target;
    coded->coded_segment.size = 0;
    coded->coded_segment.bit_offset = 0;
    coded->coded_segment.status = 0;
    coded->coded_segment.reserved = 0;
    coded->coded_segment.buf = coded->data;
    coded->coded_segment.next = NULL;
    coded->coded_ready = 0;
    size_t coded_capacity = coded->size;
    pthread_mutex_unlock(&drv->lock);

    if (!enc) {
        enc = dmd_v4l2_enc_open(c->codec, c->profile,
                                c->picture_width, c->picture_height,
                                visible_width, visible_height,
                                bitrate, gop, rate_control, qp);
        pthread_mutex_lock(&drv->lock);
        c = dmd_find_context_locked(drv, context);
        if (c && enc) c->encoder = enc;
        pthread_mutex_unlock(&drv->lock);
        if (!c || !enc) {
            if (enc) dmd_v4l2_enc_close(enc);
            pthread_mutex_lock(&drv->lock);
            drv->io_busy[context_index] = 0;
            pthread_cond_broadcast(&drv->io_done);
            c = dmd_find_context_locked(drv, context);
            if (c) {
                c->current_target = VA_INVALID_ID;
            }
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    void *scratch = malloc(coded_capacity);
    if (!scratch) {
        pthread_mutex_lock(&drv->lock);
        drv->io_busy[context_index] = 0;
        pthread_cond_broadcast(&drv->io_done);
        c = dmd_find_context_locked(drv, context);
        if (c) c->current_target = VA_INVALID_ID;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    pthread_mutex_lock(&drv->lock);
    c = dmd_find_context_locked(drv, context);
    if (!c) {
        dmd_log("encode EndPicture: context disappeared (context=%u)\n",
                (unsigned)context);
        if (c) {
            c->current_target = VA_INVALID_ID;
            drv->io_busy[context_index] = 0;
            pthread_cond_broadcast(&drv->io_done);
        }
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int pending_slot = -1;
    for (int i = 0; i < DMD_MAX_SURFACES; i++) {
        if (!c->encode_pending[i].in_use) {
            pending_slot = i;
            break;
        }
    }
    if (pending_slot < 0) {
        dmd_log("encode EndPicture: pending output table full (context=%u)\n",
                (unsigned)context);
        drv->io_busy[context_index] = 0;
        pthread_cond_broadcast(&drv->io_done);
        c->current_target = VA_INVALID_ID;
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    uint64_t input_pts = c->encode_frame_count * DMD_ENC_FRAME_INTERVAL_US;
    c->encode_pending[pending_slot].coded_id = coded_id;
    c->encode_pending[pending_slot].pts_us = input_pts;
    c->encode_pending[pending_slot].in_use = 1;
    pthread_mutex_unlock(&drv->lock);

    dmd_v4l2_enc_set_controls(enc, bitrate, gop);
    size_t coded_size = 0;
    uint64_t coded_pts = 0;
    int rc = dmd_v4l2_enc_frame(enc, surface, NULL, 0, scratch,
                                coded_capacity, &coded_size, &coded_pts,
                                force_key);

    pthread_mutex_lock(&drv->lock);
    drv->io_busy[context_index] = 0;
    pthread_cond_broadcast(&drv->io_done);
    c = dmd_find_context_locked(drv, context);
    coded = dmd_find_buffer_locked(drv, coded_id);
    surface = dmd_find_surface_locked(drv, target);
    if (!c || !coded || !surface) {
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    c->current_target = VA_INVALID_ID;
    if (rc < 0 || coded_size > UINT32_MAX) {
        dmd_log("encode EndPicture: V4L2 frame failed (codec=%d frame=%llu rc=%d coded_size=%zu)\n",
                c->codec, (unsigned long long)c->encode_frame_count, rc,
                coded_size);
        c->encode_pending[pending_slot].in_use = 0;
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    c->encode_frame_count++;
    if (coded_size && dmd_encode_store_output_locked(drv, c, coded_pts,
                                                     scratch, coded_size) < 0) {
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    pthread_mutex_unlock(&drv->lock);
    free(scratch);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_encode_sync_buffer(VADriverContextP ctx, VABufferID buffer,
                                uint64_t timeout_ns)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (buffer == VA_INVALID_ID)
        return VA_STATUS_SUCCESS;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *coded = dmd_find_buffer_locked(drv, buffer);
    if (!coded || coded->type != VAEncCodedBufferType) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    struct dmd_context *c = dmd_find_context_locked(drv, coded->context);
    if (!c || c->entrypoint != VAEntrypointEncSlice) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (coded->coded_ready) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }
    VAContextID coded_context = coded->context;

    uint64_t target_pts = 0;
    int found = 0;
    for (int i = 0; i < DMD_MAX_SURFACES; i++) {
        if (c->encode_pending[i].in_use &&
            c->encode_pending[i].coded_id == buffer) {
            target_pts = c->encode_pending[i].pts_us;
            found = 1;
            break;
        }
    }
    if (!found) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    int context_index = (int)(c - drv->contexts);
    while (drv->io_busy[context_index])
        pthread_cond_wait(&drv->io_done, &drv->lock);
    c = dmd_find_context_locked(drv, coded_context);
    coded = dmd_find_buffer_locked(drv, buffer);
    if (!coded) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!c || coded->coded_ready) {
        pthread_mutex_unlock(&drv->lock);
        return c ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    uint64_t frame_index = target_pts / DMD_ENC_FRAME_INTERVAL_US;
    uint64_t frames_submitted = c->encode_frame_count;
    if (frame_index >= frames_submitted ||
        frames_submitted - frame_index > 1) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("VAAPI encoder output did not arrive within the one-frame pipeline window\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    unsigned int expected_outputs = 0;
    for (int i = 0; i < DMD_MAX_SURFACES; i++)
        expected_outputs += c->encode_pending[i].in_use != 0;
    struct dmd_v4l2_enc *enc = c->encoder;
    VAContextID context = c->id;
    drv->io_busy[context_index] = 1;
    pthread_mutex_unlock(&drv->lock);

    struct dmd_encode_drain_context drain = { .drv = drv, .context = context };
    int rc = dmd_v4l2_enc_drain(enc, dmd_encode_store_drain_output, &drain,
                                expected_outputs);

    pthread_mutex_lock(&drv->lock);
    drv->io_busy[context_index] = 0;
    pthread_cond_broadcast(&drv->io_done);
    c = dmd_find_context_locked(drv, context);
    coded = dmd_find_buffer_locked(drv, buffer);
    int ready = coded && coded->coded_ready;
    pthread_mutex_unlock(&drv->lock);
    return rc == 0 && ready ? VA_STATUS_SUCCESS
                            : VA_STATUS_ERROR_OPERATION_FAILED;
}


VAStatus dmd_encode_destroy_context(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv) return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c || c->entrypoint != VAEntrypointEncSlice) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    int context_index = (int)(c - drv->contexts);
    while (drv->io_busy[context_index])
        pthread_cond_wait(&drv->io_done, &drv->lock);
    c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    struct dmd_v4l2_enc *enc = c->encoder;
    c->encoder = NULL;
    dmd_context_reset_locked(c);
    pthread_mutex_unlock(&drv->lock);
    dmd_v4l2_enc_close(enc);
    return VA_STATUS_SUCCESS;
}
