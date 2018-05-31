#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>

#include <SDL.h>

#include "cmdutils.h"

typedef struct PacketQueue {    // 118

  int serial;
} PacketQueue;

typedef struct AudioParams {    // 134

} AudioParams;

/* Common struct for handling all types of decoded data and allocated render buffers */
typedef struct Frame {          // 154
  AVFrame *frame;
} Frame;

typedef struct FrameQueue {     // 169
  int rindex_shown;
} FrameQueue;

typedef struct Decoder {        // 188
  PacketQueue *queue;
  AVCodecContext *avctx;
  int pkt_serial;
  SDL_Thread *decoder_tid;
} Decoder;

typedef enum {
  SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
} ShowMode;

typedef struct VideoState {
  SDL_Thread *read_tid;   // 204
  AVInputFormat *iformat; // 205
  int force_refresh;      // 207
  int paused;             // 208

  AVFormatContext *ic;    // 216

  FrameQueue pictq;       // 223
  FrameQueue sampq;       // 225

  Decoder auddec;         // 227
  Decoder viddec;         // 228

  AVStream *audio_st;     // 241

  unsigned int audio_buf_size;    /* in bytes */    // 246
  int audio_buf_index;            /* in bytes */    // 248

  struct AudioParams audio_tgt;   // 256

  ShowMode show_mode;     // 261

  AVStream *video_st;     // 284
  PacketQueue videoq;     // 285

  char *filename;         // 291
} VideoState;

/* options specified by the user */
static AVInputFormat *file_iformat;   // 310
static const char *input_filename;    // 311
static int video_disable;             // 318
static display_disable;               // 322
static int show_status = 1;           // 325

static AVPacket flush_pkt;            // 358

static SDL_AudioDeviceID audio_dev;   // 365

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)                      // 451
{

}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q) {                                  // 476
  memset(q, 0, sizeof(PacketQueue));
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet. */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)    // 538
{

}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)    // 585
{
  int ret = AVERROR(EAGAIN);

  for (;;) {
    AVPacket pkt;
    if (d->queue->serial == d->pkt_serial) {
      do {
        switch (d->avctx->codec_type) {
          case AVMEDIA_TYPE_VIDEO:
            // 6-3. decode send-receive pair
            ret = avcodec_receive_frame(d->avctx, frame);
            break;
        }
      } while (ret != AVERROR(EAGAIN));
    }

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
    }
  }
}

static void frame_queue_push(FrameQueue *f)       // 772
{

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

/* display the current picture, if any */
static void video_display(VideoState *is)         // 1342
{
  if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
    // 12-1. video for audio output
    video_audio_display(is);
  else if (is->video_st)
    // 12-2. video-image output
    video_image_display(is);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)   // 1556
{
  VideoState *is = opaque;

  if (is->video_st) {
    /* 11. display picture */
    if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
      video_display(is);
  }
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)  // 1714
{
  Frame *vp;

  // 7-1. move decoded frame to frame queue
  av_frame_move_ref(vp->frame, src_frame);
}

static int get_video_frame(VideoState *is, AVFrame *frame)    // 1745
{
  int got_picture;

  // 6-1. decode a frame
  if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
    return -1;
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

  for (;;) {
    // 6. get decoded frame
    ret = get_video_frame(is, frame);
    // 7. queue frame
    ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
  }
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
}

/* open a given stream. return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)  // 2543
{
  AVCodecContext *avctx;
  int sample_rate, nb_channels;
  int64_t channel_layout;
  int ret = 0;

  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      /* prepare audio output */
      // 13. audio open
      if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
        goto fail;
      // 14. start decoder (thread fn: audio_thread)
      if ((ret = decoder_start(&is->auddec, audio_thread, is)) < 0)
        goto out;
      SDL_PauseAudioDevice(audio_dev, 0);
      break;
    case AVMEDIA_TYPE_VIDEO:
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

  // 3. open stream
  ret = -1;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
  }

  for (;;) {
    ret = av_read_frame(ic, pkt);

    // 3-1. packet queue put
    packet_queue_put(&is->videoq, pkt);
  }

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

  while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
      // 10. video refresh
      video_refresh(is, &remaining_time);
  }
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)  // 3244
{
  SDL_Event event;

  for (;;) {
    // 9. video refresh
    refresh_loop_wait_event(cur_stream, &event);
  }
}

int main(int argc, char *argv[])  // 3645
{
  VideoState *is;

  input_filename = "little.mkv";

  int x = SHOW_MODE_VIDEO;

  av_log(NULL, AV_LOG_INFO, "%d\n", SHOW_MODE_VIDEO);

  // 1. open stream
  is = stream_open(input_filename, file_iformat);

  // 8. event loop
  event_loop(is);

  return 0;
}
