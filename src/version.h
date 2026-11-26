// The single source of the S3Tcmd version. Used by the C++ code, by res/s3tcmd.rc (file version
// resource) and by build.bat (pluginst.inf and the name of the install zip, read with findstr).
// Keep the format of the S3TCMD_VERSION_STR line: build.bat takes its third token.
#pragma once

#define S3TCMD_VERSION_MAJOR 0
#define S3TCMD_VERSION_MINOR 6
#define S3TCMD_VERSION_PATCH 0
#define S3TCMD_VERSION_STR "0.6.0"

#define S3TCMD_WIDEN2(x) L##x
#define S3TCMD_WIDEN(x) S3TCMD_WIDEN2(x)
#define S3TCMD_VERSION_WSTR S3TCMD_WIDEN(S3TCMD_VERSION_STR)
