/* rtaudio_c_private.h

   Internal helpers shared between the C API implementation (rtaudio_c.cpp)
   and its unit tests.  This header is NOT installed and is not part of the
   public API.
*/

#ifndef RTAUDIO_C_PRIVATE_H
#define RTAUDIO_C_PRIVATE_H

#include <cstddef>
#include <cstring>
#include <string>

/* Copy an error message into a fixed-size C string buffer.

   This replaces a previous `strncpy( dst, src.c_str(), src.size() - 1 )`,
   which had two bugs:
     - the length was derived from the *source* size, so a message longer
       than the destination overran the buffer;
     - an empty message made `src.size() - 1` wrap to SIZE_MAX.
   The buffer was also left unterminated on truncation.

   This helper always NUL-terminates and never writes past `dstsize` bytes.
   `dstsize` is the full size of the destination buffer (including the space
   for the terminator) and must be greater than zero. */
static inline void rtaudio_copy_error_message( char *dst, size_t dstsize,
                                               const std::string &src )
{
  if ( dst == nullptr || dstsize == 0 )
    return;
  size_t n = src.size();
  if ( n > dstsize - 1 )
    n = dstsize - 1;
  if ( n > 0 )
    memcpy( dst, src.data(), n );
  dst[n] = '\0';
}

#endif /* RTAUDIO_C_PRIVATE_H */
