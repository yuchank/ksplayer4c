#ifndef FFTOOLS_CMDUTILS_H
#define FFTOOLS_CMDUTILS_H

extern AVDictionary *format_opts;

/**
 * Print an error message to stderr, indicating filename and a human
 * readable description of the error code err.
 *
 * If strerror_r() is not available the use of this function in a
 * multithreaded application may be unsafe.
 *
 * @see av_strerror()
 */
void print_error(const char *filename, int err);    // 442


#endif //FFTOOLS_CMDUTILS_H
