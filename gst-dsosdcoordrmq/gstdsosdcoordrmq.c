/**
 * Copyright (c) 2016-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA Corporation is strictly prohibited.
 *
 * version: 0.1
 */

#include <stdio.h>
#include <gst/gst.h>

#include <gst/video/video.h>
#include <gst/base/gstbasetransform.h>
#include "gstdsosdcoordrmq.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <jansson.h>

#include "nvbufsurface.h"
#include "nvtx3/nvToolsExt.h"
#include "rabbitmq-client.h"
#include "rabbitmq-client.c"

GST_DEBUG_CATEGORY_STATIC (gst_ds_osdcoordrmq_debug);
#define GST_CAT_DEFAULT gst_ds_osdcoordrmq_debug

/* For hw blending, color should be of the form:
   class_id1, R, G, B, A:class_id2, R, G, B, A */
#define DEFAULT_CLR "0,0.0,1.0,0.0,0.3:1,0.0,1.0,1.0,0.3:2,0.0,0.0,1.0,0.3:3,1.0,1.0,0.0,0.3"
#define MAX_OSD_ELEMS 1024

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

/* Enum to identify properties */
enum
{
  PROP_0,
  PROP_SHOW_CLOCK,
  PROP_SHOW_TEXT,
  PROP_CLOCK_FONT,
  PROP_CLOCK_FONT_SIZE,
  PROP_CLOCK_X_OFFSET,
  PROP_CLOCK_Y_OFFSET,
  PROP_CLOCK_COLOR,
  PROP_PROCESS_MODE,
  PROP_HW_BLEND_COLOR_ATTRS,
  PROP_GPU_DEVICE_ID,
  PROP_SHOW_BBOX,
  PROP_SHOW_MASK,
  PROP_SHOW_COORD,
};

/* the capabilities of the inputs and outputs. */
static GstStaticPadTemplate dsosdcoordrmq_sink_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ RGBA }")));

static GstStaticPadTemplate dsosdcoordrmq_src_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ RGBA }")));

/* Default values for properties */
#define DEFAULT_FONT_SIZE 12
#define DEFAULT_FONT "Serif"
#ifdef PLATFORM_TEGRA
#define GST_NV_OSD_DEFAULT_PROCESS_MODE MODE_HW
#else
#define GST_NV_OSD_DEFAULT_PROCESS_MODE MODE_GPU
#endif
#define MAX_FONT_SIZE 60
#define DEFAULT_BORDER_WIDTH 4

/* Define our element type. Standard GObject/GStreamer boilerplate stuff */
#define gst_ds_osdcoordrmq_parent_class parent_class
G_DEFINE_TYPE (GstDsOsdCoordRmq, gst_ds_osdcoordrmq, GST_TYPE_BASE_TRANSFORM);

#define GST_TYPE_NV_OSD_PROCESS_MODE (gst_ds_osdcoordrmq_process_mode_get_type ())

static GQuark _dsmeta_quark;

static GType
gst_ds_osdcoordrmq_process_mode_get_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {MODE_CPU, "CPU_MODE", "CPU_MODE"},
      {MODE_GPU, "GPU_MODE, yet to be implemented for Tegra", "GPU_MODE"},
#ifdef PLATFORM_TEGRA
      {MODE_HW,
            "HW_MODE. Only for Tegra. For rectdraw only.",
          "HW_MODE"},
#endif
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("GstDsOsdCoordRmqMode", values);
  }
  return qtype;
}

static void gst_ds_osdcoordrmq_finalize (GObject * object);
static void gst_ds_osdcoordrmq_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_ds_osdcoordrmq_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstFlowReturn gst_ds_osdcoordrmq_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);
static gboolean gst_ds_osdcoordrmq_start (GstBaseTransform * btrans);
static gboolean gst_ds_osdcoordrmq_stop (GstBaseTransform * btrans);
static gboolean gst_ds_osdcoordrmq_parse_color (GstDsOsdCoordRmq * dsosdcoordrmq,
    guint clock_color);

static gboolean gst_ds_osdcoordrmq_parse_hw_blend_color_attrs (GstDsOsdCoordRmq * dsosdcoordrmq,
    const gchar * arr);
static gboolean gst_ds_osdcoordrmq_get_hw_blend_color_attrs (GValue * value,
    GstDsOsdCoordRmq * dsosdcoordrmq);

json_t* build_json(METADATA* metadata_arr, int cnt);

/**
 * Called when source / sink pad capabilities have been negotiated.
 */
static gboolean
gst_ds_osdcoordrmq_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  gboolean ret = TRUE;

  GstDsOsdCoordRmq *dsosdcoordrmq = GST_DSOSDCOORDRMQ (trans);
  gint width = 0, height = 0;
  cudaError_t CUerr = cudaSuccess;

  dsosdcoordrmq->frame_num = 0;

  GstStructure *structure = gst_caps_get_structure (incaps, 0);

  GST_OBJECT_LOCK (dsosdcoordrmq);
  if (!gst_structure_get_int (structure, "width", &width) ||
      !gst_structure_get_int (structure, "height", &height)) {
    GST_ELEMENT_ERROR (dsosdcoordrmq, STREAM, FAILED,
        ("caps without width/height"), NULL);
    ret = FALSE;
    goto exit_set_caps;
  }
  if (dsosdcoordrmq->dsosdcoordrmq_context && dsosdcoordrmq->width == width
      && dsosdcoordrmq->height == height) {
    goto exit_set_caps;
  }

  CUerr = cudaSetDevice (dsosdcoordrmq->gpu_id);
  if (CUerr != cudaSuccess) {
    ret = FALSE;
    GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    goto exit_set_caps;
  }

  dsosdcoordrmq->width = width;
  dsosdcoordrmq->height = height;

  if (dsosdcoordrmq->show_clock)
    nvll_osd_set_clock_params (dsosdcoordrmq->dsosdcoordrmq_context,
        &dsosdcoordrmq->clock_text_params);

  dsosdcoordrmq->conv_buf =
      nvll_osd_set_params (dsosdcoordrmq->dsosdcoordrmq_context, dsosdcoordrmq->width,
      dsosdcoordrmq->height);

