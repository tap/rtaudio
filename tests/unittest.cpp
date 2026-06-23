/******************************************/
/*
  unittest.cpp
  Dependency-free unit tests for the portable parts of RtAudio: the
  API-name lookup tables (C++ and C entry points) and the C-API error
  message copy helper.  These tests do not require any audio hardware or
  backend and run under the dummy API, so they are safe to execute in CI
  on every platform.
*/
/******************************************/

#include "RtAudio.h"
#include "rtaudio_c.h"
#include "../rtaudio_c_private.h"

#include <cstring>
#include <iostream>
#include <string>
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

// ---------------------------------------------------------------------------
// rtaudio_copy_error_message: this is the regression test for the historical
// strncpy buffer overflow / size_t underflow in rtaudio_c.cpp.
// ---------------------------------------------------------------------------
static void testErrorMessageCopy()
{
  // Empty source must not underflow and must produce an empty C string.
  {
    char buf[8];
    memset( buf, 'x', sizeof( buf ) );
    rtaudio_copy_error_message( buf, sizeof( buf ), std::string() );
    CHECK( buf[0] == '\0' );
    CHECK( strlen( buf ) == 0 );
  }

  // A message that fits is copied verbatim and NUL-terminated.
  {
    char buf[16];
    memset( buf, 'x', sizeof( buf ) );
    rtaudio_copy_error_message( buf, sizeof( buf ), std::string( "hello" ) );
    CHECK( std::string( buf ) == "hello" );
  }

  // A message longer than the buffer is truncated to dstsize-1 and stays
  // NUL-terminated (no overflow).
  {
    char buf[8];
    memset( buf, 'x', sizeof( buf ) );
    rtaudio_copy_error_message( buf, sizeof( buf ),
                                std::string( "0123456789ABCDEF" ) );
    CHECK( strlen( buf ) == sizeof( buf ) - 1 );
    CHECK( std::string( buf ) == "0123456" );
    CHECK( buf[sizeof( buf ) - 1] == '\0' );
  }

  // A message whose length is exactly dstsize-1 fits with the terminator.
  {
    char buf[6];
    memset( buf, 'x', sizeof( buf ) );
    rtaudio_copy_error_message( buf, sizeof( buf ), std::string( "12345" ) );
    CHECK( std::string( buf ) == "12345" );
    CHECK( buf[5] == '\0' );
  }

  // Embedded NULs in the source must not be mistaken for the string end:
  // exactly the requested bytes are copied.
  {
    char buf[8];
    memset( buf, 'x', sizeof( buf ) );
    std::string src( "ab", 2 );
    src.push_back( '\0' );
    src += "cd"; // length 5, contains an interior NUL
    rtaudio_copy_error_message( buf, sizeof( buf ), src );
    CHECK( memcmp( buf, "ab\0cd", 5 ) == 0 );
    CHECK( buf[5] == '\0' );
  }

  // Degenerate destinations must be handled without writing out of bounds.
  {
    rtaudio_copy_error_message( nullptr, 0, std::string( "x" ) );
    char one[1];
    one[0] = 'z';
    rtaudio_copy_error_message( one, 0, std::string( "x" ) );
    CHECK( one[0] == 'z' ); // untouched when dstsize == 0
    rtaudio_copy_error_message( one, 1, std::string( "x" ) );
    CHECK( one[0] == '\0' ); // only room for the terminator
  }
}

