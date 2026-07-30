/* wmw2_movie.c -- cutscene playback for assets/Water/Movies/
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The engine plays eight H.264 clips out of assets/Water/Movies/ -- location
 * intros and outros -- through BridgeWalaberCustomLayout::jniPlayMovie, and
 * waits to be told they finished. The port used to answer "finished already",
 * which is why cutscenes were a black screen that vanished instantly. This
 * decodes them instead.
 *
 * TWO THINGS DIFFER FROM THE USUAL SWITCH VIDEO PORT
 * --------------------------------------------------
 * 1. GLES 1.1, so there are no shaders. The reference ports this is modelled on
 *    upload Y, U and V as three textures and convert in a fragment shader; that
 *    is not available here, and linking GLESv2 alongside GLESv1_CM is a wall of
 *    multiple-definition errors (section 6.7). So swscale converts to RGBA on
 *    the CPU and the frame goes up as a single texture drawn with the
 *    fixed-function pipeline -- the same way nx_pointer draws the cursor.
 *
 *    That costs CPU, but these clips are small and short, and the engine is not
 *    drawing anything else while one is on screen.
 *
 * 2. Audio goes through the existing FMOD pump rather than a second device.
 *    fmod_audio.c already owns audout; wmw2_movie_mix_s16() is called from
 *    inside its block loop and mixes the clip's PCM into whatever FMOD produced.
 *    Opening a second audout stream would fight it for the same hardware.
 *
 * PLAYBACK IS BLOCKING, deliberately. jniPlayMovie is an upcall from inside the
 * engine's own frame; it expects the movie to happen and then to be told it is
 * over. So this drives its own present loop for the duration, which is also how
 * the reference ports do it.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <switch.h>
#include <GLES/gl.h>

#include "wmw2_movie.h"
#include "wmw_paths.h"
#include "util.h"

#if WMW2_VIDEO

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
/* Named explicitly rather than relied on transitively: avcodec.h happens to
 * pull these in today, but which libavutil headers it exposes has changed
 * between ffmpeg majors and a missing one shows up as a bare "undeclared
 * identifier" a long way from the cause. */
#include <libavutil/samplefmt.h>      /* av_samples_alloc, AV_SAMPLE_FMT_S16 */
#include <libavutil/channel_layout.h> /* AVChannelLayout, AV_CHANNEL_LAYOUT_STEREO */
#include <libavutil/pixfmt.h>         /* AV_PIX_FMT_RGBA */
#include <libavutil/rational.h>       /* av_q2d */
#include <libavutil/mem.h>            /* av_freep */

#define MOVIE_RATE     48000        /* matches the audout rate fmod_audio opens */
#define MOVIE_CHANNELS 2
#define ARING_FRAMES   (MOVIE_RATE * 2)   /* two seconds of slack */

static void (*s_present)(void);
static int   s_render_w = 720, s_render_h = 1280;
static int   s_inited;

/* --- audio ring, written by the play loop, drained by the FMOD pump -------- */
static int16_t *s_aring;
static volatile uint32_t s_awr, s_ard;
static Mutex    s_alock;
static volatile int s_playing;

static void aring_reset(void) {
  mutexLock(&s_alock);
  s_awr = s_ard = 0;
  mutexUnlock(&s_alock);
}

static void aring_push(const int16_t *src, int frames) {
  mutexLock(&s_alock);
  for (int i = 0; i < frames; i++) {
    const uint32_t next = (s_awr + 1) % ARING_FRAMES;
    if (next == s_ard) break;                 /* full: drop rather than stall */
    s_aring[s_awr * MOVIE_CHANNELS + 0] = src[i * MOVIE_CHANNELS + 0];
    s_aring[s_awr * MOVIE_CHANNELS + 1] = src[i * MOVIE_CHANNELS + 1];
    s_awr = next;
  }
  mutexUnlock(&s_alock);
}

static int aring_available(void) {
  mutexLock(&s_alock);
  const int n = (int)((s_awr + ARING_FRAMES - s_ard) % ARING_FRAMES);
  mutexUnlock(&s_alock);
  return n;
}

