#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>

#include <SDL.h>

#include "cmdutils.h"

const char program_name[] = "ksplayer";     // 63

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

typedef struct MyAVPacketList {   // 112
  AVPacket pkt;
  struct MyAVPacketList *next;
  int serial;
} MyAVPacketList;

typedef struct PacketQueue {      // 118
  MyAVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  int64_t duration;
  int abort_request;
  int serial;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE  3     // 129
#define SUBPICTURE_QUEUE_SIZE     16    // 130
#define SAMPLE_QUEUE_SIZE         9     // 131
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))   // 132

typedef struct AudioParams {    // 134

} AudioParams;

typedef struct Clock {          // 143
  double pts;           /* clock base */
  double pts_drift;     /* clock base minus time at which we updated the clock */
  double last_updated;
  double speed;
  int serial;           /* clock is based on a packet with this serial */
  int paused;
  int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers */
typedef struct Frame {          // 154
  AVFrame *frame;
} Frame;

typedef struct FrameQueue {     // 169
  Frame queue[FRAME_QUEUE_SIZE];
  int rindex;
  int windex;
  int size;
  int max_size;
  int keep_last;
  int rindex_shown;
  SDL_mutex *mutex;
  SDL_cond *cond;
  PacketQueue *pktq;
} FrameQueue;

typedef struct Decoder {        // 188
  AVPacket pkt;
  PacketQueue *queue;
  AVCodecContext *avctx;
  int pkt_serial;
  int finished;
  int packet_pending;
  SDL_cond *empty_queue_cond;
  int64_t start_pts;
  AVRational start_pts_tb;
  int64_t next_pts;
  AVRational netx_pts_tb;
  SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoState {
  SDL_Thread *read_tid;   // 204
  AVInputFormat *iformat; // 205
  int abort_request;      // 206
  int force_refresh;      // 207
  int paused;             // 208
  int last_paused;        // 209

  AVFormatContext *ic;    // 216

  Clock audclk;           // 219
  Clock vidclk;           // 220
  Clock extclk;           // 221

  FrameQueue pictq;       // 223
  FrameQueue subpq;       // 224
  FrameQueue sampq;       // 225

  Decoder auddec;         // 227
  Decoder viddec;         // 228

  int audio_stream;       // 231

  int audio_clock_serial; // 236

  AVStream *audio_st;     // 241
  PacketQueue audioq;     // 242

  unsigned int audio_buf_size;    /* in bytes */    // 246
  int audio_buf_index;            /* in bytes */    // 248

  struct AudioParams audio_tgt;   // 256

  enum ShowMode {          // 261
    SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
  } show_mode;

  int subtitle_stream;    // 276
  PacketQueue subtitleq;  // 278

  int video_stream;       // 283

  AVStream *video_st;     // 284
  PacketQueue videoq;     // 285
  int eof;                // 289

  char *filename;         // 291
  int width, height, xleft, ytop;   // 292

  SDL_cond *continue_read_thread;   // 306
} VideoState;

/* options specified by the user */
static AVInputFormat *file_iformat;   // 310
static const char *input_filename;    // 311
static const char *window_title;      // 312
static int default_width  = 640;      // 313
static int default_height = 480;      // 314
static int screen_width = 0;          // 315
static int screen_height = 0;         // 316
static int audio_disable;             // 317
static int video_disable;             // 318
static int display_disable;           // 322
static int borderless;                // 323
static int show_status = 1;           // 325
static enum ShowMode show_mode = SHOW_MODE_NONE;    // 339

/* current context */
static int is_full_screen;            // 355

static AVPacket flush_pkt;            // 358

static SDL_Window *window;            // 362
static SDL_Renderer *renderer;        // 363

static SDL_AudioDeviceID audio_dev;   // 365

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)              // 422  *
{
  MyAVPacketList *pkt1;

  if (q->abort_request)
    return -1;

  pkt1 = av_malloc(sizeof(MyAVPacketList));
  if (!pkt1)
    return -1;
  pkt1->pkt = *pkt;
  pkt1->next = NULL;
  if (pkt == &flush_pkt)
    q->serial++;
  pkt1->serial = q->serial;

  if (!q->last_pkt)
    q->first_pkt = pkt1;
  else
    q->last_pkt->next = pkt1;
  q->last_pkt = pkt1;
  q->nb_packets++;
  q->size += pkt1->pkt.size + sizeof(*pkt1);
  q->duration += pkt1->pkt.duration;
  /* XXX: should duplicate packet data in DV case */
  SDL_CondSignal(q->cond);
  return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)                      // 451  *
{
  int ret;

  SDL_LockMutex(q->mutex);
  ret = packet_queue_put_private(q, pkt);
  SDL_UnlockMutex(q->mutex);

  if (pkt != &flush_pkt && ret < 0)
    av_packet_unref(pkt);

  return ret;
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q) {                                  // 476  *
  memset(q, 0, sizeof(PacketQueue));
  q->mutex = SDL_CreateMutex();
  if (!q->mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  q->cond = SDL_CreateCond();
  if (!q->cond) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  q->abort_request = 1;
  return 0;
}

static void packet_queue_start(PacketQueue *q)    // 529  *
{
  SDL_LockMutex(q->mutex);
  q->abort_request = 0;
  packet_queue_put_private(q, &flush_pkt);
  SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet. */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)    // 538
{
  MyAVPacketList *pkt1;
  int ret;

  SDL_LockMutex(q->mutex);

  for (;;) {
    if (q->abort_request) {
      ret = -1;
      break;
    }

    pkt1 = q->first_pkt;
    if (pkt1) {
      q->first_pkt = pkt1->next;
      if (!q->first_pkt)
        q->last_pkt = NULL;
      q->nb_packets--;
      q->size -= pkt1->pkt.size + sizeof(*pkt1);
      q->duration -= pkt1->pkt.duration;
      *pkt = pkt1->pkt;
      if (serial)
        *serial = pkt1->serial;
      av_free(pkt1);
      ret = 1;
      break;
    }
    else if (!block) {
      ret = 0;
      break;
    }
    else {
      SDL_CondWait(q->cond, q->mutex);
    }
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond)   // 576  *
{
  memset(d, 0, sizeof(Decoder));
  d->avctx = avctx;
  d->queue = queue;
  d->empty_queue_cond = empty_queue_cond;
  d->start_pts = AV_NOPTS_VALUE;
  d->pkt_serial = -1;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)    // 585
{
  int ret = AVERROR(EAGAIN);

  for (;;) {
    AVPacket pkt;

    if (d->queue->serial == d->pkt_serial) {
      do {
        if (d->queue->abort_request)
          return -1;

        switch (d->avctx->codec_type) {
          case AVMEDIA_TYPE_VIDEO:
            // 6-3. decode send-receive pair
            ret = avcodec_receive_frame(d->avctx, frame);
            break;
        }
      } while (ret != AVERROR(EAGAIN));
    }

    do {
      if (d->queue->nb_packets == 0)
        SDL_CondSignal(d->empty_queue_cond);
      if (d->packet_pending) {
        av_packet_move_ref(&pkt, &d->pkt);
        d->packet_pending = 0;
      }
      else {
        if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
          return -1;
      }
    } while (d->queue->serial != d->pkt_serial);

    if (pkt.data == flush_pkt.data) {

    }
    else {
      if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {

      }
      else {
        // 6-2. decode send-receive pair
        if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {

        }
      }
      av_packet_unref(&pkt);
    }
  }
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)    // 685  *
{
  int i;
  memset(f, 0, sizeof(FrameQueue));
  if (!(f->mutex = SDL_CreateMutex())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  if (!(f->cond = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  f->pktq = pktq;
  f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
  f->keep_last = !!keep_last;
  for (i = 0; i < f->max_size; i++)
    if (!(f->queue[i].frame = av_frame_alloc()))
      return AVERROR(ENOMEM);
  return 0;
}

static Frame *frame_queue_peek_writable(FrameQueue *f)    // 740  *
{
  /* wait until we have space to put a new frame */
  SDL_LockMutex(f->mutex);
  while (f->size >= f->max_size && !f->pktq->abort_request) {
    SDL_CondWait(f->cond, f->mutex);
  }
  SDL_UnlockMutex(f->mutex);

  if (f->pktq->abort_request)
    return NULL;

  return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)    // 756
{

}

static void frame_queue_push(FrameQueue *f)       // 772  *
{
  if (++f->windex == f->max_size)
    f->windex = 0;
  SDL_LockMutex(f->mutex);
  f->size++;
  SDL_CondSignal(f->cond);
  SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
  return f->size - f->rindex_shown;
}

static void video_image_display(VideoState *is)   // 957
{

}

static void video_audio_display(VideoState *is)   // 1043
{

}

static void stream_close(VideoState *is)          // 1242
{

}

static void do_exit(VideoState *is)               // 1279
{
  if (is) {
    stream_close(is);
  }

  SDL_Quit();
  exit(0);
}

static int video_open(VideoState *is)             // 1313   *
{
  int w, h;

  if (screen_width) {
    w = screen_width;
    h = screen_height;
  }
  else {
    w = default_width;
    h = default_height;
  }

  if (!window_title)
    window_title = input_filename;
  SDL_SetWindowTitle(window, window_title);

  SDL_SetWindowSize(window, w, h);
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  if (is_full_screen)
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(window);

  is->width = w;
  is->height = h;

  return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is)         // 1342   *
{
  if (!is->width)
    video_open(is);

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
    // 12-1. video for audio output
    video_audio_display(is);
  else if (is->video_st)
    // 12-2. video-image output
    video_image_display(is);
  SDL_RenderPresent(renderer);
}

static void set_clock_at(Clock *c, double pts, int serial, double time)    // 1368   *
{
  c->pts = pts;
  c->last_updated = time;
  c->pts_drift = c->pts - time;
  c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)   // 1376   *
{
  double time = av_gettime_relative() / 1000000.0;
  set_clock_at(c, pts, serial, time);
}

static void init_clock(Clock *c, int *queue_serial)       // 1388   *
{
  c->speed = 1.0;
  c->paused = 0;
  c->queue_serial = queue_serial;
  set_clock(c, NAN, -1);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)   // 1556
{
  VideoState *is = opaque;

  if (is->video_st) {
retry:
    if (frame_queue_nb_remaining(&is->pictq) == 0) {
      // nothing to do, no picture to display in the queue
    } else {
      is->force_refresh = 1;
    }
  }

  if (is->video_st) {
    /* 11. display picture */
    if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
      video_display(is);
  }
  is->force_refresh = 0;
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)  // 1714
{
  Frame *vp;

  if (!(vp = frame_queue_peek_writable(&is->pictq)))
    return -1;

  av_frame_move_ref(vp->frame, src_frame);
  // 7-1. move decoded frame to frame queue
  frame_queue_push(&is->pictq);
  return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)    // 1745
{
  int got_picture;

  // 6-1. decode a frame
  if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
    return -1;

  if (got_picture) {

  }
}

static int audio_thread(void *arg)    // 2003
{
  VideoState *is = arg;
  AVFrame *frame = av_frame_alloc();
  int got_frame = 0;
  int ret;

  do {
    // 14-1. decode a frame
    if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
      goto the_end;

    if (got_frame) {
      // 14-2. queue frame
      frame_queue_push(&is->sampq);
    }

  } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:
  av_frame_free(&frame);
  return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), void *arg)  // 2090
{
  packet_queue_start(d->queue);
  // 5. create a decoder thread
  d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);
}

static int video_thread(void *arg)  // 2101
{
  VideoState *is = arg;
  AVFrame *frame = av_frame_alloc();
  double pts;
  double duration;
  int ret;

  if (!frame) {
    return AVERROR(ENOMEM);
  }

  for (;;) {
    // 6. get decoded frame
    ret = get_video_frame(is, frame);
    if (ret < 0)
      goto the_end;
    if (!ret)
      continue;
    // 7. queue frame
    ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
  }

the_end:
  av_frame_free(&frame);
  return 0;
}

/**
 *  return re-sampled audio data
 */
static int audio_decode_frame(VideoState *is)   // 2313
{

}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)    // 2426
{
  VideoState *is = opaque;
  int audio_size;

  while (len > 0) {
    if (is->audio_buf_index >= is->audio_buf_size) {
      // 13-1-1. re-sampling audio
      audio_size = audio_decode_frame(is);
    }
  }
}

static int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params) // 2469
{
  SDL_AudioSpec wanted_spec, spec;

  // 13-1. SDL audio callback
  wanted_spec.callback = sdl_audio_callback;

  // 13-2. open audio device
  while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {

  }

  return spec.size;
}

/* open a given stream. return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)  // 2543
{
  AVFormatContext *ic = is->ic;
  AVCodecContext *avctx;
  AVCodec *codec;
  int sample_rate, nb_channels;
  int64_t channel_layout;
  int ret = 0;

  avctx = avcodec_alloc_context3(NULL);
  if (!avctx)
    return AVERROR(ENOMEM);

  ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
  if (ret < 0)
    goto fail;

  codec = avcodec_find_decoder(avctx->codec_id);

  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      /* prepare audio output */
      // 13. audio open
      if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
        goto fail;

      is->audio_stream = stream_index;
      is->audio_st = ic->streams[stream_index];

      decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);

      // 14. start decoder (thread fn: audio_thread)
      if ((ret = decoder_start(&is->auddec, audio_thread, is)) < 0)
        goto out;
      SDL_PauseAudioDevice(audio_dev, 0);
      break;
    case AVMEDIA_TYPE_VIDEO:
      is->video_stream = stream_index;
      is->video_st = ic->streams[stream_index];

      decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
      // 4. start decoder (thread fn: video_thread)
      if ((ret = decoder_start(&is->viddec, video_thread, is)) < 0)
        goto out;
      break;
    default:
      break;
  }
  goto out;