// ---------------------------------------------------------------------------
// C++ API-name lookups round-trip for every compiled API.
// ---------------------------------------------------------------------------
static void testCxxApiNames()
{
  std::vector<RtAudio::Api> apis;
  RtAudio::getCompiledApi( apis );
  CHECK( !apis.empty() ); // at least the dummy API is always compiled

  for ( RtAudio::Api api : apis ) {
    std::string name = RtAudio::getApiName( api );
    std::string display = RtAudio::getApiDisplayName( api );
    CHECK( !name.empty() );
    CHECK( !display.empty() );
    CHECK( RtAudio::getCompiledApiByName( name ) == api );
    CHECK( RtAudio::getCompiledApiByDisplayName( display ) == api );
  }

  // Out-of-range values are handled gracefully.
  CHECK( RtAudio::getApiName( (RtAudio::Api)-1 ).empty() );
  CHECK( RtAudio::getApiName( RtAudio::NUM_APIS ).empty() );
  CHECK( RtAudio::getApiDisplayName( (RtAudio::Api)-1 ) == "Unknown" );

  // Unknown names resolve to UNSPECIFIED.
  CHECK( RtAudio::getCompiledApiByName( "not-an-api" ) == RtAudio::UNSPECIFIED );
  CHECK( RtAudio::getCompiledApiByDisplayName( "not-an-api" ) ==
         RtAudio::UNSPECIFIED );

  CHECK( !RtAudio::getVersion().empty() );
}

// ---------------------------------------------------------------------------
// C API-name lookups agree with the C++ ones and bounds-check correctly.
// ---------------------------------------------------------------------------
static void testCApiNames()
{
  unsigned int n = rtaudio_get_num_compiled_apis();
  CHECK( n >= 1 );

  const rtaudio_api_t *compiled = rtaudio_compiled_api();
  CHECK( compiled != nullptr );

  for ( unsigned int i = 0; i < n; i++ ) {
    rtaudio_api_t api = compiled[i];
    const char *name = rtaudio_api_name( api );
    const char *display = rtaudio_api_display_name( api );
    CHECK( name != nullptr );
    CHECK( display != nullptr );
    // Round-trips through the C lookup.
    CHECK( rtaudio_compiled_api_by_name( name ) == api );
    // Agrees with the C++ table.
    CHECK( RtAudio::getApiName( (RtAudio::Api)api ) == name );
  }

  // Out-of-range API ids: name() returns NULL, display_name() returns the
  // documented "Unknown" sentinel rather than reading out of bounds.
  CHECK( rtaudio_api_name( (rtaudio_api_t)-1 ) == nullptr );
  CHECK( rtaudio_api_name( (rtaudio_api_t)9999 ) == nullptr );
  CHECK( std::strcmp( rtaudio_api_display_name( (rtaudio_api_t)-1 ),
                      "Unknown" ) == 0 );

  CHECK( rtaudio_version() != nullptr );
}

// ---------------------------------------------------------------------------
// C API instance lifecycle: create/query/destroy without opening a stream
// (no audio device required).
// ---------------------------------------------------------------------------
static void testCApiLifecycle()
{
  // UNSPECIFIED selects the first compiled API.
  rtaudio_t audio = rtaudio_create( RTAUDIO_API_UNSPECIFIED );
  CHECK( audio != nullptr );
  if ( !audio )
    return;

  // A freshly created instance has no error and no open/running stream.
  CHECK( rtaudio_error_type( audio ) == RTAUDIO_ERROR_NONE );
  CHECK( rtaudio_error( audio ) == nullptr );
  CHECK( rtaudio_is_stream_open( audio ) == 0 );
  CHECK( rtaudio_is_stream_running( audio ) == 0 );

  // current_api() is a valid enum value.
  rtaudio_api_t api = rtaudio_current_api( audio );
  CHECK( api >= 0 && api < RTAUDIO_API_NUM );

  // device_count() must not crash and is non-negative.
  CHECK( rtaudio_device_count( audio ) >= 0 );

  rtaudio_destroy( audio );
}

int main()
{
  testErrorMessageCopy();
  testCxxApiNames();
  testCApiNames();
  testCApiLifecycle();

  if ( g_failures == 0 ) {
    std::cout << "All " << g_checks << " checks passed." << std::endl;
    return 0;
  }
  std::cerr << g_failures << " of " << g_checks << " checks FAILED."
            << std::endl;
  return 1;
}