int wmw2_movie_mix_s16(int16_t *dst, int frames, int channels) {
  if (!s_aring || !s_playing) return 0;
  int mixed = 0;
  mutexLock(&s_alock);
  for (int i = 0; i < frames && s_ard != s_awr; i++) {
    const int16_t l = s_aring[s_ard * MOVIE_CHANNELS + 0];
    const int16_t r = s_aring[s_ard * MOVIE_CHANNELS + 1];
    s_ard = (s_ard + 1) % ARING_FRAMES;
    /* Additive, saturating. The engine usually ducks its own audio during a
     * cutscene, but if it does not, clipping is better than a wrapped sample. */
    for (int c = 0; c < channels; c++) {
      int32_t v = dst[i * channels + c] + ((c == 0) ? l : r);
      if (v >  32767) v =  32767;
      if (v < -32768) v = -32768;
      dst[i * channels + c] = (int16_t)v;
    }
    mixed++;
  }
  mutexUnlock(&s_alock);
  return mixed;
}

int wmw2_movie_is_playing(void) { return s_playing; }

void wmw2_movie_init(void (*present)(void), int render_w, int render_h) {
  s_present  = present;
  s_render_w = render_w;
  s_render_h = render_h;
  mutexInit(&s_alock);
  s_aring = malloc((size_t)ARING_FRAMES * MOVIE_CHANNELS * sizeof(int16_t));
  s_inited = (s_aring != NULL);
  debugPrintf("movie: player ready (%dx%d target)%s\n", render_w, render_h,
              s_inited ? "" : " -- no audio ring, out of memory");
}

/* ffmpeg reads the clip out of memory through these; see wmw2_movie_play(). */
typedef struct { const uint8_t *data; size_t size; size_t pos; } MemSrc;

static int mem_read(void *o, uint8_t *buf, int n) {
  MemSrc *m = o;
  if (m->pos >= m->size) return AVERROR_EOF;
  size_t left = m->size - m->pos;
  if ((size_t)n > left) n = (int)left;
  memcpy(buf, m->data + m->pos, (size_t)n);
  m->pos += (size_t)n;
  return n;
}

static int64_t mem_seek(void *o, int64_t off, int whence) {
  MemSrc *m = o;
  if (whence == AVSEEK_SIZE) return (int64_t)m->size;
  int64_t p = (whence == SEEK_CUR) ? (int64_t)m->pos + off
            : (whence == SEEK_END) ? (int64_t)m->size + off
                                   : off;
  if (p < 0) p = 0;
  if (p > (int64_t)m->size) p = (int64_t)m->size;
  m->pos = (size_t)p;
  return p;
}

/* --------------------------------------------------------------------------
 * GLES 1.1 presentation
 *
 * Drawn into the same portrait target the engine renders into, so wmw_tate's
 * rotation carries it. Every piece of state touched is restored, for the reason
 * in section 6.8: the engine's own draw calls resume immediately afterwards and
 * a client array pointer left dangling is read on its next frame.
 * ------------------------------------------------------------------------ */

static GLuint s_tex;
static int    s_tex_w, s_tex_h;      /* allocated texture size (power of two) */
static int    s_src_w, s_src_h;      /* the part of it the frame occupies     */

/* GLES 1.1 has no guaranteed non-power-of-two texture support -- NPOT is the
 * GL_OES_texture_npot extension, not core -- and the shipped clips are 360x480,
 * which is neither. Rather than depend on an extension, allocate the next power
 * of two (512x512 here), upload each frame into the corner with
 * glTexSubImage2D, and scale the texture coordinates to match. Costs a little
 * texture memory and nothing else.
 *
 * Getting this wrong would not fail loudly: a driver without NPOT returns
 * GL_INVALID_VALUE from glTexImage2D and draws an untextured quad, so the
 * cutscene would be a solid white rectangle rather than an error. */
static int next_pot(int v) {
  int p = 1;
  while (p < v) p <<= 1;
  return p;
}

/* Full state of one GLES1 client array, so restoring it cannot leave the engine
 * reading our quad on its next draw. Same shape as nx_pointer's, and for the
 * same reason. */
