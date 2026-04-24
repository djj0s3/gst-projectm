#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef USE_GLEW
#include <GL/glew.h>
#endif
#include <glib/gstdio.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gst.h>
#include <gst/pbutils/gstaudiovisualizer.h>

#include <projectM-4/projectM.h>

#include <string.h>

#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DRAW_BUFFER
#define GL_DRAW_BUFFER 0x0C01
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16 0x81A5
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif

#define GST_PROJECTM_TIMELINE_EPSILON (1e-6)
#define GST_PROJECTM_PBO_COUNT 3

#include "caps.h"
#include "config.h"
#include "debug.h"
#include "enums.h"
#include "gstglbaseaudiovisualizer.h"
#include "plugin.h"
#include "projectm.h"

GST_DEBUG_CATEGORY_STATIC(gst_projectm_debug);
#define GST_CAT_DEFAULT gst_projectm_debug

typedef struct {
  gdouble start_time;
  gdouble duration;
  gdouble end_time;
  gchar *preset;
  gchar *complexity;
} GstProjectMTimelineEntry;

static void gst_projectm_timeline_entry_free(gpointer data);
static void gst_projectm_timeline_reset(GstProjectM *plugin);
static gboolean gst_projectm_load_timeline(GstProjectM *plugin,
                                           const gchar *path);
static void gst_projectm_activate_timeline(GstProjectM *plugin);
static void gst_projectm_timeline_update(GstProjectM *plugin,
                                         gdouble elapsed_seconds);
static gchar *gst_projectm_resolve_preset_path(GstProjectM *plugin,
                                               const gchar *preset_value);
static gint gst_projectm_timeline_find_target_index(GstProjectM *plugin,
                                                    gdouble elapsed_seconds);

static gboolean gst_projectm_ensure_pbos(GstProjectM *plugin,
                                         const GstGLFuncs *glFunctions,
                                         gsize width, gsize height);
static void gst_projectm_release_pbos(GstProjectM *plugin,
                                      const GstGLFuncs *glFunctions);
static gboolean gst_projectm_ensure_render_target(GstProjectM *plugin,
                                                  const GstGLFuncs *glFunctions,
                                                  gsize width, gsize height);
static void gst_projectm_release_render_target(GstProjectM *plugin,
                                               const GstGLFuncs *glFunctions);
static gboolean gst_projectm_download_frame_with_pbo(
    GstProjectM *plugin, const GstGLFuncs *glFunctions, GstVideoFrame *video,
    gsize width, gsize height);

static gboolean gst_projectm_nv12_init(GstProjectM *plugin,
                                       const GstGLFuncs *glFunctions,
                                       gsize width, gsize height);
static void gst_projectm_nv12_release(GstProjectM *plugin,
                                      const GstGLFuncs *glFunctions);
static gboolean gst_projectm_nv12_render(GstProjectM *plugin,
                                         const GstGLFuncs *glFunctions,
                                         GstVideoFrame *video,
                                         gsize width, gsize height);
static void gst_projectm_copy_to_frame(GstVideoFrame *video, const guint8 *src,
                                       gsize width, gsize height);

struct _GstProjectMPrivate {
  GLenum gl_format;
  projectm_handle handle;

  GstClockTime first_frame_time;
  gboolean first_frame_received;
  GstClockTime first_audio_time;
  gboolean first_audio_received;
  guint64 render_frame_count;

  GPtrArray *timeline_entries;
  gint current_timeline_index;
  gboolean timeline_active;
  gboolean timeline_initialized;

  GLuint pbo_ids[GST_PROJECTM_PBO_COUNT];
  gsize pbo_size;
  gsize pbo_width;
  gsize pbo_height;
  guint pbo_index;
  gboolean pbo_initialized;
  gboolean pbo_frame_valid;

  GLuint fbo_id;
  GLuint fbo_texture_id;
  GLuint fbo_depth_buffer_id;
  gsize fbo_width;
  gsize fbo_height;
  gboolean fbo_initialized;
  gboolean fbo_warned_missing_support;

  gboolean headless_mode;
  gboolean headless_checked;

  /* NV12 output state — populated only when negotiated output format is NV12.
   * Plugin renders projectm into the existing RGBA FBO, then runs two shader
   * passes (Y full-res, UV half-res) into helper FBOs whose color attachments
   * are R8 / RG8 textures. ReadPixels on each plane writes directly into the
   * GstVideoFrame's NV12 plane data — no CPU-side ABGR→NV12 conversion. */
  gboolean nv12_mode;
  gboolean nv12_initialized;
  GLuint   nv12_y_fbo;
  GLuint   nv12_y_tex;
  GLuint   nv12_uv_fbo;
  GLuint   nv12_uv_tex;
  GLuint   nv12_program;
  GLuint   nv12_vao;     /* required by GL 3.2 core profile (macOS) */
  GLuint   nv12_vbo;
  GLint    nv12_uniform_tex;
  GLint    nv12_uniform_pass;
  GLint    nv12_attrib_pos;
  GLint    nv12_attrib_uv;
  gsize    nv12_width;   /* full-res Y plane size — tracked to detect resize */
  gsize    nv12_height;
};

G_DEFINE_TYPE_WITH_CODE(GstProjectM, gst_projectm,
                        GST_TYPE_GL_BASE_AUDIO_VISUALIZER,
                        G_ADD_PRIVATE(GstProjectM)
                            GST_DEBUG_CATEGORY_INIT(gst_projectm_debug,
                                                    "projectm", 0,
                                                    "Plugin Root"));

static void gst_projectm_timeline_entry_free(gpointer data) {
  GstProjectMTimelineEntry *entry = (GstProjectMTimelineEntry *)data;
  if (entry == NULL) {
    return;
  }

  g_clear_pointer(&entry->preset, g_free);
  g_clear_pointer(&entry->complexity, g_free);
  g_free(entry);
}

static gint gst_projectm_timeline_entry_compare(gconstpointer a,
                                                gconstpointer b) {
  /* g_ptr_array_sort passes pointers-to-pointers: each argument is the
     address of a slot in the GPtrArray, so we must dereference once to
     obtain the actual GstProjectMTimelineEntry*. */
  const GstProjectMTimelineEntry *left =
      *(const GstProjectMTimelineEntry *const *)a;
  const GstProjectMTimelineEntry *right =
      *(const GstProjectMTimelineEntry *const *)b;

  if (left->start_time < right->start_time) {
    return -1;
  }
  if (left->start_time > right->start_time) {
    return 1;
  }
  return 0;
}

static void gst_projectm_timeline_reset(GstProjectM *plugin) {
  GstProjectMPrivate *priv = plugin->priv;

  if (priv->timeline_entries != NULL) {
    g_ptr_array_set_size(priv->timeline_entries, 0);
  }

  priv->current_timeline_index = -1;
  priv->timeline_active = FALSE;
  priv->timeline_initialized = FALSE;

  if (priv->handle != NULL) {
    projectm_set_preset_locked(priv->handle, plugin->preset_locked);

    if (plugin->preset_duration > 0.0) {
      projectm_set_preset_duration(priv->handle, plugin->preset_duration);
    } else {
      projectm_set_preset_duration(priv->handle, 999999.0);
    }
  }
}

static gchar *gst_projectm_resolve_preset_path(GstProjectM *plugin,
                                               const gchar *preset_value) {
  if (preset_value == NULL || *preset_value == '\0') {
    return NULL;
  }

  if (g_path_is_absolute(preset_value)) {
    return g_strdup(preset_value);
  }

  if (plugin->preset_path != NULL) {
    return g_canonicalize_filename(preset_value, plugin->preset_path);
  }

  return g_strdup(preset_value);
}

static gint gst_projectm_timeline_find_target_index(GstProjectM *plugin,
                                                    gdouble elapsed_seconds) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!priv->timeline_entries || priv->timeline_entries->len == 0) {
    return -1;
  }

  gint len = (gint)priv->timeline_entries->len;
  gint current = priv->current_timeline_index;

  if (current >= 0 && current < len) {
    GstProjectMTimelineEntry *entry =
        g_ptr_array_index(priv->timeline_entries, (guint)current);

    if ((elapsed_seconds + GST_PROJECTM_TIMELINE_EPSILON) <
        entry->start_time) {
      current = -1;
    } else {
      gboolean before_next = TRUE;
      if (current + 1 < len) {
        GstProjectMTimelineEntry *next_entry =
            g_ptr_array_index(priv->timeline_entries, (guint)(current + 1));
        before_next =
            (elapsed_seconds + GST_PROJECTM_TIMELINE_EPSILON) <
            next_entry->start_time;
      }

      if ((elapsed_seconds <= entry->end_time + GST_PROJECTM_TIMELINE_EPSILON) ||
          before_next || current == len - 1) {
        return current;
      }
    }
  }

  gint low = 0;
  gint high = len - 1;
  gint result = -1;

  while (low <= high) {
    gint mid = low + ((high - low) / 2);
    GstProjectMTimelineEntry *entry =
        g_ptr_array_index(priv->timeline_entries, (guint)mid);

    if ((elapsed_seconds + GST_PROJECTM_TIMELINE_EPSILON) <
        entry->start_time) {
      high = mid - 1;
      continue;
    }

    result = mid;

    if (elapsed_seconds <= entry->end_time + GST_PROJECTM_TIMELINE_EPSILON) {
      break;
    }

    low = mid + 1;
  }

  return result;
}

