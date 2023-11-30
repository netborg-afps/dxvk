#pragma once
#include <assert.h>
#include "log/log.h"
#include "util_error.h"

//#define DXVK_ENABLE_ASSERT

#ifndef DXVK_ENABLE_ASSERT
#define dxvk_assert( x ) void()
#else
#define dxvk_assert( x ) do{ if( !(x) ) { \
    dxvk::Logger::err( dxvk::str::format( "assertion failed: ", #x ) ); \
    throw dxvk::DxvkError(dxvk::str::format( "assertion failed: ", #x )); } } \
  while( false )
#endif
