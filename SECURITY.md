# Security

## How S3Tcmd handles credentials

- S3Tcmd stores **no credentials**. Every AWS request is made by the AWS CLI (`aws.exe`), started
  as a child process with an explicit `--profile`. Authentication (profiles, SSO, assumed roles,
  environment credentials) is entirely the CLI's.
- The plugin writes only these files:
  - `s3tcmd.ini` (settings) next to `wincmd.ini`;
  - the listing cache in `%LOCALAPPDATA%\S3Tcmd\` (bucket, key names, sizes and dates; no
    object contents);
  - `usercmd.ini` sections `em_s3_copy_uri` / `em_s3_presign`, only after you agree (with a backup);
  - a debug log only if `LogFile=` is set in `s3tcmd.ini`. The log contains paths, key names and
    the command lines of `aws.exe`, never credentials. Keep it off unless you troubleshoot.
- Presigned URLs (`em_s3_presign`) grant access to an object to anyone who has the URL, until they
  expire (`PresignExpirySec`, default 1 hour). Share them carefully.
- `ReadOnly=1` (globally or per profile) makes the plugin refuse every write operation.

## Reporting a vulnerability

Please report security problems privately through GitHub: *Security → Report a vulnerability* in
this repository (private vulnerability reporting). Do not open a public issue for them.
