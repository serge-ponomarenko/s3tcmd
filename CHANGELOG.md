# Changelog

## 0.6.0 (2026-11-26)

First public release.

- Browse AWS S3 from Total Commander 64-bit: `\\\S3\<profile>\<bucket>\...`, through the AWS CLI
  (all CLI authentication methods; no stored credentials). `PinnedBuckets` for buckets that
  `list-buckets` does not show.
- Download (F5, F3/F4/Enter) and upload, batched into few `aws s3 cp` calls (whole folders as one
  `--recursive` call), with progress and Cancel.
- F7 folders, F8 delete (`delete-objects`, 1000 keys per call), F6/Shift+F6/F2 server-side
  move/rename/copy within a profile, folders in one call.
- Background transfers (TC's *Background* button).
- Listing cache in memory and on disk, Ctrl+R detection, commands `refresh`, `refresh-all`,
  `prefetch`.
- Space (folder size), the F5 pre-scan and search fetch a whole folder tree with one listing,
  with the plugin's own progress window and Cancel.
- `ReadOnly` globally or per profile.
- Reversible mapping of key characters that Windows does not allow in file names.
- `s3tcmd-clip.exe`: copy `s3://` paths (`em_s3_copy_uri`) and presigned URLs (`em_s3_presign`) from
  TC user commands, e.g. bound to Ctrl+Shift+C.
- Go to an `s3://` URI from the address bar (`\\\S3\s3://bucket/key`) or with `cd s3://...` /
  `go s3://...`.
- Install package `s3tcmd-<version>.zip` for TC's plugin auto-installer; the plugin offers to add
  its user commands to `usercmd.ini` (with a backup); command `setup`.