typedef struct { GLint size, type, stride, buf; GLvoid *ptr; } ClientArray;

static void carray_save(GLenum size_e, GLenum type_e, GLenum stride_e,
                        GLenum ptr_e, ClientArray *a) {
  glGetIntegerv(size_e,   &a->size);
  glGetIntegerv(type_e,   &a->type);
  glGetIntegerv(stride_e, &a->stride);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &a->buf);
  glGetPointerv(ptr_e, &a->ptr);
}

static void draw_frame(const uint8_t *rgba, int w, int h) {
  /* ---- save every piece of state this touches -------------------------
   *
   * Section 6.8. The first version of this restored the enables and the
   * matrices but NOT the client array pointers, and the result was a black
   * screen after any cutscene that returned to a screen already built: the
   * engine's next draw was still pointing at the movie quad's two static
   * arrays. The opening cutscene hid it, because the engine constructs a fresh
   * screen afterwards and sets its pointers again. */
  const GLboolean was_blend   = glIsEnabled(GL_BLEND);
  const GLboolean was_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean was_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean was_alpha   = glIsEnabled(GL_ALPHA_TEST);
  const GLboolean was_light   = glIsEnabled(GL_LIGHTING);
  const GLboolean was_tex2d   = glIsEnabled(GL_TEXTURE_2D);
  const GLboolean was_vtx     = glIsEnabled(GL_VERTEX_ARRAY);
  const GLboolean was_txc     = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  const GLboolean was_col     = glIsEnabled(GL_COLOR_ARRAY);
  GLint prev_tex = 0, prev_buf = 0, prev_active = 0, prev_cactive = 0;
  GLint bs = GL_SRC_ALPHA, bd = GL_ONE_MINUS_SRC_ALPHA;
  glGetIntegerv(GL_TEXTURE_BINDING_2D,    &prev_tex);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING,  &prev_buf);
  glGetIntegerv(GL_ACTIVE_TEXTURE,        &prev_active);
  glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &prev_cactive);
  glGetIntegerv(GL_BLEND_SRC, &bs);
  glGetIntegerv(GL_BLEND_DST, &bd);

  ClientArray sv_v, sv_t;
  carray_save(GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE,
              GL_VERTEX_ARRAY_STRIDE, GL_VERTEX_ARRAY_POINTER, &sv_v);
  carray_save(GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE,
              GL_TEXTURE_COORD_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY_POINTER, &sv_t);

  glBindBuffer(GL_ARRAY_BUFFER, 0);       /* our arrays are client-side */
  glActiveTexture(GL_TEXTURE0);
  glClientActiveTexture(GL_TEXTURE0);

  if (!s_tex) {
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s_tex_w = s_tex_h = s_src_w = s_src_h = 0;
  }
  glBindTexture(GL_TEXTURE_2D, s_tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (w != s_src_w || h != s_src_h) {
    s_src_w = w; s_src_h = h;
    s_tex_w = next_pot(w); s_tex_h = next_pot(h);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_tex_w, s_tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    debugPrintf("movie: %dx%d frame in a %dx%d texture\n", w, h, s_tex_w, s_tex_h);
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

  /* Letterbox: fit the clip inside the portrait target, preserving aspect. */
  const float ta = (float)s_render_w / (float)s_render_h;
  const float va = (float)w / (float)h;
  float qw = 1.0f, qh = 1.0f;
  if (va > ta) qh = ta / va; else qw = va / ta;

  static GLfloat verts[8], uvs[8];        /* static: 6.8 -- never stack locals */
  const float x0 = (1.0f - qw) * 0.5f * s_render_w;
  const float x1 = s_render_w - x0;
  const float y0 = (1.0f - qh) * 0.5f * s_render_h;
  const float y1 = s_render_h - y0;
  verts[0]=x0; verts[1]=y0;  verts[2]=x1; verts[3]=y0;
  verts[4]=x0; verts[5]=y1;  verts[6]=x1; verts[7]=y1;
  const float su = (float)s_src_w / (float)s_tex_w;
  const float sv = (float)s_src_h / (float)s_tex_h;
  uvs[0]=0;  uvs[1]=0;   uvs[2]=su; uvs[3]=0;
  uvs[4]=0;  uvs[5]=sv;  uvs[6]=su; uvs[7]=sv;

  glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
  glOrthof(0, (GLfloat)s_render_w, (GLfloat)s_render_h, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_LIGHTING);
  glDisable(GL_BLEND);
  glEnable(GL_TEXTURE_2D);
  glColor4f(1, 1, 1, 1);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, verts);
  glTexCoordPointer(2, GL_FLOAT, 0, uvs);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  /* ---- restore everything ---------------------------------------------- */
  glMatrixMode(GL_MODELVIEW);  glPopMatrix();
  glMatrixMode(GL_PROJECTION); glPopMatrix();
  glMatrixMode(GL_MODELVIEW);

  /* Each array's pointer must go back WITH the buffer it was captured under. */
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)sv_v.buf);
  if (sv_v.size > 0) glVertexPointer(sv_v.size, (GLenum)sv_v.type, sv_v.stride, sv_v.ptr);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)sv_t.buf);
  if (sv_t.size > 0) glTexCoordPointer(sv_t.size, (GLenum)sv_t.type, sv_t.stride, sv_t.ptr);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);

  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  glClientActiveTexture((GLenum)prev_cactive);
  glActiveTexture((GLenum)prev_active);

  glBlendFunc((GLenum)bs, (GLenum)bd);
  if (was_blend)   glEnable(GL_BLEND);         else glDisable(GL_BLEND);
  if (was_depth)   glEnable(GL_DEPTH_TEST);    else glDisable(GL_DEPTH_TEST);
  if (was_cull)    glEnable(GL_CULL_FACE);     else glDisable(GL_CULL_FACE);
  if (was_scissor) glEnable(GL_SCISSOR_TEST);  else glDisable(GL_SCISSOR_TEST);
  if (was_alpha)   glEnable(GL_ALPHA_TEST);
  if (was_light)   glEnable(GL_LIGHTING);
  if (was_tex2d)   glEnable(GL_TEXTURE_2D);    else glDisable(GL_TEXTURE_2D);
  if (was_vtx) glEnableClientState(GL_VERTEX_ARRAY);        else glDisableClientState(GL_VERTEX_ARRAY);
  if (was_txc) glEnableClientState(GL_TEXTURE_COORD_ARRAY); else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  if (was_col) glEnableClientState(GL_COLOR_ARRAY);         else glDisableClientState(GL_COLOR_ARRAY);
}