static gboolean gst_projectm_load_timeline(GstProjectM *plugin,
                                           const gchar *path) {
  GstProjectMPrivate *priv = plugin->priv;

  gst_projectm_timeline_reset(plugin);

  if (path == NULL) {
    GST_DEBUG_OBJECT(plugin,
                     "Timeline path cleared; using internal preset switching");
    return FALSE;
  }

  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    GST_WARNING_OBJECT(plugin, "Timeline file not found: %s", path);
    return FALSE;
  }

  GKeyFile *key_file = g_key_file_new();
  GError *error = NULL;

  if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
    GST_WARNING_OBJECT(plugin, "Failed to parse timeline file %s: %s", path,
                       error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    g_key_file_free(key_file);
    return FALSE;
  }

  gsize group_count = 0;
  gchar **groups = g_key_file_get_groups(key_file, &group_count);

  if (groups == NULL || group_count == 0) {
    GST_WARNING_OBJECT(plugin, "Timeline file %s contains no segments", path);
    g_strfreev(groups);
    g_key_file_free(key_file);
    return FALSE;
  }

  for (gsize i = 0; i < group_count; i++) {
    const gchar *group = groups[i];
    gboolean segment_valid = TRUE;

    GError *value_error = NULL;
    gdouble start =
        g_key_file_get_double(key_file, group, "start", &value_error);
    if (value_error != NULL) {
      GST_WARNING_OBJECT(plugin,
                         "Timeline segment '%s' missing valid 'start': %s",
                         group, value_error->message);
      g_clear_error(&value_error);
      segment_valid = FALSE;
    }

    gdouble duration = 0.0;
    if (segment_valid) {
      duration =
          g_key_file_get_double(key_file, group, "duration", &value_error);
      if (value_error != NULL) {
        GST_WARNING_OBJECT(plugin,
                           "Timeline segment '%s' missing valid 'duration': "
                           "%s",
                           group, value_error->message);
        g_clear_error(&value_error);
        segment_valid = FALSE;
      } else if (duration <= 0.0) {
        GST_WARNING_OBJECT(plugin,
                           "Timeline segment '%s' has non-positive duration",
                           group);
        segment_valid = FALSE;
      }
    }

    gchar *preset = NULL;
    if (segment_valid) {
      preset = g_key_file_get_string(key_file, group, "preset", &value_error);
      if (value_error != NULL || preset == NULL || *preset == '\0') {
        GST_WARNING_OBJECT(plugin,
                           "Timeline segment '%s' missing valid 'preset'",
                           group);
        g_clear_error(&value_error);
        g_clear_pointer(&preset, g_free);
        segment_valid = FALSE;
      }
    }

    gchar *complexity = NULL;
    if (segment_valid) {
      complexity =
          g_key_file_get_string(key_file, group, "complexity", NULL);
      if (complexity != NULL && *complexity == '\0') {
        g_clear_pointer(&complexity, g_free);
      }
    }

    if (!segment_valid) {
      continue;
    }

    GstProjectMTimelineEntry *entry = g_new0(GstProjectMTimelineEntry, 1);
    entry->start_time = start;
    entry->duration = duration;
    entry->end_time = start + duration;
    entry->preset = preset;
    entry->complexity = complexity;

    g_ptr_array_add(priv->timeline_entries, entry);
  }

  g_strfreev(groups);
  g_key_file_free(key_file);

  if (priv->timeline_entries->len == 0) {
    GST_WARNING_OBJECT(plugin, "Timeline file %s did not yield any segments",
                       path);
    return FALSE;
  }

  g_ptr_array_sort(priv->timeline_entries,
                   (GCompareFunc)gst_projectm_timeline_entry_compare);

  priv->timeline_active = TRUE;
  priv->timeline_initialized = FALSE;
  priv->current_timeline_index = -1;

  GST_INFO_OBJECT(plugin, "Timeline ready with %u segments",
                   priv->timeline_entries->len);

  /* Diagnostic: print first 20 entries to verify sort order */
  for (guint di = 0; di < priv->timeline_entries->len && di < 20; di++) {
    GstProjectMTimelineEntry *de =
        g_ptr_array_index(priv->timeline_entries, di);
    GST_INFO_OBJECT(plugin,
                    "Timeline entry[%u]: start=%.3f duration=%.3f end=%.3f",
                    di, de->start_time, de->duration, de->end_time);
  }

  return TRUE;
}

static void gst_projectm_activate_timeline(GstProjectM *plugin) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!priv->timeline_active || priv->timeline_entries == NULL ||
      priv->timeline_entries->len == 0) {
    return;
  }

  if (plugin->preset_path == NULL) {
    gboolean requires_base = FALSE;
    for (guint i = 0; i < priv->timeline_entries->len; i++) {
      GstProjectMTimelineEntry *entry =
          g_ptr_array_index(priv->timeline_entries, i);
      if (entry->preset != NULL && !g_path_is_absolute(entry->preset)) {
        requires_base = TRUE;
        break;
      }
    }

    if (requires_base) {
      GST_WARNING_OBJECT(
          plugin,
          "Timeline contains relative preset paths but preset-path is unset; "
          "disabling timeline-driven switching");
      gst_projectm_timeline_reset(plugin);
      return;
    }
  }

  if (priv->handle != NULL) {
    projectm_set_preset_locked(priv->handle, TRUE);
    projectm_set_preset_duration(priv->handle, 999999.0);
  }

  /* If gst_projectm_load_first_timeline_preset already loaded the first preset,
   * keep current_timeline_index at 0 to avoid reloading with smooth transition.
   * This prevents the idle preset from appearing during transition animation. */
  if (priv->current_timeline_index != 0) {
    priv->current_timeline_index = -1;
    GST_DEBUG_OBJECT(plugin, "Timeline activated, will load first preset");
    gst_projectm_timeline_update(plugin, 0.0);
  } else {
    GST_DEBUG_OBJECT(plugin, "Timeline activated, first preset already loaded");
  }
  priv->timeline_initialized = TRUE;
}

static void gst_projectm_timeline_update(GstProjectM *plugin,
                                         gdouble elapsed_seconds) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!priv->timeline_active || priv->timeline_entries == NULL ||
      priv->timeline_entries->len == 0 || priv->handle == NULL) {
    return;
  }

  gint target_index =
      gst_projectm_timeline_find_target_index(plugin, elapsed_seconds);

  if (target_index < 0 ||
      target_index == priv->current_timeline_index ||
      target_index >= (gint)priv->timeline_entries->len) {
    return;
  }

  GstProjectMTimelineEntry *entry =
      g_ptr_array_index(priv->timeline_entries, (guint)target_index);
  gchar *resolved = gst_projectm_resolve_preset_path(plugin, entry->preset);

  if (resolved == NULL) {
    GST_WARNING_OBJECT(plugin,
                       "Unable to resolve preset path for timeline segment %d",
                       target_index);
    priv->current_timeline_index = target_index;
    return;
  }

  gboolean smooth_transition = TRUE;
  if (entry->complexity != NULL) {
    if (g_ascii_strcasecmp(entry->complexity, "high") == 0 ||
        g_ascii_strcasecmp(entry->complexity, "intense") == 0) {
      smooth_transition = FALSE;
    } else if (g_ascii_strcasecmp(entry->complexity, "ambient") == 0 ||
               g_ascii_strcasecmp(entry->complexity, "low") == 0) {
      smooth_transition = TRUE;
    }
  }

  GST_INFO_OBJECT(plugin,
                  "Timeline switch -> preset=%s index=%d start=%.2f duration=%.2f "
                  "elapsed=%.3f smooth=%d",
                  resolved, target_index, entry->start_time, entry->duration,
                  elapsed_seconds, smooth_transition);

  projectm_load_preset_file(priv->handle, resolved, smooth_transition);
  g_free(resolved);

  priv->current_timeline_index = target_index;
}

static void gst_projectm_copy_to_frame(GstVideoFrame *video, const guint8 *src,
                                       gsize width, gsize height) {
  guint8 *dest = (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(video, 0);
  gsize dest_stride = GST_VIDEO_FRAME_PLANE_STRIDE(video, 0);
  gsize row_size = width * 4;

  if (dest_stride == row_size) {
    memcpy(dest, src, row_size * height);
    return;
  }

  for (gsize y = 0; y < height; y++) {
    memcpy(dest + (y * dest_stride), src + (y * row_size), row_size);
  }
}

static gpointer gst_projectm_map_pbo(const GstGLFuncs *glFunctions, gsize size) {
  if (glFunctions->MapBufferRange) {
    return glFunctions->MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size,
                                       GL_MAP_READ_BIT);
  }

  if (glFunctions->MapBuffer) {
    return glFunctions->MapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
  }

  return NULL;
}

static void gst_projectm_unmap_pbo(const GstGLFuncs *glFunctions) {
  if (glFunctions->UnmapBuffer) {
    glFunctions->UnmapBuffer(GL_PIXEL_PACK_BUFFER);
  }
}

static gboolean gst_projectm_ensure_pbos(GstProjectM *plugin,
                                         const GstGLFuncs *glFunctions,
                                         gsize width, gsize height) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!glFunctions || !glFunctions->GenBuffers || !glFunctions->BindBuffer ||
      !glFunctions->BufferData) {
    return FALSE;
  }

  gsize row_size = width * 4;
  gsize required_size = row_size * height;

  if (priv->pbo_initialized && priv->pbo_size == required_size &&
      priv->pbo_width == width && priv->pbo_height == height) {
    return TRUE;
  }

  gst_projectm_release_pbos(plugin, glFunctions);

  glFunctions->GenBuffers(GST_PROJECTM_PBO_COUNT, priv->pbo_ids);
  for (guint i = 0; i < GST_PROJECTM_PBO_COUNT; i++) {
    glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, priv->pbo_ids[i]);
    glFunctions->BufferData(GL_PIXEL_PACK_BUFFER, required_size, NULL,
                            GL_STREAM_READ);
  }
  glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, 0);

  priv->pbo_initialized = TRUE;
  priv->pbo_width = width;
  priv->pbo_height = height;
  priv->pbo_size = required_size;
  priv->pbo_index = 0;
  priv->pbo_frame_valid = FALSE;

  return TRUE;
}

static void gst_projectm_release_pbos(GstProjectM *plugin,
                                      const GstGLFuncs *glFunctions) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!priv->pbo_initialized) {
    return;
  }

  if (glFunctions && glFunctions->DeleteBuffers) {
    glFunctions->DeleteBuffers(GST_PROJECTM_PBO_COUNT, priv->pbo_ids);
  }

  memset(priv->pbo_ids, 0, sizeof(priv->pbo_ids));
  priv->pbo_initialized = FALSE;
  priv->pbo_size = 0;
  priv->pbo_width = 0;
  priv->pbo_height = 0;
  priv->pbo_index = 0;
  priv->pbo_frame_valid = FALSE;
}

