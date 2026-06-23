/******************************************/
/*
  convtest.cpp
  Unit tests for RtApi's format-conversion and byte-swap routines
  (convertBuffer / byteSwapBuffer). These are protected members, so the
  tests reach them through a minimal RtApi subclass rather than by modifying
  the library. No audio hardware or backend is required, so this runs in CI
  on every platform.

  In particular this exercises the signed-integer conversions that previously
  contained undefined behaviour (left-shifting negative values), confirming
  the value-preserving results.
*/
/******************************************/

#include "RtAudio.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK( cond )                                                      \
  do {                                                                     \
    ++g_checks;                                                            \
    if ( !( cond ) ) {                                                     \
      ++g_failures;                                                        \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                << std::endl;                                              \
    }                                                                      \
  } while ( 0 )

// Minimal concrete RtApi used only to reach the protected conversion helpers.
class TestApi : public RtApi
{
public:
  RtAudio::Api getCurrentApi( void ) override { return RtAudio::RTAUDIO_DUMMY; }
  RtAudioErrorType startStream( void ) override { return RTAUDIO_NO_ERROR; }
  RtAudioErrorType stopStream( void ) override { return RTAUDIO_NO_ERROR; }
  RtAudioErrorType abortStream( void ) override { return RTAUDIO_NO_ERROR; }

  // Convert `frames` of interleaved audio with `channels` channels. By default
  // channel c maps to channel c; pass outMap to remap output channels.
  void convert( char *out, char *in, RtAudioFormat inFmt, RtAudioFormat outFmt,
                int channels, unsigned int frames,
                const std::vector<int> *outMap = nullptr )
  {
    stream_.bufferSize = frames;
    stream_.mode = OUTPUT; // avoid the DUPLEX output-clear branch
    ConvertInfo info;
    info.channels = channels;
    info.inJump = channels;
    info.outJump = channels;
    info.inFormat = inFmt;
    info.outFormat = outFmt;
    for ( int c = 0; c < channels; c++ ) {
      info.inOffset.push_back( c );
      info.outOffset.push_back( outMap ? ( *outMap )[c] : c );
    }
    convertBuffer( out, in, info );
  }

  void swap( char *buf, unsigned int samples, RtAudioFormat fmt )
  {
    byteSwapBuffer( buf, samples, fmt );
  }
};