exit_set_caps:
  GST_OBJECT_UNLOCK (dsosdcoordrmq);
  return ret;
}

/**
 * Initialize all resources.
 */
static gboolean
gst_ds_osdcoordrmq_start (GstBaseTransform * btrans)
{
  GstDsOsdCoordRmq *dsosdcoordrmq = GST_DSOSDCOORDRMQ (btrans);

  cudaError_t CUerr = cudaSuccess;
  CUerr = cudaSetDevice (dsosdcoordrmq->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    return FALSE;
  }
  GST_LOG_OBJECT (dsosdcoordrmq, "SETTING CUDA DEVICE = %d in dsosdcoordrmq func=%s\n",
      dsosdcoordrmq->gpu_id, __func__);

  dsosdcoordrmq->dsosdcoordrmq_context = nvll_osd_create_context ();

  if (dsosdcoordrmq->dsosdcoordrmq_context == NULL) {
    GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
        ("Unable to create context dsosdcoordrmq"), NULL);
    return FALSE;
  }

  int flag_integrated = -1;
  cudaDeviceGetAttribute(&flag_integrated, cudaDevAttrIntegrated, dsosdcoordrmq->gpu_id);
  if(!flag_integrated && dsosdcoordrmq->dsosdcoordrmq_mode == MODE_HW) {
    dsosdcoordrmq->dsosdcoordrmq_mode = MODE_GPU;
  }

  if (dsosdcoordrmq->num_class_entries == 0) {
    gst_ds_osdcoordrmq_parse_hw_blend_color_attrs (dsosdcoordrmq, DEFAULT_CLR);
  }

  nvll_osd_init_colors_for_hw_blend (dsosdcoordrmq->dsosdcoordrmq_context,
      dsosdcoordrmq->color_info, dsosdcoordrmq->num_class_entries);

  if (dsosdcoordrmq->show_clock) {
    nvll_osd_set_clock_params (dsosdcoordrmq->dsosdcoordrmq_context,
        &dsosdcoordrmq->clock_text_params);
  }

  return TRUE;
}

/**
 * Free up all the resources
 */
static gboolean
gst_ds_osdcoordrmq_stop (GstBaseTransform * btrans)
{
  GstDsOsdCoordRmq *dsosdcoordrmq = GST_DSOSDCOORDRMQ (btrans);

  cudaError_t CUerr = cudaSuccess;
  CUerr = cudaSetDevice (dsosdcoordrmq->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    return FALSE;
  }
  GST_LOG_OBJECT (dsosdcoordrmq, "SETTING CUDA DEVICE = %d in dsosdcoordrmq func=%s\n",
      dsosdcoordrmq->gpu_id, __func__);

  if (dsosdcoordrmq->dsosdcoordrmq_context)
    nvll_osd_destroy_context (dsosdcoordrmq->dsosdcoordrmq_context);

  dsosdcoordrmq->dsosdcoordrmq_context = NULL;
  dsosdcoordrmq->width = 0;
  dsosdcoordrmq->height = 0;

  return TRUE;
}

int frame_num = 0;
int fnum_tmp=0;
json_t *root;
rabbitmq_cli cli;

/**
 * Called when element recieves an input buffer from upstream element.
 */