static gboolean
gst_projectm_check_headless_mode(GstProjectM *plugin,
                                  const GstGLFuncs *glFunctions) {
  GstProjectMPrivate *priv = plugin->priv;

  if (priv->headless_checked) {
    return priv->headless_mode;
  }

  priv->headless_checked = TRUE;
  priv->headless_mode = FALSE;

  /* Check for environment variable to force FBO/headless mode */
  const gchar *force_fbo = g_getenv("GST_PROJECTM_FORCE_FBO");
  if (force_fbo && (g_ascii_strcasecmp(force_fbo, "1") == 0 ||
                    g_ascii_strcasecmp(force_fbo, "true") == 0 ||
                    g_ascii_strcasecmp(force_fbo, "yes") == 0)) {
    priv->headless_mode = TRUE;
    GST_INFO_OBJECT(plugin,
                    "FBO mode forced via GST_PROJECTM_FORCE_FBO environment variable");
    return TRUE;
  }

  if (!glFunctions || !glFunctions->CheckFramebufferStatus ||
      !glFunctions->BindFramebuffer) {
    return FALSE;
  }

  /* Save current framebuffer binding */
  GLint current_fbo = 0;
  if (glFunctions->GetIntegerv) {
    glFunctions->GetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);
  }

  /* Bind default framebuffer (0) and check its status */
  glFunctions->BindFramebuffer(GL_FRAMEBUFFER, 0);
  GLenum status = glFunctions->CheckFramebufferStatus(GL_FRAMEBUFFER);

  /* Restore previous binding */
  glFunctions->BindFramebuffer(GL_FRAMEBUFFER, (GLuint)current_fbo);

  /* In headless mode, framebuffer 0 will be incomplete or undefined */
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    priv->headless_mode = TRUE;
    GST_INFO_OBJECT(plugin,
                    "Detected headless mode (default framebuffer status=0x%x); "
                    "FBO rendering required",
                    status);
  } else {
    GST_DEBUG_OBJECT(plugin,
                     "Default framebuffer available (status=0x%x); "
                     "FBO optional",
                     status);
  }

  return priv->headless_mode;
}

static gboolean
gst_projectm_has_rt_support(GstProjectM *plugin,
                            const GstGLFuncs *glFunctions) {
#define GST_PROJECTM_REQUIRE_GL_FUNC(func)                                     \
  if (!glFunctions || !glFunctions->func) {                                   \
    if (!plugin->priv->fbo_warned_missing_support) {                          \
      GST_WARNING_OBJECT(plugin,                                              \
                         "GL function %s is unavailable; falling back to "    \
                         "default framebuffer",                               \
                         #func);                                              \
      plugin->priv->fbo_warned_missing_support = TRUE;                        \
    }                                                                         \
    return FALSE;                                                             \
  }

  GST_PROJECTM_REQUIRE_GL_FUNC(GenFramebuffers);
  GST_PROJECTM_REQUIRE_GL_FUNC(DeleteFramebuffers);
  GST_PROJECTM_REQUIRE_GL_FUNC(BindFramebuffer);
  GST_PROJECTM_REQUIRE_GL_FUNC(FramebufferTexture2D);
  GST_PROJECTM_REQUIRE_GL_FUNC(GenTextures);
  GST_PROJECTM_REQUIRE_GL_FUNC(DeleteTextures);
  GST_PROJECTM_REQUIRE_GL_FUNC(BindTexture);
  GST_PROJECTM_REQUIRE_GL_FUNC(TexImage2D);
  GST_PROJECTM_REQUIRE_GL_FUNC(TexParameteri);

#undef GST_PROJECTM_REQUIRE_GL_FUNC
  return TRUE;
}

static gboolean gst_projectm_ensure_render_target(GstProjectM *plugin,
                                                  const GstGLFuncs *glFunctions,
                                                  gsize width, gsize height) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!gst_projectm_has_rt_support(plugin, glFunctions)) {
    return FALSE;
  }

  if (priv->fbo_initialized && priv->fbo_width == width &&
      priv->fbo_height == height) {
    /* Ensure FBO is bound - it might have been unbound by something else */
    if (glFunctions->BindFramebuffer) {
      glFunctions->BindFramebuffer(GL_FRAMEBUFFER, priv->fbo_id);
    }
    return TRUE;
  }

  /* Save old FBO info - we'll delete after new one is bound to avoid
   * ever binding framebuffer 0 in headless mode */
  GLuint old_fbo = priv->fbo_id;
  GLuint old_tex = priv->fbo_texture_id;
  GLuint old_depth = priv->fbo_depth_buffer_id;
  gboolean had_old = priv->fbo_initialized;

  /* Clear the state - we'll set new values below */
  priv->fbo_id = 0;
  priv->fbo_texture_id = 0;
  priv->fbo_depth_buffer_id = 0;
  priv->fbo_initialized = FALSE;

  GLuint new_fbo = 0;
  GLuint new_tex = 0;
  GLuint new_depth = 0;
  glFunctions->GenFramebuffers(1, &new_fbo);
  glFunctions->GenTextures(1, &new_tex);

  glFunctions->BindTexture(GL_TEXTURE_2D, new_tex);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                             GL_CLAMP_TO_EDGE);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                             GL_CLAMP_TO_EDGE);
  glFunctions->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width,
                          (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glFunctions->BindTexture(GL_TEXTURE_2D, 0);

  glFunctions->BindFramebuffer(GL_FRAMEBUFFER, new_fbo);
  glFunctions->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, new_tex, 0);

  if (glFunctions->DrawBuffers) {
    GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glFunctions->DrawBuffers(1, &draw_buffer);
  } else if (glFunctions->DrawBuffer) {
    glFunctions->DrawBuffer(GL_COLOR_ATTACHMENT0);
  }
  if (glFunctions->ReadBuffer) {
    glFunctions->ReadBuffer(GL_COLOR_ATTACHMENT0);
  }

  if (glFunctions->GenRenderbuffers && glFunctions->DeleteRenderbuffers &&
      glFunctions->BindRenderbuffer && glFunctions->RenderbufferStorage &&
      glFunctions->FramebufferRenderbuffer) {
    glFunctions->GenRenderbuffers(1, &new_depth);
    glFunctions->BindRenderbuffer(GL_RENDERBUFFER, new_depth);
#ifdef GL_DEPTH24_STENCIL8
    glFunctions->RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                                     (GLsizei)width, (GLsizei)height);
    glFunctions->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                         GL_RENDERBUFFER, new_depth);
    glFunctions->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                         GL_RENDERBUFFER, new_depth);
#else
    glFunctions->RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                                     (GLsizei)width, (GLsizei)height);
    glFunctions->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                         GL_RENDERBUFFER, new_depth);
#endif
    glFunctions->BindRenderbuffer(GL_RENDERBUFFER, 0);
  } else if (!priv->fbo_warned_missing_support) {
    GST_DEBUG_OBJECT(plugin,
                     "Renderbuffer functions unavailable; continuing without "
                     "depth attachment");
    priv->fbo_warned_missing_support = TRUE;
  }

  gboolean success = TRUE;
  if (glFunctions->CheckFramebufferStatus) {
    GLenum status = glFunctions->CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      GST_ERROR_OBJECT(plugin,
                       "Failed to build framebuffer for headless rendering "
                       "(status=0x%x)",
                       status);
      success = FALSE;
    }
  }

  if (!success) {
    /* Clean up the new FBO that failed */
    glFunctions->BindFramebuffer(GL_FRAMEBUFFER, 0);
    glFunctions->DeleteFramebuffers(1, &new_fbo);
    glFunctions->DeleteTextures(1, &new_tex);
    if (new_depth != 0 && glFunctions->DeleteRenderbuffers) {
      glFunctions->DeleteRenderbuffers(1, &new_depth);
    }
    /* Also clean up old resources if any */
    if (had_old) {
      if (old_fbo != 0 && glFunctions->DeleteFramebuffers) {
        glFunctions->DeleteFramebuffers(1, &old_fbo);
      }
      if (old_tex != 0 && glFunctions->DeleteTextures) {
        glFunctions->DeleteTextures(1, &old_tex);
      }
      if (old_depth != 0 && glFunctions->DeleteRenderbuffers) {
        glFunctions->DeleteRenderbuffers(1, &old_depth);
      }
    }
    return FALSE;
  }

  priv->fbo_id = new_fbo;
  priv->fbo_texture_id = new_tex;
  priv->fbo_depth_buffer_id = new_depth;
  priv->fbo_width = width;
  priv->fbo_height = height;
  priv->fbo_initialized = TRUE;

  /* Now delete the old FBO resources (after new FBO is bound and initialized).
   * This ensures we never have framebuffer 0 bound in headless mode. */
  if (had_old) {
    if (old_fbo != 0 && glFunctions->DeleteFramebuffers) {
      glFunctions->DeleteFramebuffers(1, &old_fbo);
    }
    if (old_tex != 0 && glFunctions->DeleteTextures) {
      glFunctions->DeleteTextures(1, &old_tex);
    }
    if (old_depth != 0 && glFunctions->DeleteRenderbuffers) {
      glFunctions->DeleteRenderbuffers(1, &old_depth);
    }
    GST_DEBUG_OBJECT(plugin, "Deleted old FBO %u", old_fbo);
  }

  /* Keep the FBO bound - do NOT unbind to framebuffer 0.
   * In headless EGL modes, framebuffer 0 doesn't exist. */
  GST_DEBUG_OBJECT(plugin, "Created FBO %u (%zux%zu) and keeping it bound",
                   new_fbo, width, height);

  return TRUE;
}

static void gst_projectm_release_render_target(GstProjectM *plugin,
                                               const GstGLFuncs *glFunctions) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!priv->fbo_initialized) {
    return;
  }

  if (glFunctions && glFunctions->DeleteFramebuffers && priv->fbo_id != 0) {
    glFunctions->DeleteFramebuffers(1, &priv->fbo_id);
  }
  if (glFunctions && glFunctions->DeleteTextures && priv->fbo_texture_id != 0) {
    glFunctions->DeleteTextures(1, &priv->fbo_texture_id);
  }
  if (glFunctions && glFunctions->DeleteRenderbuffers &&
      priv->fbo_depth_buffer_id != 0) {
    glFunctions->DeleteRenderbuffers(1, &priv->fbo_depth_buffer_id);
  }

  priv->fbo_id = 0;
  priv->fbo_texture_id = 0;
  priv->fbo_depth_buffer_id = 0;
  priv->fbo_width = 0;
  priv->fbo_height = 0;
  priv->fbo_initialized = FALSE;
  priv->fbo_warned_missing_support = FALSE;
}

static gboolean gst_projectm_download_frame_with_pbo(
    GstProjectM *plugin, const GstGLFuncs *glFunctions, GstVideoFrame *video,
    gsize width, gsize height) {
  GstProjectMPrivate *priv = plugin->priv;

  if (!priv->pbo_initialized || !glFunctions || !glFunctions->BindBuffer) {
    return FALSE;
  }

  guint next_index = (priv->pbo_index + 1) % GST_PROJECTM_PBO_COUNT;
  GLuint next_pbo = priv->pbo_ids[next_index];

  glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, next_pbo);
  glFunctions->ReadPixels(0, 0, width, height, priv->gl_format,
                          GL_UNSIGNED_INT_8_8_8_8, 0);
  glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, 0);

  gboolean copied = FALSE;

  if (priv->pbo_frame_valid) {
    GLuint ready_pbo = priv->pbo_ids[priv->pbo_index];
    glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, ready_pbo);
    guint8 *mapped = (guint8 *)gst_projectm_map_pbo(glFunctions, priv->pbo_size);
    if (mapped != NULL) {
      gst_projectm_copy_to_frame(video, mapped, width, height);
      copied = TRUE;
      gst_projectm_unmap_pbo(glFunctions);
    }
    glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }

  priv->pbo_index = next_index;
  priv->pbo_frame_valid = TRUE;

  if (!copied) {
    glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, next_pbo);
    guint8 *mapped = (guint8 *)gst_projectm_map_pbo(glFunctions, priv->pbo_size);
    if (mapped != NULL) {
      gst_projectm_copy_to_frame(video, mapped, width, height);
      copied = TRUE;
      gst_projectm_unmap_pbo(glFunctions);
    }
    glFunctions->BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }

  return copied;
}