/* ------------------------------------------------------------------------- */

static int skip_requested(PadState *pad) {
  padUpdate(pad);
  const u64 down = padGetButtonsDown(pad);
  return (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) != 0;
}

int wmw2_movie_play(const char *engine_path) {
  if (!s_inited || !s_present || !engine_path) return 0;

  /* Every resource declared up front and NULL, because the failure paths below
   * use `goto done` and C lets a goto jump over an initialiser -- the variable
   * is then indeterminate, and done: would free garbage. */
  AVFormatContext *fmt   = NULL;
  AVIOContext     *avio  = NULL;
  uint8_t         *clip  = NULL;
  AVCodecContext  *vctx  = NULL, *actx = NULL;
  struct SwsContext *sws = NULL;
  SwrContext      *swr   = NULL;
  uint8_t         *rgba  = NULL;
  AVFrame         *frame = NULL;
  AVPacket        *pkt   = NULL;
  size_t clip_size = 0;
  int vidx = -1, aidx = -1, played = 0;
  MemSrc src = { NULL, 0, 0 };

  char path[WMW_PATH_MAX];
  {
    /* The engine hands us "/Water/Movies/x.mp4"; the resolver turns that into
     * the real location under assets/. */
    char buf[WMW_PATH_MAX];
    const char *rp = wmw_resolve(engine_path, buf, sizeof(buf));
    snprintf(path, sizeof(path), "%s", rp);
  }

  /* Read the clip into memory and feed ffmpeg through a custom AVIOContext,
   * rather than handing it the path.
   *
   * avformat_open_input() parses its filename as a URL, and every devkitPro
   * path starts with a devoptab prefix -- "sdmc:/switch/..." -- which ffmpeg
   * reads as a protocol named "sdmc". There is no such protocol, so it fails
   * before touching the filesystem:
   *
   *     movie: cannot open sdmc:/switch/wmw2_nx/assets/Water/Movies/location1_swampy.mp4
   *
   * even though the path is exactly right and the file is exactly there.
   * Escaping it as "file:..." is not enough either, because ffmpeg's file
   * protocol then re-parses what follows. Bypassing the URL layer entirely is
   * what the reference ports do, and the clips are ~1 MB so holding one is
   * cheap. */
  {
    FILE *cf = fopen(path, "rb");
    if (!cf) {
      debugPrintf("movie: cannot open %s\n", path);
      goto done;
    }
    fseek(cf, 0, SEEK_END);
    const long n = ftell(cf);
    fseek(cf, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(cf); goto done; }
    clip = av_malloc((size_t)n);
    if (!clip) { fclose(cf); goto done; }
    clip_size = fread(clip, 1, (size_t)n, cf);
    fclose(cf);
    if (clip_size != (size_t)n) goto done;
  }

  src.data = clip; src.size = clip_size; src.pos = 0;
  const int avio_bufsz = 32768;
  uint8_t *avio_buf = av_malloc(avio_bufsz);
  avio = avio_buf
      ? avio_alloc_context(avio_buf, avio_bufsz, 0, &src, mem_read, NULL, mem_seek)
      : NULL;
  if (!avio) { av_free(avio_buf); goto done; }

  fmt = avformat_alloc_context();
  if (!fmt) goto done;
  fmt->pb = avio;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_open_input(&fmt, NULL, NULL, NULL) != 0) {
    debugPrintf("movie: cannot parse %s\n", path);
    fmt = NULL;                     /* open_input frees it on failure */
    goto done;
  }
  if (avformat_find_stream_info(fmt, NULL) < 0) {
    debugPrintf("movie: no stream info in %s\n", path);
    goto done;
  }

  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    const enum AVMediaType t = fmt->streams[i]->codecpar->codec_type;
    if (t == AVMEDIA_TYPE_VIDEO && vidx < 0) vidx = (int)i;
    if (t == AVMEDIA_TYPE_AUDIO && aidx < 0) aidx = (int)i;
  }
  if (vidx < 0) {
    debugPrintf("movie: no video stream in %s\n", path);
    goto done;
  }

  frame = av_frame_alloc();
  pkt   = av_packet_alloc();

  if (!frame || !pkt) {
    debugPrintf("movie: out of memory setting up %s\n", path);
    goto done;
  }

  {
    const AVCodec *vc = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
    if (!vc) {
      debugPrintf("movie: no decoder for %s\n", path);
      goto done;
    }
    vctx = avcodec_alloc_context3(vc);
    if (!vctx) goto done;
    if (avcodec_parameters_to_context(vctx, fmt->streams[vidx]->codecpar) < 0)
      goto done;
    vctx->thread_count = 3;            /* three of the four cores are ours */
    if (avcodec_open2(vctx, vc, NULL) < 0) {
      debugPrintf("movie: could not open the decoder for %s\n", path);
      goto done;
    }
  }
  if (aidx >= 0) {
    /* Audio is optional throughout: a clip that will not decode its soundtrack
     * still plays, silently, rather than not playing. */
    const AVCodec *ac = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id);
    if (ac) actx = avcodec_alloc_context3(ac);
    if (actx &&
        (avcodec_parameters_to_context(actx, fmt->streams[aidx]->codecpar) < 0 ||
         avcodec_open2(actx, ac, NULL) < 0)) {
      avcodec_free_context(&actx);
      actx = NULL;
    }
  }

  const int vw = vctx->width, vh = vctx->height;
  sws = sws_getContext(vw, vh, vctx->pix_fmt, vw, vh, AV_PIX_FMT_RGBA,
                       SWS_BILINEAR, NULL, NULL, NULL);
  rgba = malloc((size_t)vw * vh * 4);
  if (!sws || !rgba) goto done;

  if (actx) {
    AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
    /* Order matters: swr_alloc_set_opts2 can return non-zero AND leave swr
     * NULL, so it has to be checked before swr_init is allowed near it. */
    if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, MOVIE_RATE,
                            &actx->ch_layout, actx->sample_fmt,
                            actx->sample_rate, 0, NULL) != 0 ||
        !swr || swr_init(swr) != 0) {
      if (swr) swr_free(&swr);
      swr = NULL;
    }
  }

  aring_reset();
  s_playing = 1;
  played = 1;
  debugPrintf("movie: playing %s (%dx%d%s)\n", path, vw, vh,
              swr ? ", with audio" : ", silent");
  debugLogFlush();

  PadState pad;
  padInitializeDefault(&pad);

  const AVRational vtb = fmt->streams[vidx]->time_base;
  const uint64_t t0 = armTicksToNs(armGetSystemTick());
  int skipped = 0;

  while (!skipped && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vidx) {
      if (avcodec_send_packet(vctx, pkt) == 0) {
        while (avcodec_receive_frame(vctx, frame) == 0) {
          /* Hold each frame until its presentation time. The clips are 30fps
           * and the loop is otherwise free-running, so without this they play
           * at decode speed. */
          if (frame->pts != AV_NOPTS_VALUE) {
            const double pts = (double)frame->pts * av_q2d(vtb);
            for (;;) {
              const double now =
                  (double)(armTicksToNs(armGetSystemTick()) - t0) / 1e9;
              if (now >= pts) break;
              if (skip_requested(&pad)) { skipped = 1; break; }
              svcSleepThread(1000000ull);          /* 1 ms */
            }
          }
          if (skipped) break;

          uint8_t *dst[4] = { rgba, NULL, NULL, NULL };
          int stride[4] = { vw * 4, 0, 0, 0 };
          sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
                    0, vh, dst, stride);
          draw_frame(rgba, vw, vh);
          s_present();
        }
      }
    } else if (actx && swr && pkt->stream_index == aidx) {
      if (avcodec_send_packet(actx, pkt) == 0) {
        while (avcodec_receive_frame(actx, frame) == 0) {
          uint8_t *obuf = NULL;
          const int max_out = swr_get_out_samples(swr, frame->nb_samples);
          if (av_samples_alloc(&obuf, NULL, MOVIE_CHANNELS, max_out,
                               AV_SAMPLE_FMT_S16, 0) >= 0) {
            const int n = swr_convert(swr, &obuf, max_out,
                                      (const uint8_t **)frame->data,
                                      frame->nb_samples);
            if (n > 0) aring_push((const int16_t *)obuf, n);
            av_freep(&obuf);
          }
        }
      }
    }
    av_packet_unref(pkt);
    if (!skipped && skip_requested(&pad)) skipped = 1;
  }

  if (skipped) debugPrintf("movie: skipped by the player\n");
  else         debugPrintf("movie: finished\n");

done:
  s_playing = 0;
  aring_reset();
  if (swr)  swr_free(&swr);
  if (sws)  sws_freeContext(sws);
  free(rgba);
  av_packet_free(&pkt);
  av_frame_free(&frame);
  if (actx) avcodec_free_context(&actx);
  if (vctx) avcodec_free_context(&vctx);
  /* Our AVIOContext and its buffer are ours to free; avformat_close_input will
   * not touch them because the context was flagged AVFMT_FLAG_CUSTOM_IO. */
  if (fmt) avformat_close_input(&fmt);
  if (avio) { av_free(avio->buffer); avio_context_free(&avio); }
  av_free(clip);
  debugLogFlush();
  return played;
}

#else  /* !WMW2_VIDEO */

void wmw2_movie_init(void (*present)(void), int w, int h) {
  (void)present; (void)w; (void)h;
  debugPrintf("movie: built without video support (WMW2_VIDEO is 0)\n");
}
int wmw2_movie_play(const char *p) { (void)p; return 0; }
int wmw2_movie_mix_s16(int16_t *d, int f, int c) { (void)d; (void)f; (void)c; return 0; }
int wmw2_movie_is_playing(void) { return 0; }

#endif
