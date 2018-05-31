#include <string.h>

#include <libavutil/error.h>

#include <SDL.h>
#include <SDL_thread.h>
#include <libavutil/log.h>
#include <libavutil/dict.h>

#include "cmdutils.h"

AVDictionary *format_opts;    // 73

void print_error(const char *filename, int err)       // 1081
{
  char errbuf[128];
  const char *errbuf_ptr = errbuf;

  if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
    errbuf_ptr = strerror(AVUNERROR(err));
  av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}