/* ═══════════════════════════════════════════════════════════════════
 * NV12 output path
 *
 * When the negotiated downstream format is NV12, the plugin runs two
 * GLSL fragment-shader passes against the projectm RGBA FBO instead of
 * doing CPU-side colour conversion downstream:
 *
 *   1. Y pass  — full-resolution: writes BT.601 luma into a R8 texture
 *   2. UV pass — half-resolution: writes interleaved U/V into a RG8
 *                texture (linear-filtered sampling averages 2x2 RGB
 *                blocks, giving 4:2:0 chroma subsampling for free)
 *
 * ReadPixels then pulls each plane directly into the GstVideoFrame's
 * NV12 plane data. This eliminates the downstream "videoconvert
 * ABGR→NV12" CPU step which dominated render time on Apple Silicon.
 *
 * Shader uses `#version 150` core profile syntax (works on macOS GL3.2
 * core context, which is what Tauri/Cocoa exposes). If we ever target
 * GLES2 we'll need a second source string.
 * ═══════════════════════════════════════════════════════════════════ */

static const char *NV12_VERTEX_SHADER =
    "#version 150 core\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

/* BT.601 STUDIO-SWING (limited-range) coefficients:
 *   Y  in [16, 235]
 *   UV in [16, 240]
 * vtenc_h264 expects studio-swing input by default, matching the
 * `color_range=tv` we observe in ffprobe output. Do NOT swap to
 * full-range (`pc`) coefficients without also signalling it on the
 * encoder, otherwise downstream players will misinterpret luma. */
static const char *NV12_FRAGMENT_SHADER =
    "#version 150 core\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_pass;\n"  /* 0=Y, 1=UV */
    "in  vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec4 c = texture(u_tex, v_uv);\n"
    "  if (u_pass == 0) {\n"
    "    float y = 0.257*c.r + 0.504*c.g + 0.098*c.b + 16.0/255.0;\n"
    "    fragColor = vec4(y, 0.0, 0.0, 1.0);\n"
    "  } else {\n"
    "    float u = -0.148*c.r - 0.291*c.g + 0.439*c.b + 128.0/255.0;\n"
    "    float v =  0.439*c.r - 0.368*c.g - 0.071*c.b + 128.0/255.0;\n"
    "    fragColor = vec4(u, v, 0.0, 1.0);\n"
    "  }\n"
    "}\n";

/* Fullscreen quad geometry: 4 verts, interleaved (pos.xy, uv.xy).
 * Two triangles via GL_TRIANGLE_STRIP. */
static const GLfloat NV12_QUAD_VERTS[] = {
    /* x,    y,    u,   v   */
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

static GLuint gst_projectm_nv12_compile_shader(GstProjectM *plugin,
                                               const GstGLFuncs *glFunctions,
                                               GLenum type,
                                               const char *source) {
  GLuint shader = glFunctions->CreateShader(type);
  if (shader == 0) {
    GST_ERROR_OBJECT(plugin, "NV12: CreateShader returned 0");
    return 0;
  }
  glFunctions->ShaderSource(shader, 1, &source, NULL);
  glFunctions->CompileShader(shader);

  GLint status = GL_FALSE;
  glFunctions->GetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_FALSE) {
    char log[1024];
    GLsizei len = 0;
    if (glFunctions->GetShaderInfoLog) {
      glFunctions->GetShaderInfoLog(shader, sizeof(log) - 1, &len, log);
      log[len] = '\0';
    } else {
      log[0] = '\0';
    }
    GST_ERROR_OBJECT(plugin, "NV12: shader compile failed: %s", log);
    glFunctions->DeleteShader(shader);
    return 0;
  }
  return shader;
}

static gboolean gst_projectm_nv12_init(GstProjectM *plugin,
                                       const GstGLFuncs *glFunctions,
                                       gsize width, gsize height) {
  GstProjectMPrivate *priv = plugin->priv;

  /* If already initialized at the right size, nothing to do */
  if (priv->nv12_initialized &&
      priv->nv12_width == width &&
      priv->nv12_height == height) {
    return TRUE;
  }

  /* Resize: tear down and rebuild textures/FBOs but keep shader program */
  if (priv->nv12_initialized) {
    if (priv->nv12_y_fbo) glFunctions->DeleteFramebuffers(1, &priv->nv12_y_fbo);
    if (priv->nv12_uv_fbo) glFunctions->DeleteFramebuffers(1, &priv->nv12_uv_fbo);
    if (priv->nv12_y_tex) glFunctions->DeleteTextures(1, &priv->nv12_y_tex);
    if (priv->nv12_uv_tex) glFunctions->DeleteTextures(1, &priv->nv12_uv_tex);
    priv->nv12_y_fbo = priv->nv12_uv_fbo = 0;
    priv->nv12_y_tex = priv->nv12_uv_tex = 0;
  }

  /* Compile shader program once. Persists across resizes. */
  if (priv->nv12_program == 0) {
    GLuint vs = gst_projectm_nv12_compile_shader(plugin, glFunctions,
                                                  GL_VERTEX_SHADER,
                                                  NV12_VERTEX_SHADER);
    GLuint fs = gst_projectm_nv12_compile_shader(plugin, glFunctions,
                                                  GL_FRAGMENT_SHADER,
                                                  NV12_FRAGMENT_SHADER);
    if (vs == 0 || fs == 0) {
      if (vs) glFunctions->DeleteShader(vs);
      if (fs) glFunctions->DeleteShader(fs);
      return FALSE;
    }

    GLuint prog = glFunctions->CreateProgram();
    glFunctions->AttachShader(prog, vs);
    glFunctions->AttachShader(prog, fs);
    glFunctions->LinkProgram(prog);

    GLint linked = GL_FALSE;
    glFunctions->GetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
      char log[1024];
      GLsizei len = 0;
      if (glFunctions->GetProgramInfoLog) {
        glFunctions->GetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
        log[len] = '\0';
      } else {
        log[0] = '\0';
      }
      GST_ERROR_OBJECT(plugin, "NV12: program link failed: %s", log);
      glFunctions->DeleteShader(vs);
      glFunctions->DeleteShader(fs);
      glFunctions->DeleteProgram(prog);
      return FALSE;
    }
    glFunctions->DeleteShader(vs);
    glFunctions->DeleteShader(fs);

    priv->nv12_program       = prog;
    priv->nv12_uniform_tex   = glFunctions->GetUniformLocation(prog, "u_tex");
    priv->nv12_uniform_pass  = glFunctions->GetUniformLocation(prog, "u_pass");
    priv->nv12_attrib_pos    = glFunctions->GetAttribLocation(prog, "a_pos");
    priv->nv12_attrib_uv     = glFunctions->GetAttribLocation(prog, "a_uv");
  }

  /* Quad VBO + VAO — also persist. GL 3.2 core profile mandates a bound
   * VAO before any vertex attribute call; without it every draw fails
   * with GL_INVALID_OPERATION. We bake the attribute pointer setup into
   * the VAO once so each frame's draw pass is just bind+draw. */
  if (priv->nv12_vbo == 0) {
    glFunctions->GenBuffers(1, &priv->nv12_vbo);
    glFunctions->BindBuffer(GL_ARRAY_BUFFER, priv->nv12_vbo);
    glFunctions->BufferData(GL_ARRAY_BUFFER, sizeof(NV12_QUAD_VERTS),
                            NV12_QUAD_VERTS, GL_STATIC_DRAW);
    glFunctions->BindBuffer(GL_ARRAY_BUFFER, 0);
  }
  if (priv->nv12_vao == 0 && glFunctions->GenVertexArrays) {
    glFunctions->GenVertexArrays(1, &priv->nv12_vao);
    glFunctions->BindVertexArray(priv->nv12_vao);
    glFunctions->BindBuffer(GL_ARRAY_BUFFER, priv->nv12_vbo);
    if (priv->nv12_attrib_pos >= 0) {
      glFunctions->EnableVertexAttribArray((GLuint)priv->nv12_attrib_pos);
      glFunctions->VertexAttribPointer((GLuint)priv->nv12_attrib_pos, 2,
                                       GL_FLOAT, GL_FALSE,
                                       4 * sizeof(GLfloat), (void *)0);
    }
    if (priv->nv12_attrib_uv >= 0) {
      glFunctions->EnableVertexAttribArray((GLuint)priv->nv12_attrib_uv);
      glFunctions->VertexAttribPointer((GLuint)priv->nv12_attrib_uv, 2,
                                       GL_FLOAT, GL_FALSE,
                                       4 * sizeof(GLfloat),
                                       (void *)(2 * sizeof(GLfloat)));
    }
    glFunctions->BindVertexArray(0);
    glFunctions->BindBuffer(GL_ARRAY_BUFFER, 0);
  }

  /* Y plane FBO: full WxH, single channel R8.
   * Note: filter set to GL_LINEAR so the source→Y shader pass can bilinear-
   * sample the RGBA source. (Y uses full-res so no scaling; LINEAR is just
   * a safer default in case of non-integer viewport ratios later.) */
  glFunctions->GenTextures(1, &priv->nv12_y_tex);
  glFunctions->BindTexture(GL_TEXTURE_2D, priv->nv12_y_tex);
  glFunctions->TexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                          (GLsizei)width, (GLsizei)height,
                          0, GL_RED, GL_UNSIGNED_BYTE, NULL);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFunctions->GenFramebuffers(1, &priv->nv12_y_fbo);
  glFunctions->BindFramebuffer(GL_FRAMEBUFFER, priv->nv12_y_fbo);
  glFunctions->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, priv->nv12_y_tex, 0);
  if (glFunctions->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    GST_ERROR_OBJECT(plugin, "NV12: Y FBO incomplete (likely R8 unsupported)");
    /* Clean up partial allocations before returning so a later re-init
     * (e.g. at a different resolution) doesn't leak texture/FBO objects. */
    glFunctions->DeleteFramebuffers(1, &priv->nv12_y_fbo);
    glFunctions->DeleteTextures(1, &priv->nv12_y_tex);
    priv->nv12_y_fbo = 0;
    priv->nv12_y_tex = 0;
    return FALSE;
  }

  /* UV plane FBO: half W/2 x H/2, two channels RG8.
   * Filter set to GL_LINEAR so the viewport-scaled quad draw (rendering
   * source WxH into a W/2 x H/2 destination) automatically averages 2x2
   * source blocks via hardware bilinear — this is the 4:2:0 chroma
   * subsampling. No per-frame TexParameteri needed on the source texture. */
  glFunctions->GenTextures(1, &priv->nv12_uv_tex);
  glFunctions->BindTexture(GL_TEXTURE_2D, priv->nv12_uv_tex);
  glFunctions->TexImage2D(GL_TEXTURE_2D, 0, GL_RG8,
                          (GLsizei)(width / 2), (GLsizei)(height / 2),
                          0, GL_RG, GL_UNSIGNED_BYTE, NULL);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glFunctions->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFunctions->GenFramebuffers(1, &priv->nv12_uv_fbo);
  glFunctions->BindFramebuffer(GL_FRAMEBUFFER, priv->nv12_uv_fbo);
  glFunctions->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, priv->nv12_uv_tex, 0);
  if (glFunctions->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    GST_ERROR_OBJECT(plugin, "NV12: UV FBO incomplete (likely RG8 unsupported)");
    glFunctions->DeleteFramebuffers(1, &priv->nv12_y_fbo);
    glFunctions->DeleteFramebuffers(1, &priv->nv12_uv_fbo);
    glFunctions->DeleteTextures(1, &priv->nv12_y_tex);
    glFunctions->DeleteTextures(1, &priv->nv12_uv_tex);
    priv->nv12_y_fbo = priv->nv12_uv_fbo = 0;
    priv->nv12_y_tex = priv->nv12_uv_tex = 0;
    return FALSE;
  }

  /* Restore projectm's render-target binding. Mirrors the ABGR path's
   * "never unbind to framebuffer 0 in headless mode" rule — fbo 0 doesn't
   * exist under headless EGL. */
  if (priv->fbo_id != 0) {
    glFunctions->BindFramebuffer(GL_FRAMEBUFFER, priv->fbo_id);
  } else {
    glFunctions->BindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  glFunctions->BindTexture(GL_TEXTURE_2D, 0);

  priv->nv12_width = width;
  priv->nv12_height = height;
  priv->nv12_initialized = TRUE;

  GST_INFO_OBJECT(plugin, "NV12 output initialized at %zux%zu (Y=R8, UV=RG8 half-res)",
                  width, height);
  return TRUE;
}