static GstFlowReturn
gst_ds_osdcoordrmq_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstDsOsdCoordRmq *dsosdcoordrmq = GST_DSOSDCOORDRMQ (trans);
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  unsigned int rect_cnt = 0;
  unsigned int segment_cnt = 0;
  unsigned int text_cnt = 0;
  unsigned int line_cnt = 0;
  unsigned int arrow_cnt = 0;
  unsigned int circle_cnt = 0;
  unsigned int i = 0;
  int idx = 0;
  gpointer state = NULL;
  NvBufSurface *surface = NULL;
  NvDsBatchMeta *batch_meta = NULL;

  METADATA metadata;
  char *str_obj;
  METADATA metadata_arr[128];
  int m_cnt=0;

  char *host_name = "x.x.x.x";
  int port = 32094;
  char *vhost_name = "tao";
  char *rmq_id = "guest";
  char *rmq_pass = "guest";
  char *queue_name = "peoplenet-metadata-queue-test";

  if (dsosdcoordrmq->frame_num == 0) {
    cli = new_rabbitmq_client(host_name, port, vhost_name, rmq_id, rmq_pass);
  }

  if (!gst_buffer_map (buf, &inmap, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
        ("Unable to map info from buffer"), NULL);
    return GST_FLOW_ERROR;
  }

  nvds_set_input_system_timestamp (buf, GST_ELEMENT_NAME (dsosdcoordrmq));

  cudaError_t CUerr = cudaSuccess;
  CUerr = cudaSetDevice (dsosdcoordrmq->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    return GST_FLOW_ERROR;
  }
  GST_LOG_OBJECT (dsosdcoordrmq, "SETTING CUDA DEVICE = %d in dsosdcoordrmq func=%s\n",
      dsosdcoordrmq->gpu_id, __func__);

  surface = (NvBufSurface *) inmap.data;

  /* Get metadata. Update rectangle and text params */
  GstMeta *gst_meta;
  NvDsMeta *dsmeta;
  char context_name[100];
  snprintf (context_name, sizeof (context_name), "%s_(Frame=%u)",
      GST_ELEMENT_NAME (dsosdcoordrmq), dsosdcoordrmq->frame_num);
  nvtxRangePushA (context_name);
  while ((gst_meta = gst_buffer_iterate_meta (buf, &state))) {
    if (gst_meta_api_type_has_tag (gst_meta->info->api, _dsmeta_quark)) {
      dsmeta = (NvDsMeta *) gst_meta;
      if (dsmeta->meta_type == NVDS_BATCH_GST_META) {
        batch_meta = (NvDsBatchMeta *) dsmeta->meta_data;
        break;
      }
    }
  }

  NvDsMetaList *l = NULL;
  NvDsMetaList *full_obj_meta_list = NULL;
  if (batch_meta)
    full_obj_meta_list = batch_meta->obj_meta_pool->full_list;
  NvDsObjectMeta *object_meta = NULL;

  for (l = full_obj_meta_list; l != NULL; l = l->next) {
    object_meta = (NvDsObjectMeta *) (l->data);
    if (dsosdcoordrmq->draw_bbox) {
      dsosdcoordrmq->rect_params[rect_cnt] = object_meta->rect_params;
#ifdef PLATFORM_TEGRA
      /* In case of hardware blending, values set in hw-blend-color-attr
         should be considered as rect bg color values*/
      if (dsosdcoordrmq->dsosdcoordrmq_mode == MODE_HW && dsosdcoordrmq->hw_blend) {
        for (idx = 0; idx < dsosdcoordrmq->num_class_entries; idx++) {
          if (dsosdcoordrmq->color_info[idx].id == object_meta->class_id) {
            dsosdcoordrmq->rect_params[rect_cnt].color_id = idx;
            dsosdcoordrmq->rect_params[rect_cnt].has_bg_color = TRUE;
            dsosdcoordrmq->rect_params[rect_cnt].bg_color.red =
              dsosdcoordrmq->color_info[idx].color.red;
            dsosdcoordrmq->rect_params[rect_cnt].bg_color.blue =
              dsosdcoordrmq->color_info[idx].color.blue;
            dsosdcoordrmq->rect_params[rect_cnt].bg_color.green =
              dsosdcoordrmq->color_info[idx].color.green;
            dsosdcoordrmq->rect_params[rect_cnt].bg_color.alpha =
              dsosdcoordrmq->color_info[idx].color.alpha;
            break;
          }
        }
      }
#endif
      rect_cnt++;
    }
    /* Get the label and coordinates of the drawn bboxs*/
    if (dsosdcoordrmq->display_coord) {
      metadata.frame_number = dsosdcoordrmq->frame_num;
      metadata.label = object_meta->text_params.display_text;
      metadata.top_left.x = object_meta->rect_params.left;
      metadata.top_left.y = object_meta->rect_params.top;
      metadata.top_right.x = object_meta->rect_params.left + object_meta->rect_params.width;
      metadata.top_right.y = object_meta->rect_params.top;
      metadata.bottom_left.x = object_meta->rect_params.left;
      metadata.bottom_left.y = object_meta->rect_params.top + object_meta->rect_params.height;
      metadata.bottom_right.x = object_meta->rect_params.left + object_meta->rect_params.width;
      metadata.bottom_right.y = object_meta->rect_params.top + object_meta->rect_params.height;

      metadata_arr[m_cnt] = metadata;
      m_cnt++;
    }

    if (rect_cnt == MAX_OSD_ELEMS) {
      dsosdcoordrmq->frame_rect_params->num_rects = rect_cnt;
      dsosdcoordrmq->frame_rect_params->rect_params_list = dsosdcoordrmq->rect_params;
      dsosdcoordrmq->frame_rect_params->buf_ptr = &surface->surfaceList[0];
      dsosdcoordrmq->frame_rect_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
      if (nvll_osd_draw_rectangles (dsosdcoordrmq->dsosdcoordrmq_context,
              dsosdcoordrmq->frame_rect_params) == -1) {
        GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
            ("Unable to draw rectangles"), NULL);
        return GST_FLOW_ERROR;
      }
      rect_cnt = 0;
    }
    if (dsosdcoordrmq->draw_mask && object_meta->mask_params.data &&
                              object_meta->mask_params.size > 0) {
      dsosdcoordrmq->mask_rect_params[segment_cnt] = object_meta->rect_params;
      dsosdcoordrmq->mask_params[segment_cnt++] = object_meta->mask_params;
      if (segment_cnt == MAX_OSD_ELEMS) {
        dsosdcoordrmq->frame_mask_params->num_segments = segment_cnt;
        dsosdcoordrmq->frame_mask_params->rect_params_list = dsosdcoordrmq->mask_rect_params;
        dsosdcoordrmq->frame_mask_params->mask_params_list = dsosdcoordrmq->mask_params;
        dsosdcoordrmq->frame_mask_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoordrmq->frame_mask_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
        if (nvll_osd_draw_segment_masks (dsosdcoordrmq->dsosdcoordrmq_context,
                dsosdcoordrmq->frame_mask_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
              ("Unable to draw rectangles"), NULL);
          return GST_FLOW_ERROR;
        }
        segment_cnt = 0;
      }
    }
    if (object_meta->text_params.display_text)
      dsosdcoordrmq->text_params[text_cnt++] = object_meta->text_params;
    if (text_cnt == MAX_OSD_ELEMS) {
      dsosdcoordrmq->frame_text_params->num_strings = text_cnt;
      dsosdcoordrmq->frame_text_params->text_params_list = dsosdcoordrmq->text_params;
      dsosdcoordrmq->frame_text_params->buf_ptr = &surface->surfaceList[0];
      dsosdcoordrmq->frame_text_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
      if (nvll_osd_put_text (dsosdcoordrmq->dsosdcoordrmq_context,
              dsosdcoordrmq->frame_text_params) == -1) {
        GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
            ("Unable to draw text"), NULL);
        return GST_FLOW_ERROR;
      }
      text_cnt = 0;
    }
  }

  /* Send metadata in JSON format to RabbitMQ*/
  root = build_json(metadata_arr, m_cnt);
  if (m_cnt > 0) {
    str_obj = json_dumps(root, 0);
    int reply = rabbitmq_cli_publish(cli , queue_name, str_obj);
    // printf("%s\n", str_obj);
  }

  NvDsMetaList *display_meta_list = NULL;
  if (batch_meta)
    display_meta_list = batch_meta->display_meta_pool->full_list;
  NvDsDisplayMeta *display_meta = NULL;

  /* Get objects to be drawn from display meta.
   * Draw objects if count equals MAX_OSD_ELEMS.
   */
  for (l = display_meta_list; l != NULL; l = l->next) {
    display_meta = (NvDsDisplayMeta *) (l->data);

    unsigned int cnt = 0;
    for (cnt = 0; cnt < display_meta->num_rects; cnt++) {
      dsosdcoordrmq->rect_params[rect_cnt++] = display_meta->rect_params[cnt];
      if (rect_cnt == MAX_OSD_ELEMS) {
        dsosdcoordrmq->frame_rect_params->num_rects = rect_cnt;
        dsosdcoordrmq->frame_rect_params->rect_params_list = dsosdcoordrmq->rect_params;
        dsosdcoordrmq->frame_rect_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoordrmq->frame_rect_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
        if (nvll_osd_draw_rectangles (dsosdcoordrmq->dsosdcoordrmq_context,
                dsosdcoordrmq->frame_rect_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
              ("Unable to draw rectangles"), NULL);
          return GST_FLOW_ERROR;
        }
        rect_cnt = 0;
      }
    }

    for (cnt = 0; cnt < display_meta->num_labels; cnt++) {
      if (display_meta->text_params[cnt].display_text) {
        dsosdcoordrmq->text_params[text_cnt++] = display_meta->text_params[cnt];
        if (text_cnt == MAX_OSD_ELEMS) {
          dsosdcoordrmq->frame_text_params->num_strings = text_cnt;
          dsosdcoordrmq->frame_text_params->text_params_list = dsosdcoordrmq->text_params;
          dsosdcoordrmq->frame_text_params->buf_ptr = &surface->surfaceList[0];
          dsosdcoordrmq->frame_text_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
          if (nvll_osd_put_text (dsosdcoordrmq->dsosdcoordrmq_context,
                  dsosdcoordrmq->frame_text_params) == -1) {
            GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
                ("Unable to draw text"), NULL);
            return GST_FLOW_ERROR;
          }
          text_cnt = 0;
        }
      }
    }

    for (cnt = 0; cnt < display_meta->num_lines; cnt++) {
      dsosdcoordrmq->line_params[line_cnt++] = display_meta->line_params[cnt];
      if (line_cnt == MAX_OSD_ELEMS) {
        dsosdcoordrmq->frame_line_params->num_lines = line_cnt;
        dsosdcoordrmq->frame_line_params->line_params_list = dsosdcoordrmq->line_params;
        dsosdcoordrmq->frame_line_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoordrmq->frame_line_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
        if (nvll_osd_draw_lines (dsosdcoordrmq->dsosdcoordrmq_context,
                dsosdcoordrmq->frame_line_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
              ("Unable to draw lines"), NULL);
          return GST_FLOW_ERROR;
        }
        line_cnt = 0;
      }
    }

    for (cnt = 0; cnt < display_meta->num_arrows; cnt++) {
      dsosdcoordrmq->arrow_params[arrow_cnt++] = display_meta->arrow_params[cnt];
      if (arrow_cnt == MAX_OSD_ELEMS) {
        dsosdcoordrmq->frame_arrow_params->num_arrows = arrow_cnt;
        dsosdcoordrmq->frame_arrow_params->arrow_params_list = dsosdcoordrmq->arrow_params;
        dsosdcoordrmq->frame_arrow_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoordrmq->frame_arrow_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
        if (nvll_osd_draw_arrows (dsosdcoordrmq->dsosdcoordrmq_context,
                dsosdcoordrmq->frame_arrow_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
              ("Unable to draw arrows"), NULL);
          return GST_FLOW_ERROR;
        }
        arrow_cnt = 0;
      }
    }

    for (cnt = 0; cnt < display_meta->num_circles; cnt++) {
      dsosdcoordrmq->circle_params[circle_cnt++] = display_meta->circle_params[cnt];
      if (circle_cnt == MAX_OSD_ELEMS) {
        dsosdcoordrmq->frame_circle_params->num_circles = circle_cnt;
        dsosdcoordrmq->frame_circle_params->circle_params_list =
            dsosdcoordrmq->circle_params;
        dsosdcoordrmq->frame_circle_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoordrmq->frame_circle_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
        if (nvll_osd_draw_circles (dsosdcoordrmq->dsosdcoordrmq_context,
                dsosdcoordrmq->frame_circle_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
              ("Unable to draw circles"), NULL);
          return GST_FLOW_ERROR;
        }
        circle_cnt = 0;
      }
    }
    i++;
  }

  dsosdcoordrmq->num_rect = rect_cnt;
  dsosdcoordrmq->num_segments = segment_cnt;
  dsosdcoordrmq->num_strings = text_cnt;
  dsosdcoordrmq->num_lines = line_cnt;
  dsosdcoordrmq->num_arrows = arrow_cnt;
  dsosdcoordrmq->num_circles = circle_cnt;
  if (rect_cnt != 0 && dsosdcoordrmq->draw_bbox) {
    dsosdcoordrmq->frame_rect_params->num_rects = dsosdcoordrmq->num_rect;
    dsosdcoordrmq->frame_rect_params->rect_params_list = dsosdcoordrmq->rect_params;
    dsosdcoordrmq->frame_rect_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoordrmq->frame_rect_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
    if (nvll_osd_draw_rectangles (dsosdcoordrmq->dsosdcoordrmq_context,
            dsosdcoordrmq->frame_rect_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
          ("Unable to draw rectangles"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (segment_cnt != 0 && dsosdcoordrmq->draw_mask) {
    dsosdcoordrmq->frame_mask_params->num_segments = dsosdcoordrmq->num_segments;
    dsosdcoordrmq->frame_mask_params->rect_params_list = dsosdcoordrmq->mask_rect_params;
    dsosdcoordrmq->frame_mask_params->mask_params_list = dsosdcoordrmq->mask_params;
    dsosdcoordrmq->frame_mask_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoordrmq->frame_mask_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
    if (nvll_osd_draw_segment_masks (dsosdcoordrmq->dsosdcoordrmq_context,
            dsosdcoordrmq->frame_mask_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
          ("Unable to draw segment masks"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  if ((dsosdcoordrmq->show_clock || text_cnt) && dsosdcoordrmq->draw_text) {
    dsosdcoordrmq->frame_text_params->num_strings = dsosdcoordrmq->num_strings;
    dsosdcoordrmq->frame_text_params->text_params_list = dsosdcoordrmq->text_params;
    dsosdcoordrmq->frame_text_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoordrmq->frame_text_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
    if (nvll_osd_put_text (dsosdcoordrmq->dsosdcoordrmq_context,
            dsosdcoordrmq->frame_text_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED, ("Unable to draw text"),
          NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (line_cnt != 0) {
    dsosdcoordrmq->frame_line_params->num_lines = dsosdcoordrmq->num_lines;
    dsosdcoordrmq->frame_line_params->line_params_list = dsosdcoordrmq->line_params;
    dsosdcoordrmq->frame_line_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoordrmq->frame_line_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
    if (nvll_osd_draw_lines (dsosdcoordrmq->dsosdcoordrmq_context,
            dsosdcoordrmq->frame_line_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED, ("Unable to draw lines"),
          NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (arrow_cnt != 0) {
    dsosdcoordrmq->frame_arrow_params->num_arrows = dsosdcoordrmq->num_arrows;
    dsosdcoordrmq->frame_arrow_params->arrow_params_list = dsosdcoordrmq->arrow_params;
    dsosdcoordrmq->frame_arrow_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoordrmq->frame_arrow_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
    if (nvll_osd_draw_arrows (dsosdcoordrmq->dsosdcoordrmq_context,
            dsosdcoordrmq->frame_arrow_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
          ("Unable to draw arrows"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (circle_cnt != 0) {
    dsosdcoordrmq->frame_circle_params->num_circles = dsosdcoordrmq->num_circles;
    dsosdcoordrmq->frame_circle_params->circle_params_list = dsosdcoordrmq->circle_params;
    dsosdcoordrmq->frame_circle_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoordrmq->frame_circle_params->mode = dsosdcoordrmq->dsosdcoordrmq_mode;
    if (nvll_osd_draw_circles (dsosdcoordrmq->dsosdcoordrmq_context,
            dsosdcoordrmq->frame_circle_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoordrmq, RESOURCE, FAILED,
          ("Unable to draw circles"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  nvtxRangePop ();
  dsosdcoordrmq->frame_num++;

  nvds_set_output_system_timestamp (buf, GST_ELEMENT_NAME (dsosdcoordrmq));

  gst_buffer_unmap (buf, &inmap);

  return GST_FLOW_OK;
}

/* Called when the plugin is destroyed.
 * Free all structures which have been malloc'd.
 */
static void
gst_ds_osdcoordrmq_finalize (GObject * object)
{
  GstDsOsdCoordRmq *dsosdcoordrmq = GST_DSOSDCOORDRMQ (object);

  if (dsosdcoordrmq->clock_text_params.font_params.font_name) {
    g_free ((char *) dsosdcoordrmq->clock_text_params.font_params.font_name);
  }
  g_free (dsosdcoordrmq->rect_params);
  g_free (dsosdcoordrmq->mask_rect_params);
  g_free (dsosdcoordrmq->mask_params);
  g_free (dsosdcoordrmq->text_params);
  g_free (dsosdcoordrmq->line_params);
  g_free (dsosdcoordrmq->arrow_params);
  g_free (dsosdcoordrmq->circle_params);

  g_free (dsosdcoordrmq->frame_rect_params);
  g_free (dsosdcoordrmq->frame_mask_params);
  g_free (dsosdcoordrmq->frame_text_params);
  g_free (dsosdcoordrmq->frame_line_params);
  g_free (dsosdcoordrmq->frame_arrow_params);
  g_free (dsosdcoordrmq->frame_circle_params);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* Install properties, set sink and src pad capabilities, override the required
 * functions of the base class, These are common to all instances of the
 * element.
 */
static void
gst_ds_osdcoordrmq_class_init (GstDsOsdCoordRmqClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_ds_osdcoordrmq_transform_ip);
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_ds_osdcoordrmq_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_ds_osdcoordrmq_stop);
  base_transform_class->set_caps = GST_DEBUG_FUNCPTR (gst_ds_osdcoordrmq_set_caps);

  gobject_class->set_property = gst_ds_osdcoordrmq_set_property;
  gobject_class->get_property = gst_ds_osdcoordrmq_get_property;
  gobject_class->finalize = gst_ds_osdcoordrmq_finalize;

  base_transform_class->passthrough_on_same_caps = TRUE;

  g_object_class_install_property (gobject_class, PROP_SHOW_CLOCK,
      g_param_spec_boolean ("display-clock", "clock",
          "Whether to display clock", FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_TEXT,
      g_param_spec_boolean ("display-text", "text", "Whether to display text",
          TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_BBOX,
      g_param_spec_boolean ("display-bbox", "text", "Whether to display bounding boxes",
          TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_MASK,
      g_param_spec_boolean ("display-mask", "text", "Whether to display instance mask",
          TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_COORD,
      g_param_spec_boolean ("display-coord", "text", "Whether to display coordinate",
	  TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT,
      g_param_spec_string ("clock-font", "clock-font",
          "Clock Font to be set",
          "DEFAULT_FONT",
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT_SIZE,
      g_param_spec_uint ("clock-font-size", "clock-font-size",
          "font size of the clock",
          0, MAX_FONT_SIZE, DEFAULT_FONT_SIZE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CLOCK_X_OFFSET,
      g_param_spec_uint ("x-clock-offset", "x-clock-offset",
          "x-clock-offset",
          0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CLOCK_Y_OFFSET,
      g_param_spec_uint ("y-clock-offset", "y-clock-offset",
          "y-clock-offset",
          0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CLOCK_COLOR,
      g_param_spec_uint ("clock-color", "clock-color",
          "clock-color",
          0, G_MAXUINT, G_MAXUINT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_PROCESS_MODE,
      g_param_spec_enum ("process-mode", "Process Mode",
          "Rect and text draw process mode",
          GST_TYPE_NV_OSD_PROCESS_MODE,
          GST_NV_OSD_DEFAULT_PROCESS_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_HW_BLEND_COLOR_ATTRS,
      g_param_spec_string ("hw-blend-color-attr", "HW Blend Color Attr",
          "color attributes for all classes,\n"
          "\t\t\t Use string with values of color class atrributes \n"
          "\t\t\t in ClassID (int), r(float), g(float), b(float), a(float)\n"
          "\t\t\t in order to set the property.\n"
          "\t\t\t Applicable only for HW mode on Jetson.\n"
          "\t\t\t e.g. 0,0.0,1.0,0.0,0.3:1,1.0,0.0,0.3,0.3",
          DEFAULT_CLR,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_GPU_DEVICE_ID,
      g_param_spec_uint ("gpu-id", "Set GPU Device ID",
          "Set GPU Device ID",
          0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  gst_element_class_set_details_simple (gstelement_class,
      "DsOsdCoordRmq plugin",
      "DsOsdCoordRmq functionality",
      "Gstreamer bounding box draw element",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&dsosdcoordrmq_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&dsosdcoordrmq_sink_factory));

  _dsmeta_quark = g_quark_from_static_string (NVDS_META_STRING);
}

/* Function called when a property of the element is set. Standard boilerplate.
 */
static void
gst_ds_osdcoordrmq_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDsOsdCoordRmq *dsosdcoordrmq = GST_DSOSDCOORDRMQ (object);

  switch (prop_id) {
    case PROP_SHOW_CLOCK:
      dsosdcoordrmq->show_clock = g_value_get_boolean (value);
      break;
    case PROP_SHOW_TEXT:
      dsosdcoordrmq->draw_text = g_value_get_boolean (value);
      break;
    case PROP_SHOW_BBOX:
      dsosdcoordrmq->draw_bbox = g_value_get_boolean (value);
      break;
    case PROP_SHOW_MASK:
      dsosdcoordrmq->draw_mask = g_value_get_boolean (value);
      break;
    case PROP_SHOW_COORD:
      dsosdcoordrmq->display_coord = g_value_get_boolean (value);
      break;
    case PROP_CLOCK_FONT:
      if (dsosdcoordrmq->clock_text_params.font_params.font_name) {
        g_free ((char *) dsosdcoordrmq->clock_text_params.font_params.font_name);
      }
      dsosdcoordrmq->clock_text_params.font_params.font_name =
          (gchar *) g_value_dup_string (value);
      break;
    case PROP_CLOCK_FONT_SIZE:
      dsosdcoordrmq->clock_text_params.font_params.font_size =
          g_value_get_uint (value);
      break;
    case PROP_CLOCK_X_OFFSET:
      dsosdcoordrmq->clock_text_params.x_offset = g_value_get_uint (value);
      break;
    case PROP_CLOCK_Y_OFFSET:
      dsosdcoordrmq->clock_text_params.y_offset = g_value_get_uint (value);
      break;
    case PROP_CLOCK_COLOR:
      gst_ds_osdcoordrmq_parse_color (dsosdcoordrmq, g_value_get_uint (value));
      break;
    case PROP_PROCESS_MODE:
      dsosdcoordrmq->dsosdcoordrmq_mode = (NvOSD_Mode) g_value_get_enum (value);
      break;
    case PROP_HW_BLEND_COLOR_ATTRS:
      dsosdcoordrmq->hw_blend = TRUE;
      gst_ds_osdcoordrmq_parse_hw_blend_color_attrs (dsosdcoordrmq,
          g_value_get_string (value));
      break;
    case PROP_GPU_DEVICE_ID:
      dsosdcoordrmq->gpu_id = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Function called when a property of the element is requested. Standard
 * boilerplate.
 */
static void
gst_ds_osdcoordrmq_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDsOsdCoordRmq *dsosdcoordrmq = GST_DSOSDCOORDRMQ (object);

  switch (prop_id) {
    case PROP_SHOW_CLOCK:
      g_value_set_boolean (value, dsosdcoordrmq->show_clock);
      break;
    case PROP_SHOW_TEXT:
      g_value_set_boolean (value, dsosdcoordrmq->draw_text);
      break;
    case PROP_SHOW_BBOX:
      g_value_set_boolean (value, dsosdcoordrmq->draw_bbox);
      break;
    case PROP_SHOW_MASK:
      g_value_set_boolean (value, dsosdcoordrmq->draw_mask);
      break;
    case PROP_SHOW_COORD:
      g_value_set_boolean (value, dsosdcoordrmq->display_coord);
      break;
    case PROP_CLOCK_FONT:
      g_value_set_string (value, dsosdcoordrmq->font);
      break;
    case PROP_CLOCK_FONT_SIZE:
      g_value_set_uint (value, dsosdcoordrmq->clock_font_size);
      break;
    case PROP_CLOCK_X_OFFSET:
      g_value_set_uint (value, dsosdcoordrmq->clock_text_params.x_offset);
      break;
    case PROP_CLOCK_Y_OFFSET:
      g_value_set_uint (value, dsosdcoordrmq->clock_text_params.y_offset);
      break;
    case PROP_CLOCK_COLOR:
      g_value_set_uint (value, dsosdcoordrmq->clock_color);
      break;
    case PROP_PROCESS_MODE:
      g_value_set_enum (value, dsosdcoordrmq->dsosdcoordrmq_mode);
      break;
    case PROP_HW_BLEND_COLOR_ATTRS:
      gst_ds_osdcoordrmq_get_hw_blend_color_attrs (value, dsosdcoordrmq);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint (value, dsosdcoordrmq->gpu_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Set default values of certain properties.
 */
static void
gst_ds_osdcoordrmq_init (GstDsOsdCoordRmq * dsosdcoordrmq)
{
  dsosdcoordrmq->show_clock = FALSE;
  dsosdcoordrmq->draw_text = TRUE;
  dsosdcoordrmq->draw_bbox = TRUE;
  dsosdcoordrmq->draw_mask = FALSE;
  dsosdcoordrmq->display_coord = TRUE;
  dsosdcoordrmq->clock_text_params.font_params.font_name = g_strdup (DEFAULT_FONT);
  dsosdcoordrmq->clock_text_params.font_params.font_size = DEFAULT_FONT_SIZE;
  dsosdcoordrmq->dsosdcoordrmq_mode = GST_NV_OSD_DEFAULT_PROCESS_MODE;
  dsosdcoordrmq->border_width = DEFAULT_BORDER_WIDTH;
  dsosdcoordrmq->num_rect = 0;
  dsosdcoordrmq->num_segments = 0;
  dsosdcoordrmq->num_strings = 0;
  dsosdcoordrmq->num_lines = 0;
  dsosdcoordrmq->clock_text_params.font_params.font_color.red = 1.0;
  dsosdcoordrmq->clock_text_params.font_params.font_color.green = 0.0;
  dsosdcoordrmq->clock_text_params.font_params.font_color.blue = 0.0;
  dsosdcoordrmq->clock_text_params.font_params.font_color.alpha = 1.0;
  dsosdcoordrmq->rect_params = g_new0 (NvOSD_RectParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->mask_rect_params = g_new0 (NvOSD_RectParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->mask_params = g_new0 (NvOSD_MaskParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->text_params = g_new0 (NvOSD_TextParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->line_params = g_new0 (NvOSD_LineParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->arrow_params = g_new0 (NvOSD_ArrowParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->circle_params = g_new0 (NvOSD_CircleParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->frame_rect_params = g_new0 (NvOSD_FrameRectParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->frame_mask_params = g_new0 (NvOSD_FrameSegmentMaskParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->frame_text_params = g_new0 (NvOSD_FrameTextParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->frame_line_params = g_new0 (NvOSD_FrameLineParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->frame_arrow_params = g_new0 (NvOSD_FrameArrowParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->frame_circle_params =
      g_new0 (NvOSD_FrameCircleParams, MAX_OSD_ELEMS);
  dsosdcoordrmq->hw_blend = FALSE;
}

/**
 * Set color of text for clock, if enabled.
 */
static gboolean
gst_ds_osdcoordrmq_parse_color (GstDsOsdCoordRmq * dsosdcoordrmq, guint clock_color)
{
  dsosdcoordrmq->clock_text_params.font_params.font_color.red =
      (gfloat) ((clock_color & 0xff000000) >> 24) / 255;
  dsosdcoordrmq->clock_text_params.font_params.font_color.green =
      (gfloat) ((clock_color & 0x00ff0000) >> 16) / 255;
  dsosdcoordrmq->clock_text_params.font_params.font_color.blue =
      (gfloat) ((clock_color & 0x0000ff00) >> 8) / 255;
  dsosdcoordrmq->clock_text_params.font_params.font_color.alpha =
      (gfloat) ((clock_color & 0x000000ff)) / 255;
  return TRUE;
}

/**
 * Boiler plate for registering a plugin and an element.
 */
static gboolean
dsosdcoordrmq_init (GstPlugin * dsosdcoordrmq)
{
  GST_DEBUG_CATEGORY_INIT (gst_ds_osdcoordrmq_debug, "dsosdcoordrmq", 0, "dsosdcoordrmq plugin");

  return gst_element_register (dsosdcoordrmq, "dsosdcoordrmq", GST_RANK_PRIMARY,
      GST_TYPE_DSOSDCOORDRMQ);
}

static gboolean
gst_ds_osdcoordrmq_parse_hw_blend_color_attrs (GstDsOsdCoordRmq * dsosdcoordrmq,
    const gchar * arr)
{
  gchar *str = (gchar *) arr;
  int idx = 0;
  int class_id = 0;

  while (str != NULL && str[0] != '\0') {
    class_id = atoi (str);
    if (class_id >= MAX_BG_CLR) {
      g_print ("dsosdcoordrmq: class_id %d is exceeding than %d\n", class_id,
          MAX_BG_CLR);
      exit (-1);
    }
    dsosdcoordrmq->color_info[idx].id = class_id;
    str = g_strstr_len (str, -1, ",") + 1;

    dsosdcoordrmq->color_info[idx].color.red = atof (str);
    str = g_strstr_len (str, -1, ",") + 1;
    dsosdcoordrmq->color_info[idx].color.green = atof (str);
    str = g_strstr_len (str, -1, ",") + 1;
    dsosdcoordrmq->color_info[idx].color.blue = atof (str);
    str = g_strstr_len (str, -1, ",") + 1;
    dsosdcoordrmq->color_info[idx].color.alpha = atof (str);
    str = g_strstr_len (str, -1, ":");

    if (str) {
      str = str + 1;
    }
    idx++;
    if (idx >= MAX_BG_CLR) {
      g_print ("idx (%d) entries exceeded MAX_CLASSES %d\n", idx, MAX_BG_CLR);
      break;
    }
  }

  dsosdcoordrmq->num_class_entries = idx;
  return TRUE;
}

static gboolean
gst_ds_osdcoordrmq_get_hw_blend_color_attrs (GValue * value, GstDsOsdCoordRmq * dsosdcoordrmq)
{
  int idx = 0;
  gchar arr[100];

  while (idx < (dsosdcoordrmq->num_class_entries - 1)) {
    sprintf (arr, "%d,%f,%f,%f,%f:",
        dsosdcoordrmq->color_info[idx].id, dsosdcoordrmq->color_info[idx].color.red,
        dsosdcoordrmq->color_info[idx].color.green,
        dsosdcoordrmq->color_info[idx].color.blue,
        dsosdcoordrmq->color_info[idx].color.alpha);
    idx++;
  }
  sprintf (arr, "%d,%f,%f,%f,%f:",
      dsosdcoordrmq->color_info[idx].id, dsosdcoordrmq->color_info[idx].color.red,
      dsosdcoordrmq->color_info[idx].color.green,
      dsosdcoordrmq->color_info[idx].color.blue,
      dsosdcoordrmq->color_info[idx].color.alpha);

  g_value_set_string (value, arr);
  return TRUE;
}

json_t* build_json(METADATA* metadata_arr, int cnt)
{
  json_t *root = json_object();
  json_error_t err;
  json_t *results = json_array();
  json_t *object = json_object();
  json_t *coordinate = json_object();
  json_t *jtop_left = json_object();
  json_t *jtop_right = json_object();
  json_t *jbottom_left = json_object();
  json_t *jbottom_right = json_object();

  for (int i=0; i<cnt; i++) {
    json_object_set(jtop_left, "x", json_integer(metadata_arr[i].top_left.x));
    json_object_set(jtop_left, "y", json_integer(metadata_arr[i].top_left.y));
    json_object_set(jtop_right, "x", json_integer(metadata_arr[i].top_right.x));
    json_object_set(jtop_right, "y", json_integer(metadata_arr[i].top_right.y));
    json_object_set(jbottom_left, "x", json_integer(metadata_arr[i].bottom_left.x));
    json_object_set(jbottom_left, "y", json_integer(metadata_arr[i].bottom_left.y));
    json_object_set(jbottom_right, "x", json_integer(metadata_arr[i].bottom_right.x));
    json_object_set(jbottom_right, "y", json_integer(metadata_arr[i].bottom_right.y));

    json_object_set(coordinate, "topLeft", jtop_left);
    json_object_set(coordinate, "topRight", jtop_right);
    json_object_set(coordinate, "bottomLeft", jbottom_left);
    json_object_set(coordinate, "bottomRight", jbottom_right);

    json_object_set(object, "label", json_string(metadata_arr[i].label));
    json_object_set(object, "coordinate", coordinate);
    json_array_append(results, object);
  }

  json_object_set(root, "frameNumber", json_integer(metadata_arr[0].frame_number));
  json_object_set(root, "inferredResult", results);

  return root;
}


#ifndef PACKAGE
#define PACKAGE "dsosdcoordrmq"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvdsgst_dsosdcoordrmq,
    PACKAGE_DESCRIPTION,
    dsosdcoordrmq_init, DS_VERSION, PACKAGE_LICENSE, PACKAGE_NAME, PACKAGE_URL)