fail:
  avcodec_free_context(&avctx);
out:

  return ret;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)     // 2725
{
  VideoState *is = arg;
  AVFormatContext *ic = NULL;
  int err, ret;
  int st_index[AVMEDIA_TYPE_NB];
  AVPacket pkt1, *pkt = &pkt1;
  int64_t stream_start_time;
  SDL_mutex *wait_mutex = SDL_CreateMutex();

  if (!wait_mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  memset(st_index, -1, sizeof(st_index));

  ic = avformat_alloc_context();
  if (!ic) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
  if (err < 0) {
    print_error(is->filename, err);
    ret = -1;
    goto fail;
  }

  is->ic = ic;

  if (show_status)
    av_dump_format(ic, 0, is->filename, 0);

  if (!video_disable)
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);

  if (!audio_disable)
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO], st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);

  is->show_mode = show_mode;

  // 3. open the streams
  if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
    stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
  }

  ret = -1;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
  }
  if (is->show_mode == SHOW_MODE_NONE)
    is->show_mode = ret > 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

  for (;;) {
    if (is->abort_request)
      break;
    if (is->paused != is->last_paused) {

    }

    ret = av_read_frame(ic, pkt);
    if (ret < 0) {
      if (ic->pb && ic->pb->error)
        break;
    }
    else {
      is->eof = 0;
    }

    /* check if packet is in play range specified by user. then queue, otherwise discard */
    // 3-1. packet queue put
    if (pkt->stream_index == is->audio_stream) {
      packet_queue_put(&is->audioq, pkt);
    }
    else if (pkt->stream_index == is->video_stream) {
      packet_queue_put(&is->videoq, pkt);
    }
    else if (pkt->stream_index == is->subtitle_stream) {
      packet_queue_put(&is->subtitleq, pkt);
    }
    else {
      av_packet_unref(pkt);
    }
  }

  ret = 0;