static void gst_projectm_nv12_release(GstProjectM *plugin,
                                      const GstGLFuncs *glFunctions) {
  GstProjectMPrivate *priv = plugin->priv;
  /* Skip the early-exit guard. We touched too many independent resources
   * (program, VBO, VAO, two FBOs, two textures) for any single flag to be
   * a reliable signal — if init failed midway through, only some are
   * non-zero. The per-resource null checks below handle every case. */

  if (glFunctions) {
    if (priv->nv12_y_fbo)  glFunctions->DeleteFramebuffers(1, &priv->nv12_y_fbo);
    if (priv->nv12_uv_fbo) glFunctions->DeleteFramebuffers(1, &priv->nv12_uv_fbo);
    if (priv->nv12_y_tex)  glFunctions->DeleteTextures(1, &priv->nv12_y_tex);
    if (priv->nv12_uv_tex) glFunctions->DeleteTextures(1, &priv->nv12_uv_tex);
    if (priv->nv12_vbo)    glFunctions->DeleteBuffers(1, &priv->nv12_vbo);
    if (priv->nv12_vao && glFunctions->DeleteVertexArrays)
      glFunctions->DeleteVertexArrays(1, &priv->nv12_vao);
    if (priv->nv12_program) glFunctions->DeleteProgram(priv->nv12_program);
  }

  priv->nv12_y_fbo = priv->nv12_uv_fbo = 0;
  priv->nv12_y_tex = priv->nv12_uv_tex = 0;
  priv->nv12_vbo = 0;
  priv->nv12_vao = 0;
  priv->nv12_program = 0;
  priv->nv12_initialized = FALSE;
  priv->nv12_width = priv->nv12_height = 0;
}

/* Run a single full-screen quad pass with the NV12 program.
 * Caller must have bound the destination FBO + set viewport before calling.
 * VAO carries the vertex attribute setup so we just bind + draw.
 *
 * Note: we deliberately do NOT mutate the source texture's filter mode
 * here. The 4:2:0 chroma subsampling for the UV pass comes from rendering
 * the source-sized quad into a half-sized viewport — bilinear sampling on
 * the source texture (set once during FBO creation) averages 2x2 RGB
 * blocks naturally during rasterisation. Mutating the source filter
 * per-frame would also affect the ABGR PBO readback path. */
static void gst_projectm_nv12_draw_pass(GstProjectM *plugin,
                                        const GstGLFuncs *glFunctions,
                                        int pass) {
  GstProjectMPrivate *priv = plugin->priv;

  glFunctions->UseProgram(priv->nv12_program);
  glFunctions->Uniform1i(priv->nv12_uniform_tex, 0);     /* texture unit 0 */
  glFunctions->Uniform1i(priv->nv12_uniform_pass, pass);

  glFunctions->ActiveTexture(GL_TEXTURE0);
  glFunctions->BindTexture(GL_TEXTURE_2D, priv->fbo_texture_id);

  if (priv->nv12_vao && glFunctions->BindVertexArray) {
    glFunctions->BindVertexArray(priv->nv12_vao);
  }
  glFunctions->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if (priv->nv12_vao && glFunctions->BindVertexArray) {
    glFunctions->BindVertexArray(0);
  }

  glFunctions->BindTexture(GL_TEXTURE_2D, 0);
  glFunctions->UseProgram(0);
}

