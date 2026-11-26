@echo off
rem Copy to tests\it.local.bat (ignored by git) and fill in your values.
rem build.bat calls it before running tests; "build.bat it" / "build.bat itw" then use them.

rem Read-only tests: a readable folder with subfolders and a few small files.
set "S3TCMD_IT_PROFILE=my-profile"
set "S3TCMD_IT_BUCKET=my-bucket"
set "S3TCMD_IT_PREFIX=some/folder/"

rem Write tests: everything is written below <prefix>s3tcmd-it-<pid>-<n>/ and deleted afterwards.
set "S3TCMD_ITW_PROFILE=my-writer-profile"
set "S3TCMD_ITW_BUCKET=my-test-bucket"
set "S3TCMD_ITW_PREFIX=tests/"
rem Optional: a profile that can read but not delete there (checks the AccessDenied report).
set "S3TCMD_ITW_RO_PROFILE="