fail:
  return 0;
}

static VideoState *stream_open(const char *filename, AVInputFormat *iformat)  // 3047
{
  VideoState *is;

  is = av_mallocz(sizeof(VideoState));
  if (!is)
    return NULL;
  is->filename = av_strdup(filename);
  if (!is->filename)
    goto fail;
  is->iformat = iformat;
  is->ytop    = 0;
  is->xleft   = 0;

  /* start video display */
  if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
    goto fail;
  if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
    goto fail;
  if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
    goto fail;

  if (packet_queue_init(&is->videoq) < 0 ||
      packet_queue_init(&is->audioq) < 0 ||
      packet_queue_init(&is->subtitleq) < 0)
    goto fail;

  if (!(is->continue_read_thread = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    goto fail;
  }

  init_clock(&is->vidclk, &is->videoq.serial);
  init_clock(&is->audclk, &is->audioq.serial);
  init_clock(&is->extclk, &is->extclk.serial);
  is->audio_clock_serial = -1;

  // 2. create a thread
  is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
  if (!is->read_tid) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
    stream_close(is);
    return NULL;
  }
  return is;
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event)   // 3199
{
  double remaining_time = 0.0;
  SDL_PumpEvents();
  while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    remaining_time = REFRESH_RATE;
    if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
      // 10. video refresh
      video_refresh(is, &remaining_time);
    SDL_PumpEvents();
  }
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)  // 3244
{
  SDL_Event event;

  for (;;) {
    // 9. video refresh
    refresh_loop_wait_event(cur_stream, &event);
    switch (event.type) {
      case SDL_QUIT:
        do_exit(cur_stream);
        break;
      default:
        break;
    }
  }
}

int main(int argc, char *argv[])  // 3645
{
  int flags;
  VideoState *is;

  input_filename = "little.mkv";

  if (display_disable) {
    video_disable = 1;
  }
  flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (SDL_Init(flags)) {
    av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
    av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
    exit(1);
  }

  if (!display_disable) {
    int flags = SDL_WINDOW_HIDDEN;
    if (borderless)
      flags |= SDL_WINDOW_BORDERLESS;
    else
      flags |= SDL_WINDOW_RESIZABLE;
    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
    if (window) {
      renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      if (!renderer) {
        av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, 0);
      }
    }
  }

  // 1. open stream
  is = stream_open(input_filename, file_iformat);
  if (!is) {
    av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
    do_exit(NULL);
  }

  // 8. event loop
  event_loop(is);

  /* never returns */

  return 0;
}