// ---------------------------------------------------------------------------
// byteSwapBuffer: 16/32/64-bit endianness reversal.
// ---------------------------------------------------------------------------
static void testByteSwap()
{
  TestApi t;

  uint8_t b16[4] = { 0x01, 0x02, 0x03, 0x04 };
  t.swap( (char *)b16, 2, RTAUDIO_SINT16 );
  CHECK( b16[0] == 0x02 && b16[1] == 0x01 && b16[2] == 0x04 && b16[3] == 0x03 );

  uint8_t b32[4] = { 0x01, 0x02, 0x03, 0x04 };
  t.swap( (char *)b32, 1, RTAUDIO_SINT32 );
  CHECK( b32[0] == 0x04 && b32[1] == 0x03 && b32[2] == 0x02 && b32[3] == 0x01 );

  uint8_t b64[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  t.swap( (char *)b64, 1, RTAUDIO_FLOAT64 );
  CHECK( b64[0] == 8 && b64[1] == 7 && b64[2] == 6 && b64[3] == 5 &&
         b64[4] == 4 && b64[5] == 3 && b64[6] == 2 && b64[7] == 1 );
}

// ---------------------------------------------------------------------------
// Known-value conversions for the formerly-UB signed left-shift paths.
// ---------------------------------------------------------------------------
static void testKnownValueConversions()
{
  TestApi t;
  const unsigned int N = 5;

  // SINT8 -> SINT32 : value << 24
  {
    int8_t in[N] = { -128, -1, 0, 1, 127 };
    int32_t out[N];
    t.convert( (char *)out, (char *)in, RTAUDIO_SINT8, RTAUDIO_SINT32, 1, N );
    CHECK( out[0] == -2147483647 - 1 ); // -128 << 24
    CHECK( out[1] == -16777216 );       //   -1 << 24
    CHECK( out[2] == 0 );
    CHECK( out[3] == 16777216 );        //    1 << 24
    CHECK( out[4] == 2130706432 );      //  127 << 24
  }

  // SINT16 -> SINT32 : value << 16
  {
    int16_t in[N] = { -32768, -1, 0, 1, 32767 };
    int32_t out[N];
    t.convert( (char *)out, (char *)in, RTAUDIO_SINT16, RTAUDIO_SINT32, 1, N );
    CHECK( out[0] == -2147483647 - 1 ); // -32768 << 16
    CHECK( out[1] == -65536 );          //     -1 << 16
    CHECK( out[2] == 0 );
    CHECK( out[3] == 65536 );           //      1 << 16
    CHECK( out[4] == 2147418112 );      //  32767 << 16
  }

  // SINT8 -> SINT16 : value << 8
  {
    int8_t in[N] = { -128, -1, 0, 1, 127 };
    int16_t out[N];
    t.convert( (char *)out, (char *)in, RTAUDIO_SINT8, RTAUDIO_SINT16, 1, N );
    CHECK( out[0] == -32768 ); // -128 << 8
    CHECK( out[1] == -256 );   //   -1 << 8
    CHECK( out[2] == 0 );
    CHECK( out[3] == 256 );    //    1 << 8
    CHECK( out[4] == 32512 );  //  127 << 8
  }
}

// ---------------------------------------------------------------------------
// Round-trip conversions: A -> B -> A should be identity (or a documented
// truncation). Buffers are sized for the widest format (8 bytes/sample).
// ---------------------------------------------------------------------------
static void testRoundTrips()
{
  TestApi t;
  const unsigned int N = 7;
  const int16_t s16[N] = { -32768, -12345, -1, 0, 1, 12345, 32767 };
  const int8_t s8[N] = { -128, -100, -1, 0, 1, 100, 127 };

  unsigned char mid[N * 8];
  int16_t back16[N];
  int8_t back8[N];

  // SINT16 -> SINT32 -> SINT16
  t.convert( (char *)mid, (char *)s16, RTAUDIO_SINT16, RTAUDIO_SINT32, 1, N );
  t.convert( (char *)back16, (char *)mid, RTAUDIO_SINT32, RTAUDIO_SINT16, 1, N );
  for ( unsigned int i = 0; i < N; i++ ) CHECK( back16[i] == s16[i] );

  // SINT8 -> SINT32 -> SINT8
  t.convert( (char *)mid, (char *)s8, RTAUDIO_SINT8, RTAUDIO_SINT32, 1, N );
  t.convert( (char *)back8, (char *)mid, RTAUDIO_SINT32, RTAUDIO_SINT8, 1, N );
  for ( unsigned int i = 0; i < N; i++ ) CHECK( back8[i] == s8[i] );

  // SINT16 -> SINT24 -> SINT16 (Int24 layout is internal; round-trip only)
  t.convert( (char *)mid, (char *)s16, RTAUDIO_SINT16, RTAUDIO_SINT24, 1, N );
  t.convert( (char *)back16, (char *)mid, RTAUDIO_SINT24, RTAUDIO_SINT16, 1, N );
  for ( unsigned int i = 0; i < N; i++ ) CHECK( back16[i] == s16[i] );

  // SINT16 -> FLOAT32 -> SINT16 (16-bit fits exactly in a float mantissa)
  t.convert( (char *)mid, (char *)s16, RTAUDIO_SINT16, RTAUDIO_FLOAT32, 1, N );
  t.convert( (char *)back16, (char *)mid, RTAUDIO_FLOAT32, RTAUDIO_SINT16, 1, N );
  for ( unsigned int i = 0; i < N; i++ ) CHECK( back16[i] == s16[i] );

  // SINT16 -> FLOAT64 -> SINT16
  t.convert( (char *)mid, (char *)s16, RTAUDIO_SINT16, RTAUDIO_FLOAT64, 1, N );
  t.convert( (char *)back16, (char *)mid, RTAUDIO_FLOAT64, RTAUDIO_SINT16, 1, N );
  for ( unsigned int i = 0; i < N; i++ ) CHECK( back16[i] == s16[i] );

  // SINT8 -> FLOAT32 -> SINT8
  t.convert( (char *)mid, (char *)s8, RTAUDIO_SINT8, RTAUDIO_FLOAT32, 1, N );
  t.convert( (char *)back8, (char *)mid, RTAUDIO_FLOAT32, RTAUDIO_SINT8, 1, N );
  for ( unsigned int i = 0; i < N; i++ ) CHECK( back8[i] == s8[i] );
}

// ---------------------------------------------------------------------------
// Channel mapping: the in/out offset tables remap channels.
// ---------------------------------------------------------------------------
static void testChannelMapping()
{
  TestApi t;
  const unsigned int frames = 3;
  // Interleaved stereo: frame i = {Li, Ri}
  int16_t in[frames * 2] = { 10, 20, 11, 21, 12, 22 };
  int16_t out[frames * 2];
  std::vector<int> swapLR = { 1, 0 }; // write L into slot 1, R into slot 0

  t.convert( (char *)out, (char *)in, RTAUDIO_SINT16, RTAUDIO_SINT16, 2, frames,
             &swapLR );
  for ( unsigned int i = 0; i < frames; i++ ) {
    CHECK( out[i * 2 + 0] == in[i * 2 + 1] ); // R landed in slot 0
    CHECK( out[i * 2 + 1] == in[i * 2 + 0] ); // L landed in slot 1
  }
}

int main()
{
  testByteSwap();
  testKnownValueConversions();
  testRoundTrips();
  testChannelMapping();

  if ( g_failures == 0 ) {
    std::cout << "All " << g_checks << " checks passed." << std::endl;
    return 0;
  }
  std::cerr << g_failures << " of " << g_checks << " checks FAILED."
            << std::endl;
  return 1;
}