static gboolean gst_projectm_nv12_render(GstProjectM *plugin,
                                         const GstGLFuncs *glFunctions,
                                         GstVideoFrame *video,
                                         gsize width, gsize height) {
  if (!gst_projectm_nv12_init(plugin, glFunctions, width, height)) {
    return FALSE;
  }
  GstProjectMPrivate *priv = plugin->priv;

  /* Save current viewport and disable depth/blend that projectm may have left on */
  GLint prev_viewport[4] = {0, 0, 0, 0};
  if (glFunctions->GetIntegerv) glFunctions->GetIntegerv(GL_VIEWPORT, prev_viewport);
  if (glFunctions->Disable) {
    glFunctions->Disable(GL_DEPTH_TEST);
    glFunctions->Disable(GL_BLEND);
    glFunctions->Disable(GL_CULL_FACE);
  }

  /* Y pass: full-resolution into nv12_y_fbo */
  glFunctions->BindFramebuffer(GL_FRAMEBUFFER, priv->nv12_y_fbo);
  glFunctions->Viewport(0, 0, (GLsizei)width, (GLsizei)height);
  gst_projectm_nv12_draw_pass(plugin, glFunctions, 0);

  /* ReadPixels Y plane directly into the GstVideoFrame's plane 0.
   * NV12 plane 0 stride may be > width (alignment padding) — we use
   * GL_PACK_ROW_LENGTH so the GL writes match GStreamer's stride.
   * GL_PACK_ROW_LENGTH is in PIXELS (not bytes). For R8 the values are
   * identical, but we compute explicitly so the unit is obvious.
   * GL_PACK_ALIGNMENT=1 avoids GL padding rows to 4-byte boundaries. */
  GLint y_stride_bytes = GST_VIDEO_FRAME_PLANE_STRIDE(video, 0);
  GLint y_stride_pixels = y_stride_bytes;        /* R8: 1 byte/pixel */
  if (glFunctions->PixelStorei) {
    glFunctions->PixelStorei(GL_PACK_ALIGNMENT, 1);
    glFunctions->PixelStorei(GL_PACK_ROW_LENGTH, y_stride_pixels);
  }
  glFunctions->ReadPixels(0, 0, (GLsizei)width, (GLsizei)height,
                          GL_RED, GL_UNSIGNED_BYTE,
                          (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(video, 0));

  /* UV pass: half-resolution into nv12_uv_fbo */
  glFunctions->BindFramebuffer(GL_FRAMEBUFFER, priv->nv12_uv_fbo);
  glFunctions->Viewport(0, 0, (GLsizei)(width / 2), (GLsizei)(height / 2));
  gst_projectm_nv12_draw_pass(plugin, glFunctions, 1);

  /* ReadPixels UV plane (interleaved RG = U,V,U,V…) into plane 1.
   * GL_PACK_ROW_LENGTH is in PIXELS; each UV texel is RG (2 bytes) so
   * pixels = stride_bytes / 2. */
  GLint uv_stride_bytes = GST_VIDEO_FRAME_PLANE_STRIDE(video, 1);
  GLint uv_stride_pixels = uv_stride_bytes / 2;  /* RG8: 2 bytes/pixel */
  if (glFunctions->PixelStorei) {
    glFunctions->PixelStorei(GL_PACK_ROW_LENGTH, uv_stride_pixels);
  }
  glFunctions->ReadPixels(0, 0, (GLsizei)(width / 2), (GLsizei)(height / 2),
                          GL_RG, GL_UNSIGNED_BYTE,
                          (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(video, 1));

  /* Reset pack state so other code in this GL context gets defaults back */
  if (glFunctions->PixelStorei) {
    glFunctions->PixelStorei(GL_PACK_ROW_LENGTH, 0);
    glFunctions->PixelStorei(GL_PACK_ALIGNMENT, 4);  /* GL default */
  }
  /* Restore projectm's render-target binding, not framebuffer 0. Under
   * headless EGL (production GPU pods) framebuffer 0 doesn't exist and
   * binding it raises GL_INVALID_OPERATION on the next draw — same rule
   * the ABGR path already follows in its FBO guard. */
  if (priv->fbo_id != 0) {
    glFunctions->BindFramebuffer(GL_FRAMEBUFFER, priv->fbo_id);
  } else {
    glFunctions->BindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  if (glFunctions->Viewport) {
    glFunctions->Viewport(prev_viewport[0], prev_viewport[1],
                          prev_viewport[2], prev_viewport[3]);
  }
  return TRUE;
}

gboolean gst_projectm_timeline_is_active(GstProjectM *plugin) {
  if (plugin == NULL) {
    return FALSE;
  }

  GstProjectMPrivate *priv = plugin->priv;
  return priv->timeline_active && priv->timeline_entries != NULL &&
         priv->timeline_entries->len > 0;
}

void gst_projectm_load_first_timeline_preset(GstProjectM *plugin, projectm_handle handle) {
  if (plugin == NULL || handle == NULL) {
    return;
  }

  GstProjectMPrivate *priv = plugin->priv;
  if (!priv->timeline_active || priv->timeline_entries == NULL ||
      priv->timeline_entries->len == 0) {
    return;
  }

  // Get the first timeline entry
  GstProjectMTimelineEntry *entry =
      g_ptr_array_index(priv->timeline_entries, 0);
  if (entry == NULL || entry->preset == NULL) {
    return;
  }

  // Resolve the preset path
  gchar *resolved = gst_projectm_resolve_preset_path(plugin, entry->preset);
  if (resolved == NULL) {
    GST_WARNING_OBJECT(plugin,
                       "Unable to resolve first timeline preset path: %s",
                       entry->preset);
    return;
  }

  GST_INFO_OBJECT(plugin,
                  "Loading first timeline preset immediately to avoid idle screen: %s",
                  resolved);

  // Load the preset with immediate (non-smooth) transition to avoid blending with idle
  projectm_load_preset_file(handle, resolved, FALSE);
  g_free(resolved);

  // Mark that we're at timeline index 0
  priv->current_timeline_index = 0;
}

void gst_projectm_set_property(GObject *object, guint property_id,
                               const GValue *value, GParamSpec *pspec) {
  GstProjectM *plugin = GST_PROJECTM(object);

  const gchar *property_name = g_param_spec_get_name(pspec);
  GST_DEBUG_OBJECT(plugin, "set-property <%s>", property_name);

  switch (property_id) {
  case PROP_PRESET_PATH:
    plugin->preset_path = g_strdup(g_value_get_string(value));
    break;
  case PROP_TEXTURE_DIR_PATH:
    plugin->texture_dir_path = g_strdup(g_value_get_string(value));
    break;
  case PROP_BEAT_SENSITIVITY:
    plugin->beat_sensitivity = g_value_get_float(value);
    break;
  case PROP_HARD_CUT_DURATION:
    plugin->hard_cut_duration = g_value_get_double(value);
    break;
  case PROP_HARD_CUT_ENABLED:
    plugin->hard_cut_enabled = g_value_get_boolean(value);
    break;
  case PROP_HARD_CUT_SENSITIVITY:
    plugin->hard_cut_sensitivity = g_value_get_float(value);
    break;
  case PROP_SOFT_CUT_DURATION:
    plugin->soft_cut_duration = g_value_get_double(value);
    break;
  case PROP_PRESET_DURATION:
    plugin->preset_duration = g_value_get_double(value);
    break;
  case PROP_MESH_SIZE: {
    const gchar *meshSizeStr = g_value_get_string(value);
    gint width, height;

    gchar **parts = g_strsplit(meshSizeStr, ",", 2);

    if (parts && g_strv_length(parts) == 2) {
      width = atoi(parts[0]);
      height = atoi(parts[1]);

      plugin->mesh_width = width;
      plugin->mesh_height = height;

      g_strfreev(parts);
    }
  } break;
  case PROP_ASPECT_CORRECTION:
    plugin->aspect_correction = g_value_get_boolean(value);
    break;
  case PROP_EASTER_EGG:
    plugin->easter_egg = g_value_get_float(value);
    break;
  case PROP_PRESET_LOCKED:
    plugin->preset_locked = g_value_get_boolean(value);
    break;
  case PROP_TIMELINE_PATH: {
    gchar *new_path = g_value_dup_string(value);

    if (new_path && *new_path == '\0') {
      g_free(new_path);
      new_path = NULL;
    }

    g_free(plugin->timeline_path);
    plugin->timeline_path = new_path;

    if (gst_projectm_load_timeline(plugin, plugin->timeline_path)) {
      if (plugin->priv->handle != NULL) {
        gst_projectm_activate_timeline(plugin);
      }
      GST_INFO_OBJECT(plugin, "Loaded timeline from %s with %u segments",
                      plugin->timeline_path,
                      plugin->priv->timeline_entries
                          ? plugin->priv->timeline_entries->len
                          : 0);
    } else if (plugin->timeline_path != NULL) {
      GST_WARNING_OBJECT(plugin,
                         "Failed to load timeline from %s, falling back to "
                         "internal preset selection",
                         plugin->timeline_path);
    }
    break;
  }
  case PROP_ENABLE_PLAYLIST:
    plugin->enable_playlist = g_value_get_boolean(value);
    break;
  case PROP_SHUFFLE_PRESETS:
    plugin->shuffle_presets = g_value_get_boolean(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    break;
  }
}

void gst_projectm_get_property(GObject *object, guint property_id,
                               GValue *value, GParamSpec *pspec) {
  GstProjectM *plugin = GST_PROJECTM(object);

  const gchar *property_name = g_param_spec_get_name(pspec);
  GST_DEBUG_OBJECT(plugin, "get-property <%s>", property_name);

  switch (property_id) {
  case PROP_PRESET_PATH:
    g_value_set_string(value, plugin->preset_path);
    break;
  case PROP_TEXTURE_DIR_PATH:
    g_value_set_string(value, plugin->texture_dir_path);
    break;
  case PROP_BEAT_SENSITIVITY:
    g_value_set_float(value, plugin->beat_sensitivity);
    break;
  case PROP_HARD_CUT_DURATION:
    g_value_set_double(value, plugin->hard_cut_duration);
    break;
  case PROP_HARD_CUT_ENABLED:
    g_value_set_boolean(value, plugin->hard_cut_enabled);
    break;
  case PROP_HARD_CUT_SENSITIVITY:
    g_value_set_float(value, plugin->hard_cut_sensitivity);
    break;
  case PROP_SOFT_CUT_DURATION:
    g_value_set_double(value, plugin->soft_cut_duration);
    break;
  case PROP_PRESET_DURATION:
    g_value_set_double(value, plugin->preset_duration);
    break;
  case PROP_MESH_SIZE: {
    gchar *meshSizeStr =
        g_strdup_printf("%lu,%lu", plugin->mesh_width, plugin->mesh_height);
    g_value_set_string(value, meshSizeStr);
    g_free(meshSizeStr);
    break;
  }
  case PROP_ASPECT_CORRECTION:
    g_value_set_boolean(value, plugin->aspect_correction);
    break;
  case PROP_EASTER_EGG:
    g_value_set_float(value, plugin->easter_egg);
    break;
  case PROP_PRESET_LOCKED:
    g_value_set_boolean(value, plugin->preset_locked);
    break;
  case PROP_TIMELINE_PATH:
    g_value_set_string(value, plugin->timeline_path);
    break;
  case PROP_ENABLE_PLAYLIST:
    g_value_set_boolean(value, plugin->enable_playlist);
    break;
  case PROP_SHUFFLE_PRESETS:
    g_value_set_boolean(value, plugin->shuffle_presets);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    break;
  }
}

static void gst_projectm_init(GstProjectM *plugin) {
  plugin->priv = gst_projectm_get_instance_private(plugin);

  plugin->priv->timeline_entries =
      g_ptr_array_new_with_free_func(gst_projectm_timeline_entry_free);
  plugin->priv->current_timeline_index = -1;
  plugin->priv->timeline_active = FALSE;
  plugin->priv->timeline_initialized = FALSE;
  plugin->priv->first_frame_received = FALSE;
  plugin->priv->first_frame_time = GST_CLOCK_TIME_NONE;
  plugin->priv->first_audio_received = FALSE;
  plugin->priv->first_audio_time = GST_CLOCK_TIME_NONE;
  plugin->priv->render_frame_count = 0;

  // Set default values for properties
  plugin->preset_path = DEFAULT_PRESET_PATH;
  plugin->texture_dir_path = DEFAULT_TEXTURE_DIR_PATH;
  plugin->timeline_path = DEFAULT_TIMELINE_PATH;
  plugin->beat_sensitivity = DEFAULT_BEAT_SENSITIVITY;
  plugin->hard_cut_duration = DEFAULT_HARD_CUT_DURATION;
  plugin->hard_cut_enabled = DEFAULT_HARD_CUT_ENABLED;
  plugin->hard_cut_sensitivity = DEFAULT_HARD_CUT_SENSITIVITY;
  plugin->soft_cut_duration = DEFAULT_SOFT_CUT_DURATION;
  plugin->preset_duration = DEFAULT_PRESET_DURATION;
  plugin->enable_playlist = DEFAULT_ENABLE_PLAYLIST;
  plugin->shuffle_presets = DEFAULT_SHUFFLE_PRESETS;

  const gchar *meshSizeStr = DEFAULT_MESH_SIZE;
  gint width, height;

  gchar **parts = g_strsplit(meshSizeStr, ",", 2);

  if (parts && g_strv_length(parts) == 2) {
    width = atoi(parts[0]);
    height = atoi(parts[1]);

    plugin->mesh_width = width;
    plugin->mesh_height = height;

    g_strfreev(parts);
  }

  plugin->aspect_correction = DEFAULT_ASPECT_CORRECTION;
  plugin->easter_egg = DEFAULT_EASTER_EGG;
  plugin->preset_locked = DEFAULT_PRESET_LOCKED;
  plugin->priv->handle = NULL;
  memset(plugin->priv->pbo_ids, 0, sizeof(plugin->priv->pbo_ids));
  plugin->priv->pbo_initialized = FALSE;
  plugin->priv->pbo_frame_valid = FALSE;
  plugin->priv->pbo_size = 0;
  plugin->priv->pbo_width = 0;
  plugin->priv->pbo_height = 0;
  plugin->priv->pbo_index = 0;
  plugin->priv->fbo_id = 0;
  plugin->priv->fbo_texture_id = 0;
  plugin->priv->fbo_depth_buffer_id = 0;
  plugin->priv->fbo_width = 0;
  plugin->priv->fbo_height = 0;
  plugin->priv->fbo_initialized = FALSE;
  plugin->priv->fbo_warned_missing_support = FALSE;
  plugin->priv->headless_mode = FALSE;
  plugin->priv->headless_checked = FALSE;
}

static void gst_projectm_finalize(GObject *object) {
  GstProjectM *plugin = GST_PROJECTM(object);
  g_free(plugin->preset_path);
  g_free(plugin->texture_dir_path);
  g_free(plugin->timeline_path);

  if (plugin->priv->timeline_entries != NULL) {
    g_ptr_array_free(plugin->priv->timeline_entries, TRUE);
    plugin->priv->timeline_entries = NULL;
  }
  G_OBJECT_CLASS(gst_projectm_parent_class)->finalize(object);
}

static void gst_projectm_gl_stop(GstGLBaseAudioVisualizer *src) {
  GstProjectM *plugin = GST_PROJECTM(src);
  const GstGLFuncs *glFunctions =
      src->context ? src->context->gl_vtable : NULL;

  if (plugin->priv->handle) {
    GST_DEBUG_OBJECT(plugin, "Destroying ProjectM instance");
    projectm_destroy(plugin->priv->handle);
    plugin->priv->handle = NULL;
  }

  gst_projectm_release_pbos(plugin, glFunctions);
  gst_projectm_release_render_target(plugin, glFunctions);
  gst_projectm_nv12_release(plugin, glFunctions);
  plugin->priv->current_timeline_index = -1;
  plugin->priv->timeline_initialized = FALSE;
  plugin->priv->first_frame_received = FALSE;
  plugin->priv->first_frame_time = GST_CLOCK_TIME_NONE;
  plugin->priv->headless_checked = FALSE;
  plugin->priv->headless_mode = FALSE;
}

static gboolean gst_projectm_gl_start(GstGLBaseAudioVisualizer *glav) {
  // Cast the audio visualizer to the ProjectM plugin
  GstProjectM *plugin = GST_PROJECTM(glav);
  const GstGLFuncs *glFunctions = glav->context->gl_vtable;

#ifdef USE_GLEW
  GST_DEBUG_OBJECT(plugin, "Initializing GLEW");
  GLenum err = glewInit();
  if (GLEW_OK != err) {
    GST_ERROR_OBJECT(plugin, "GLEW initialization failed");
    return FALSE;
  }
#endif

  /* Check for headless mode early - we need to create FBO before ProjectM init */
  gboolean is_headless = gst_projectm_check_headless_mode(plugin, glFunctions);

  if (is_headless) {
    GST_INFO_OBJECT(plugin, "Headless mode detected, creating FBO before ProjectM init");

    /* Create FBO with a default size - will be resized on first render if needed */
    /* Use 1920x1080 as initial size, common for video output */
    gboolean fbo_ok = gst_projectm_ensure_render_target(plugin, glFunctions, 1920, 1080);
    if (!fbo_ok) {
      GST_ERROR_OBJECT(plugin,
                       "Headless mode requires FBO but FBO creation failed");
      return FALSE;
    }

    /* Bind the FBO so ProjectM sees it as the current framebuffer during init */
    if (glFunctions->BindFramebuffer) {
      glFunctions->BindFramebuffer(GL_FRAMEBUFFER, plugin->priv->fbo_id);
      GST_DEBUG_OBJECT(plugin, "Bound FBO %u before ProjectM initialization",
                       plugin->priv->fbo_id);
    }
  }

  // Check if ProjectM instance exists, and create if not
  if (!plugin->priv->handle) {
    // Create ProjectM instance
    plugin->priv->handle = projectm_init(plugin);
    if (!plugin->priv->handle) {
      GST_ERROR_OBJECT(plugin, "ProjectM could not be initialized");
      return FALSE;
    }
    gl_error_handler(glav->context, plugin);

    plugin->priv->current_timeline_index = -1;
    plugin->priv->timeline_initialized = FALSE;
    plugin->priv->first_frame_received = FALSE;
    plugin->priv->first_frame_time = GST_CLOCK_TIME_NONE;

    gst_projectm_activate_timeline(plugin);
  }

  return TRUE;
}

static gboolean gst_projectm_setup(GstGLBaseAudioVisualizer *glav) {
  GstAudioVisualizer *bscope = GST_AUDIO_VISUALIZER(glav);
  GstProjectM *plugin = GST_PROJECTM(glav);

  // Calculate depth based on pixel stride and bits
  gint depth = bscope->vinfo.finfo->pixel_stride[0] *
               ((bscope->vinfo.finfo->bits >= 8) ? 8 : 1);

  // Calculate required samples per frame
  bscope->req_spf =
      (bscope->ainfo.channels * bscope->ainfo.rate * 2) / bscope->vinfo.fps_n;

  // get GStreamer video format and map it to the corresponding OpenGL pixel
  // format
  const GstVideoFormat video_format = GST_VIDEO_INFO_FORMAT(&bscope->vinfo);

  // TODO: why is the reversed byte order needed when copying pixel data from
  // OpenGL ?
  switch (video_format) {
  case GST_VIDEO_FORMAT_ABGR:
    plugin->priv->gl_format = GL_RGBA;
    plugin->priv->nv12_mode = FALSE;
    break;

  case GST_VIDEO_FORMAT_RGBA:
    // GL_ABGR_EXT does not seem to be well-supported, does not work on Windows
    plugin->priv->gl_format = GL_ABGR_EXT;
    plugin->priv->nv12_mode = FALSE;
    break;

  case GST_VIDEO_FORMAT_NV12:
    /* NV12 takes a different render path: projectm renders to its existing
     * RGBA FBO, then two GLSL passes convert to Y (full res, R8) and UV
     * (half res, RG8) plane textures. ReadPixels each plane into the
     * GstVideoFrame. We still keep gl_format = GL_RGBA for the source FBO
     * since projectm itself only outputs RGBA. */
    plugin->priv->gl_format = GL_RGBA;
    plugin->priv->nv12_mode = TRUE;
    GST_INFO_OBJECT(plugin,
                    "Output format = NV12: enabling GPU shader-based "
                    "RGBA→NV12 conversion path");
    break;

  default:
    GST_ERROR_OBJECT(plugin, "Unsupported video format: %d", video_format);
    return FALSE;
  }

  // Log audio info
  GST_DEBUG_OBJECT(
      glav, "Audio Information <Channels: %d, SampleRate: %d, Description: %s>",
      bscope->ainfo.channels, bscope->ainfo.rate,
      bscope->ainfo.finfo->description);

  // Log video info
  GST_DEBUG_OBJECT(glav,
                   "Video Information <Dimensions: %dx%d, FPS: %d/%d, Depth: "
                   "%dbit, SamplesPerFrame: %d>",
                   GST_VIDEO_INFO_WIDTH(&bscope->vinfo),
                   GST_VIDEO_INFO_HEIGHT(&bscope->vinfo), bscope->vinfo.fps_n,
                   bscope->vinfo.fps_d, depth, bscope->req_spf);

  return TRUE;
}

static double get_seconds_since_first_frame(GstProjectM *plugin,
                                            GstVideoFrame *frame) {
  if (!plugin->priv->first_frame_received) {
    // Store the timestamp of the first frame
    plugin->priv->first_frame_time = GST_BUFFER_PTS(frame->buffer);
    plugin->priv->first_frame_received = TRUE;
    return 0.0;
  }

  // Calculate elapsed time
  GstClockTime current_time = GST_BUFFER_PTS(frame->buffer);
  GstClockTime elapsed_time = current_time - plugin->priv->first_frame_time;

  // Convert to fractional seconds
  gdouble elapsed_seconds = (gdouble)elapsed_time / GST_SECOND;

  return elapsed_seconds;
}

/**
 * get_audio_elapsed_seconds:
 *
 * Returns elapsed seconds based on AUDIO buffer PTS.
 * Audio PTS is the authoritative clock for timeline decisions because it
 * advances at the true audio playback rate regardless of video encoding speed.
 * When CPU encoding is used (x264enc fallback), video PTS can run at 0.5-0.7x
 * of audio time, causing timeline entries to be skipped if video PTS is used.
 */
static double get_audio_elapsed_seconds(GstProjectM *plugin, GstBuffer *audio) {
  if (!plugin->priv->first_audio_received) {
    plugin->priv->first_audio_time = GST_BUFFER_PTS(audio);
    plugin->priv->first_audio_received = TRUE;
    return 0.0;
  }

  GstClockTime current_time = GST_BUFFER_PTS(audio);
  GstClockTime elapsed_time = current_time - plugin->priv->first_audio_time;

  return (gdouble)elapsed_time / GST_SECOND;
}

// TODO: CLEANUP & ADD DEBUGGING
static gboolean gst_projectm_render(GstGLBaseAudioVisualizer *glav,
                                    GstBuffer *audio, GstVideoFrame *video) {
  GstProjectM *plugin = GST_PROJECTM(glav);

  GstMapInfo audioMap;
  gboolean result = TRUE;

  // Use audio PTS as the authoritative clock for timeline decisions.
  // Audio PTS advances at the true playback rate regardless of video encoding
  // speed. Video PTS can drift when CPU encoding (x264enc) is used as fallback.
  double audio_elapsed = get_audio_elapsed_seconds(plugin, audio);
  double video_elapsed = get_seconds_since_first_frame(plugin, video);

  // Set projectM time from audio PTS so animations sync to audio, not encoding speed
  projectm_set_frame_time(plugin->priv->handle, audio_elapsed);

  // Timeline switching uses audio PTS to ensure all entries are visited
  gst_projectm_timeline_update(plugin, audio_elapsed);

  // PTS diagnostic: log audio vs video PTS every 600 frames (~10s at 60fps)
  plugin->priv->render_frame_count++;
  if (plugin->priv->render_frame_count % 600 == 0) {
    GST_INFO_OBJECT(plugin,
                    "PTS diagnostic frame=%lu audio_elapsed=%.3f "
                    "video_elapsed=%.3f ratio=%.3f timeline_idx=%d",
                    (unsigned long)plugin->priv->render_frame_count,
                    audio_elapsed, video_elapsed,
                    video_elapsed > 0.001 ? audio_elapsed / video_elapsed : 0.0,
                    plugin->priv->current_timeline_index);
  }

  // AUDIO
  gst_buffer_map(audio, &audioMap, GST_MAP_READ);

  // GST_DEBUG_OBJECT(plugin, "Audio Samples: %u, Offset: %lu, Offset End: %lu,
  // Sample Rate: %d, FPS: %d, Required Samples Per Frame: %d",
  //                  audioMap.size / 8, audio->offset, audio->offset_end,
  //                  bscope->ainfo.rate, bscope->vinfo.fps_n, bscope->req_spf);

  projectm_pcm_add_int16(plugin->priv->handle, (gint16 *)audioMap.data,
                         audioMap.size / 4, PROJECTM_STEREO);

  // GST_DEBUG_OBJECT(plugin, "Audio Data: %d %d %d %d", ((gint16
  // *)audioMap.data)[100], ((gint16 *)audioMap.data)[101], ((gint16
  // *)audioMap.data)[102], ((gint16 *)audioMap.data)[103]);

  // VIDEO
  const GstGLFuncs *glFunctions = glav->context->gl_vtable;

  size_t windowWidth, windowHeight;

  projectm_get_window_size(plugin->priv->handle, &windowWidth, &windowHeight);

  /* Check if we're in headless mode (no default framebuffer) */
  gboolean is_headless = gst_projectm_check_headless_mode(plugin, glFunctions);

  gboolean using_fbo = gst_projectm_ensure_render_target(
      plugin, glFunctions, windowWidth, windowHeight);
  gboolean restore_viewport = FALSE;
  GLint previous_viewport[4] = {0, 0, 0, 0};

  /* In headless mode, we MUST have an FBO to render to */
  if (is_headless && !using_fbo) {
    GST_ERROR_OBJECT(plugin,
                     "Headless mode detected but FBO creation failed; "
                     "cannot render without a valid framebuffer");
    gst_buffer_unmap(audio, &audioMap);
    return FALSE;
  }

  if (using_fbo && glFunctions && glFunctions->BindFramebuffer) {
    glFunctions->BindFramebuffer(GL_FRAMEBUFFER, plugin->priv->fbo_id);
    GST_LOG_OBJECT(plugin, "Bound FBO %u for rendering (%zux%zu)",
                   plugin->priv->fbo_id, windowWidth, windowHeight);
    if (glFunctions->Viewport) {
      if (glFunctions->GetIntegerv) {
        glFunctions->GetIntegerv(GL_VIEWPORT, previous_viewport);
        restore_viewport = TRUE;
      }
      glFunctions->Viewport(0, 0, (GLsizei)windowWidth, (GLsizei)windowHeight);
    }
  } else if (!is_headless && glFunctions && glFunctions->BindFramebuffer) {
    /* Only bind framebuffer 0 if we're NOT in headless mode */
    glFunctions->BindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  /* Use FBO-specific render function when we have an FBO, otherwise use default */
  if (using_fbo && plugin->priv->fbo_id != 0) {
    projectm_opengl_render_frame_fbo(plugin->priv->handle, plugin->priv->fbo_id);
    GST_LOG_OBJECT(plugin, "Rendered frame to FBO %u", plugin->priv->fbo_id);
  } else {
    projectm_opengl_render_frame(plugin->priv->handle);
  }
  gl_error_handler(glav->context, plugin);

  /* NV12 path: shader-convert RGBA FBO into Y + UV planes, ReadPixels each.
   * Skips PBO async readback — perf gain comes from eliminating downstream
   * videoconvert, not from PBO double-buffering of the readback itself. */
  if (plugin->priv->nv12_mode) {
    if (!gst_projectm_nv12_render(plugin, glFunctions, video,
                                   windowWidth, windowHeight)) {
      GST_ERROR_OBJECT(plugin, "NV12 render path failed");
      gst_buffer_unmap(audio, &audioMap);
      return FALSE;
    }
  } else {
    /* Ensure FBO is still bound for ReadPixels */
    if (using_fbo && glFunctions && glFunctions->BindFramebuffer) {
      glFunctions->BindFramebuffer(GL_FRAMEBUFFER, plugin->priv->fbo_id);
    }

    gboolean used_async = FALSE;
    if (gst_projectm_ensure_pbos(plugin, glFunctions, windowWidth,
                                 windowHeight)) {
      used_async = gst_projectm_download_frame_with_pbo(
          plugin, glFunctions, video, windowWidth, windowHeight);
    }

    if (!used_async) {
      glFunctions->ReadPixels(0, 0, windowWidth, windowHeight,
                              plugin->priv->gl_format, GL_UNSIGNED_INT_8_8_8_8,
                              (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(video, 0));
    }
  }

  if (using_fbo && glFunctions && glFunctions->BindFramebuffer) {
    /* In headless mode, don't unbind to framebuffer 0 since it doesn't exist */
    if (!is_headless) {
      glFunctions->BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (restore_viewport && glFunctions->Viewport) {
      glFunctions->Viewport(previous_viewport[0], previous_viewport[1],
                            previous_viewport[2], previous_viewport[3]);
    }
  }

  gst_buffer_unmap(audio, &audioMap);

  // GST_DEBUG_OBJECT(plugin, "Video Data: %d %d\n",
  // GST_VIDEO_FRAME_N_PLANES(video), ((uint8_t
  // *)(GST_VIDEO_FRAME_PLANE_DATA(video, 0)))[0]);

  // GST_DEBUG_OBJECT(plugin, "Rendered one frame");

  return result;
}

static void gst_projectm_class_init(GstProjectMClass *klass) {
  GObjectClass *gobject_class = (GObjectClass *)klass;
  GstElementClass *element_class = (GstElementClass *)klass;
  GstGLBaseAudioVisualizerClass *scope_class =
      GST_GL_BASE_AUDIO_VISUALIZER_CLASS(klass);

  // Setup audio and video caps
  const gchar *audio_sink_caps = get_audio_sink_cap(0);
  const gchar *video_src_caps = get_video_src_cap(0);

  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                           gst_caps_from_string(video_src_caps)));
  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                           gst_caps_from_string(audio_sink_caps)));

  gst_element_class_set_static_metadata(
      GST_ELEMENT_CLASS(klass), "ProjectM Visualizer", "Generic",
      "A plugin for visualizing music using ProjectM",
      "AnomieVision <anomievision@gmail.com> | Tristan Charpentier "
      "<tristan_charpentier@hotmail.com>");

  // Setup properties
  gobject_class->set_property = gst_projectm_set_property;
  gobject_class->get_property = gst_projectm_get_property;

  g_object_class_install_property(
      gobject_class, PROP_PRESET_PATH,
      g_param_spec_string(
          "preset", "Preset",
          "Specifies the path to the preset file. The preset file determines "
          "the visual style and behavior of the audio visualizer.",
          DEFAULT_PRESET_PATH, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TEXTURE_DIR_PATH,
      g_param_spec_string("texture-dir", "Texture Directory",
                          "Sets the path to the directory containing textures "
                          "used in the visualizer.",
                          DEFAULT_TEXTURE_DIR_PATH,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TIMELINE_PATH,
      g_param_spec_string(
          "timeline-path", "Timeline Path",
          "Path to a preset timeline definition (.ini) used for deterministic "
          "preset scheduling.",
          DEFAULT_TIMELINE_PATH, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_BEAT_SENSITIVITY,
      g_param_spec_float(
          "beat-sensitivity", "Beat Sensitivity",
          "Controls the sensitivity to audio beats. Higher values make the "
          "visualizer respond more strongly to beats.",
          0.0, 5.0, DEFAULT_BEAT_SENSITIVITY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_HARD_CUT_DURATION,
      g_param_spec_double("hard-cut-duration", "Hard Cut Duration",
                          "Sets the duration, in seconds, for hard cuts. Hard "
                          "cuts are abrupt transitions in the visualizer.",
                          0.0, 999999.0, DEFAULT_HARD_CUT_DURATION,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_HARD_CUT_ENABLED,
      g_param_spec_boolean(
          "hard-cut-enabled", "Hard Cut Enabled",
          "Enables or disables hard cuts. When enabled, the visualizer may "
          "exhibit sudden transitions based on the audio input.",
          DEFAULT_HARD_CUT_ENABLED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_HARD_CUT_SENSITIVITY,
      g_param_spec_float(
          "hard-cut-sensitivity", "Hard Cut Sensitivity",
          "Adjusts the sensitivity of the visualizer to hard cuts. Higher "
          "values increase the responsiveness to abrupt changes in audio.",
          0.0, 1.0, DEFAULT_HARD_CUT_SENSITIVITY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_SOFT_CUT_DURATION,
      g_param_spec_double(
          "soft-cut-duration", "Soft Cut Duration",
          "Sets the duration, in seconds, for soft cuts. Soft cuts are "
          "smoother transitions between visualizer states.",
          0.0, 999999.0, DEFAULT_SOFT_CUT_DURATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_PRESET_DURATION,
      g_param_spec_double("preset-duration", "Preset Duration",
                          "Sets the duration, in seconds, for each preset. A "
                          "zero value causes the preset to play indefinitely.",
                          0.0, 999999.0, DEFAULT_PRESET_DURATION,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_MESH_SIZE,
      g_param_spec_string("mesh-size", "Mesh Size",
                          "Sets the size of the mesh used in rendering. The "
                          "format is 'width,height'.",
                          DEFAULT_MESH_SIZE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ASPECT_CORRECTION,
      g_param_spec_boolean(
          "aspect-correction", "Aspect Correction",
          "Enables or disables aspect ratio correction. When enabled, the "
          "visualizer adjusts for aspect ratio differences in rendering.",
          DEFAULT_ASPECT_CORRECTION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_EASTER_EGG,
      g_param_spec_float(
          "easter-egg", "Easter Egg",
          "Controls the activation of an Easter Egg feature. The value "
          "determines the likelihood of triggering the Easter Egg.",
          0.0, 1.0, DEFAULT_EASTER_EGG,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_PRESET_LOCKED,
      g_param_spec_boolean(
          "preset-locked", "Preset Locked",
          "Locks or unlocks the current preset. When locked, the visualizer "
          "remains on the current preset without automatic changes.",
          DEFAULT_PRESET_LOCKED, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ENABLE_PLAYLIST,
      g_param_spec_boolean(
          "enable-playlist", "Enable Playlist",
          "Enables or disables the playlist feature. When enabled, the "
          "visualizer can switch between presets based on a provided playlist.",
          DEFAULT_ENABLE_PLAYLIST, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_SHUFFLE_PRESETS,
      g_param_spec_boolean(
          "shuffle-presets", "Shuffle Presets",
          "Enables or disables preset shuffling. When enabled, the visualizer "
          "randomly selects presets from the playlist if presets are provided "
          "and not locked. Playlist must be enabled for this to take effect.",
          DEFAULT_SHUFFLE_PRESETS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gobject_class->finalize = gst_projectm_finalize;

  scope_class->supported_gl_api = GST_GL_API_OPENGL3 | GST_GL_API_GLES2;
  scope_class->gl_start = GST_DEBUG_FUNCPTR(gst_projectm_gl_start);
  scope_class->gl_stop = GST_DEBUG_FUNCPTR(gst_projectm_gl_stop);
  scope_class->gl_render = GST_DEBUG_FUNCPTR(gst_projectm_render);
  scope_class->setup = GST_DEBUG_FUNCPTR(gst_projectm_setup);
}

static gboolean plugin_init(GstPlugin *plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_projectm_debug, "projectm", 0,
                          "projectM visualizer plugin");

  return gst_element_register(plugin, "projectm", GST_RANK_NONE,
                              GST_TYPE_PROJECTM);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, projectm,
                  "plugin to visualize audio using the ProjectM library",
                  plugin_init, PACKAGE_VERSION, PACKAGE_LICENSE, PACKAGE_NAME,
                  PACKAGE_ORIGIN)
