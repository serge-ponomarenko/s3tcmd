# S3Tcmd design notes

How the plugin works, and what was measured about Total Commander (TC) and the AWS CLI along the
way. User-facing behaviour is in the [README](../README.md).

Measured with TC 11.58 x64 (plugin interface 2.11) and AWS CLI 2.x on Windows 11.

## 1. Decisions

| Topic | Decision |
|---|---|
| Plugin type | File system plugin (WFX), 64-bit only (`s3tcmd.wfx64`) |
| Language / toolchain | C++17, MSVC (VS 2019 or later), plain `build.bat` |
| Backend | `aws.exe` (AWS CLI v2) as a child process; no AWS SDK |
| Storage | AWS S3 (other S3-compatible services are untested) |
| Root layout | `\\\S3\<profile>\<bucket>\<key path>` |
| Configuration | `s3tcmd.ini` next to `wincmd.ini`; no settings dialog |
| Dependencies | nlohmann/json (vendored), the WFX SDK header |

Using the CLI instead of an SDK means every authentication method the CLI supports works (SSO,
assumed roles, credential processes), and the plugin never handles credentials. The price is the
start-up cost of `aws.exe` (see 3.2), which shapes the caching and batching design.

## 2. Virtual file system

```
\\\S3\
  ├─ profile-a\              ← AWS CLI profiles (aws configure list-profiles)
  │   ├─ my-bucket\          ← buckets (s3api list-buckets + PinnedBuckets)
  │   │   ├─ folder\         ← CommonPrefixes
  │   │   └─ file.txt        ← Contents
  │   └─ ...
  └─ profile-b\
```

- `\profile\bucket\a\b\c.txt` maps to (profile, bucket, key `a/b/c.txt`).
- Folders are virtual: a prefix exists if any key starts with it. A key ending in `/` is a folder
  marker (created by F7; needed for empty folders).
- `list-buckets` may be denied, and buckets of other accounts are never in it, even if they are
  readable. `PinnedBuckets` per profile lists extra buckets that are always shown.
- **Names:** S3 keys can contain characters that Windows file names cannot (`\ / : * ? " < > |`,
  control characters, a trailing dot or space). `names.*` maps them to look-alike Unicode
  characters (e.g. `:` → `：` U+FF1A) and back; the mapping is reversible and unit-tested. Display
  names therefore never contain an ASCII `:`, which makes `s3:` in a path unambiguous (section 7).
- Keys with empty segments (`/x`, `a//b`) are real in S3. Directory keys inside the plugin are
  key prefixes ending in `/` (`""`, `a/`, `/`, `a//`), never joined names, so they cannot collide.

## 3. AWS CLI

### 3.1 Commands

| Action | Command |
|---|---|
| Profiles | `aws configure list-profiles` |
| Buckets | `aws s3api list-buckets --query "Buckets[].[Name,CreationDate]"` |
| Folder | `aws s3api list-objects-v2 --bucket B --prefix K/ --delimiter / --max-items N` |
| Tree | `aws s3api list-objects-v2 --bucket B --prefix K/ --max-items 20000 [--starting-token T] --query "{c:Contents[].[Key,Size,LastModified],t:NextToken}"` |
| Download | `aws s3 cp s3://B/K <local>`; batches: `--recursive [--exclude "*" --include ...]` |
| Upload | `aws s3 cp <local> s3://B/K`; batches as above |
| Folder marker | `aws s3api put-object --bucket B --key K/` |
| Delete | `aws s3api delete-objects --delete file://<json>` (1000 keys per call) |
| Move / copy in S3 | `aws s3 mv` / `aws s3 cp` (with `--recursive` for folders) |
| Presign | `aws s3 presign s3://B/K --expires-in S --region R` |
| Bucket region | `aws s3api get-bucket-location` (cached) |

Every call gets an explicit `--profile`. The plugin never relies on an inherited `AWS_PROFILE`.

### 3.2 Measured costs

- Process start-up: ~0.75 s; a folder listing end to end: ~1.5–2 s; `configure list-profiles`:
  ~1.7 s (cached like everything else).
- Flat listing throughput: ~1,400–1,800 keys/s (1,000 keys per S3 page).
- `--max-items N` limits `Contents` only; `CommonPrefixes` come on top, so a folder with only
  subfolders is not capped by it.
- The CLI follows region redirects for listings and transfers by itself. `presign` does not: it
  signs offline, for us-east-1 unless `--region` is given, so the region is looked up once
  (`get-bucket-location`; `null` = us-east-1, `EU` = eu-west-1) and cached in
  `%LOCALAPPDATA%\S3Tcmd\bucket-regions.ini`.

### 3.3 Environment of `aws.exe`

| Variable | Why |
|---|---|
| `PYTHONUTF8=1` | Without it the CLI writes to a pipe in the ANSI code page, and characters outside it are lost. |
| `AWS_CLI_FILE_ENCODING=UTF-8` | `file://` parameters are read in the ANSI code page even with `PYTHONUTF8=1`. `delete-objects` with a non-ASCII key then *silently succeeds* for a wrong key (S3 does not report missing keys). |
| `AWS_EC2_METADATA_DISABLED=true` | `DisableImds=1` (default). With no region in the profile and none in TC's environment, the CLI probes the EC2 metadata service and waits for timeouts: a listing took 3.8 s instead of 2.1 s, a tiny `cp` 14.8 s instead of 2.3 s. Set `DisableImds=0` on EC2 with an instance role. |
| `AWS_PAGER=` | No pager. |

Processes are started with `CreateProcessW` (`CREATE_NO_WINDOW`, own command-line quoting,
unit-tested against `CommandLineToArgvW`) in a job object. Stdout and stderr are read on worker
threads; a cancel kills the job.

### 3.4 `aws s3 cp` progress output

Through a pipe, progress records are separated by `\r`:

```
Completed 8.0 MiB/17.5 MiB (2.3 MiB/s) with 1 file(s) remaining\r
download: s3://bucket/key to <local path>\r\n
```

- Units vary (Bytes/KiB/MiB/GiB), updates come every few MiB.
- With `--recursive` the totals cover the whole batch and grow while the CLI is still listing.
  One `download:`/`upload:` line marks each finished file.
- Downloads go to a temporary sibling `<name>.<8 random chars>` that is renamed at the end. After
  a cancel these leftovers must be deleted. The local mtime is set to `LastModified`.
- `aws s3 mv/cp --recursive` skips folder marker objects, so a folder move has to recreate and
  delete markers itself (section 5.4).

## 4. Listings and caching

### 4.1 How TC lists a plugin folder (measured)

- Every panel listing is wrapped in `FsStatusInfo(START/END, FS_STATUS_OP_LIST)`.
- Entering a folder lists it; going up lists the parent again. **Ctrl+R lists the same path
  again**, with nothing in between. Tab and Alt+Tab trigger no listing. After an operation, the
  source panel is not re-read.
- A command-line command (`FsExecuteFile` with verb `quote <text>`) that returns `FS_EXEC_OK` makes
  TC re-list the current path.
- Recursive operations (Space = `CALCSIZE`, the F5 pre-scan, search, delete) walk the tree with one
  `FsFindFirst` per folder, **without** an `OP_LIST` wrapper.
- The first call after loading is `FsExecuteFile("", "MODE I")` (transfer mode).

### 4.2 Directory cache

- In-memory tree keyed by plugin path; each folder stores compact entries (name, attributes, size,
  mtime), a timestamp, and whether its whole subtree is known (`subtreeComplete`).
- Cached folders are served without AWS calls; no expiry by default (`CacheTtlSec=0`).
- **Refresh detection:** a panel listing of path P right after a panel listing of P is Ctrl+R and
  bypasses the cache for P only (`RefreshHeuristic=1`). Listings during operations do not count.
  Commands that make TC re-list the panel call `SuppressNext()` so that re-list is not taken for
  Ctrl+R. When both panels are inside the plugin, alternating Ctrl+R may read as navigation; the
  `refresh` command covers that.
- Every change made through the plugin invalidates the parent listing; folder changes also drop
  the folder's cached subtree.

### 4.3 Tree listings (one call for a whole subtree)

TC's recursive walks would cost one `aws.exe` call per subfolder (Space on a folder with 16
subfolders took ~55 s). Instead, inside a recursive operation the first `FsFindFirst` of a selected
folder runs **one flat listing of the whole subtree** and caches every directory in it; the rest of
TC's walk is served from the cache (the same folder: 1 call, ~2 s).

- Chunks of 20,000 keys (`--max-items` + `--starting-token`), ~11 s each, with progress and
  Cancel between and during chunks. Capped by `PrefetchMaxKeys` (default 200,000).
- Keys come sorted, so on truncation or cancel only the directories containing the last key are
  incomplete; every other directory listed so far is cached as complete.
- The command `prefetch` does the same for the current folder without a size calculation.

### 4.4 Disk cache

- One UTF-8 text file per group in `%LOCALAPPDATA%\S3Tcmd\cache`: `\` (profiles and bucket lists)
  and one per `\profile\bucket`.
- Loaded lazily on the first listing that touches the group; never overrides fresher in-memory
  data; entries older than `DiskCacheMaxAgeDays` are skipped.
- Dirty groups are saved after operations, debounced to 15 s after panel listings, and at
  `DLL_PROCESS_DETACH`; writes are atomic (temp file + rename). `refresh-all` deletes the files.
- A tree of ~1,100 folders / 5,000 entries is ~350 KB and loads in ~16 ms.

### 4.5 Progress where TC shows none

TC shows a progress dialog for `ProgressProc` calls only in transfer operations (`GET_*`,
`PUT_*`, `RENMOV_*`, `SYNC_*`), not for `CALCSIZE`, command-line `EXEC`, search or delete. It calls
the plugin on its UI thread and shows "Not Responding" meanwhile.

`progress_window.*` therefore shows its own window after 1 s (text, bar, Cancel), running on its
own thread with its own message loop and without an owner window, so it stays responsive while
TC's thread waits. Space has one window for the whole operation: the AWS listing and TC's walk
over the cached folders (81,000 cached folders take ~1 s, if the log file is kept open).

## 5. Transfers

### 5.1 Downloads (TC's calls, measured)

- F5 always uses `GET_MULTI`, even for one file. `GET_SINGLE` is only used for F3/F4/Enter (temp
  downloads to `%TEMP%\_tc\`).
- TC pre-scans all selected folders, then calls `FsGetFile` once per file, depth-first, and
  creates the local folders itself.
- Overwrite: the first call has flags 0; on `FS_FILE_EXISTS` TC asks, then calls again with
  OVERWRITE. With "Overwrite all" TC still calls with 0 first, so the existence check must be cheap.
- **Returning `FS_FILE_OK` without writing the file works**, and files written at
  `FS_STATUS_END(GET_MULTI)` appear correctly. TC's dialog stays open during END: `ProgressProc`
  updates are shown and Cancel works.
- Alt+F5 (pack) is a `GET_MULTI` into `%TEMP%\_tc0\`, and END arrives before TC packs.

### 5.2 Batched downloads

Inside `GET_MULTI`, `FsGetFile` checks for an existing target, queues the file and returns OK. At
END, `PlanDownloads()` turns the queue into few commands:

1. **Subtree:** one `cp --recursive` when the queue contains exactly the files of a tree listed
   in this operation (or fully cached) and the local names equal the keys. Keys that need
   look-alike characters are excluded (`--exclude`, up to 50) and copied separately.
2. **Filtered:** 2+ files of one directory: `--recursive --exclude "*" --include <name>...`
   (fnmatch-escaped, chunked below the command-line limit). Only for directories whose recursive
   subtree has at most 5,000 keys, because the CLI lists the whole source subtree first.
3. **Single:** everything else.

Progress (`Completed X/Y`) is mapped to the batch bytes; `ProgressProc` is polled every 500 ms.
Results are verified on disk, leftovers deleted, and failures reported in one message.
`BatchDownloads=0` switches to one `cp` per file. TC's own total bar is meaningless for a deferred
batch (it counts `FsGetFile` returns), so the real numbers are shown in the source line.

### 5.3 Uploads, mkdir, delete

- **Upload (`PUT_MULTI`):** queued the same way; `PlanUploads()` emits one `cp --recursive` per
  fully selected local folder, filtered commands for small folders, single files otherwise. TC
  passes the plugin target folder as `RemoteDir` of `FS_STATUS_START`.
- **F7:** a zero-byte marker `dir/`. During `PUT_MULTI`, markers are created only for folders that
  received no files.
- **F8:** TC walks the tree and calls `FsDeleteFile` per file and `FsRemoveDir` bottom-up; the
  plugin queues keys (a folder adds its marker) and runs `delete-objects` at END, 1000 keys per
  call. Per-key errors (quiet mode `Errors[]`, e.g. AccessDenied with HTTP 200) are reported once.

### 5.4 Rename, move, copy inside S3

- F6/F2 on a folder is **one** `FsRenMovFile` call (`ri.SizeHigh=0xFFFFFFFF`). The plugin runs a
  tree listing (for progress), `mv/cp --recursive`, recreates markers that empty folders need, and
  deletes the old markers.
- Server-side only within one profile (across buckets is fine). Between profiles there are no
  shared credentials, so the plugin returns `NOTSUPPORTED`.
- F6 between S3 and a local disk: `aws s3 mv` per file.

### 5.5 Background transfers and threads

`FsGetBackgroundFlags()` returns `BG_DOWNLOAD | BG_UPLOAD`, so TC offers *Background* and runs the
whole operation on its own thread. All per-operation state is `thread_local`; the cache, disk
cache, configuration and log are mutex-protected. TC keeps `FsStatusInfo` START..END of one
operation on one thread.

### 5.6 Write safety

`ReadOnly=1` (global or per profile) refuses every write. Buckets themselves are never created or
deleted. One error message per operation.

## 6. Errors

`errors.*` classifies CLI stderr (AccessDenied, NoSuchBucket, NoSuchKey, expired SSO session,
expired temporary credentials, no credentials, unknown profile, network, cancelled) into short
messages. An expired SSO session gets the hint
`aws sso login --profile <name>`; the plugin does not start the login itself.

## 7. `s3://` paths

### 7.1 Copying (`s3tcmd-clip.exe`)

The WFX API cannot change what `cm_CopyFullNamesToClip` copies. A separate GUI-subsystem helper
(rather than a `rundll32` export, which security tools often block) is run as a TC user command:

```ini
[em_s3_copy_uri]
cmd=<plugin folder>\s3tcmd-clip.exe
param=uri %Y%y %UL --cwd %P
```

Measured TC parameters for user commands in a plugin panel:

- `%UL`: UTF-8 list file with BOM, CRLF, **display names**, folders with a trailing backslash.
- `%P%S`: `\\\S3\profile\bucket\…\name`, quoted only when needed; folders without a backslash.
- With the cursor on `..` and nothing selected, TC drops **all** parameters, even literal text;
  `%Y%y` keeps the others, so `--cwd %P` (last, because of TC's quoting of a trailing backslash)
  gets through.
- The working directory is `c:\`; `COMMANDER_INI` points to `wincmd.ini`.

The helper converts plugin paths to `s3://bucket/key` (decoding display names, `/` after folders)
and copies all other paths unchanged. The plugin's root name is read from `[FileSystemPlugins]` in
`wincmd.ini`. `presign` runs up to 8 `aws s3 presign` in parallel. `--out <file>` replaces the
clipboard and message boxes (tests).

### 7.2 Going to a URI

- Address bar: the text after `\\\S3\` reaches `FsFindFirst` unchanged, forward slashes
  included (`\s3://bucket/dir/`, `\profile\s3://bucket/dir/`).
- Command line: a bare `s3://…` is **not** passed to the plugin; `cd s3://…` and `go s3://…`
  arrive as `quote` commands.
- The WFX API cannot redirect a listing. The plugin resolves the profile (explicit; else the
  current panel's profile if it sees the bucket; else an owner from the cached bucket lists; else
  a pinned one; missing bucket lists are fetched in parallel), returns an empty listing, and after
  `FS_STATUS_END(LIST)` sends TC's documented `CD` command (`WM_COPYDATA`, `dwData='C'+256*'D'`,
  data `BOM + path \r \0 S \0`) to its main window. A file URI is sent without a trailing
  backslash, so TC puts the cursor on the file.
- A failed listing would make TC fall back to the previous folder, possibly a local drive, so on
  failure the plugin sends `CD` back into the plugin.

## 8. Install package

### 8.1 TC's plugin auto-installer (measured)

The TC help only says "open the plugin ZIP with Enter". From the executable and tests:

- `pluginst.inf` in the archive root, section `[plugininstall]`, keys `type`, `file`,
  `defaultdir`, `description`, `version` (and `defaultextension` for packers). There is no
  `file64=`; `file=s3tcmd.wfx64` works for a 64-bit-only plugin.
- `version=` is only displayed; versions are not compared.
- Dialogs: *"This archive contains the following Total Commander plugin/addon … install it?"*,
  then *"Install to:"* (default `pluginbasedir\wfx\<defaultdir>`). For an existing installation:
  *"A plugin with this name is already installed in the following directory: … overwrite?"*.
- TC extracts `*.* | pluginst.inf`: every file of the archive except `pluginst.inf`.
- Upgrading while the plugin is loaded works; the new DLL is active after restarting TC.
- Uninstalling or reinstalling does not touch `[Shortcuts]`.

### 8.2 `usercmd.ini` setup

TC reads `usercmd.ini` (next to `%COMMANDER_INI%`) on demand: a section added while TC runs works
at once. Hotkeys live in `wincmd.ini [Shortcuts]`, and there is no command to make TC re-read
them, so the plugin never writes hotkeys.

On the first panel listing of the plugin root in a version (`[Setup] UserCmdOffered` in
`s3tcmd.ini`), `setup.*` checks `[em_s3_copy_uri]` and `[em_s3_presign]`:

| State | Condition | Action on "Yes" |
|---|---|---|
| Ok | section with a `cmd=` that exists (or cannot be judged) | none |
| Missing | no section, or no `cmd=` | write `cmd`, `param`, `menu` (keep an existing `param`/`menu`) |
| StaleCmd | absolute `cmd=` to a missing file | write `cmd` only |

Only if something is not Ok does it ask (RequestProc `RT_MsgYesNo`), once per version. Before
writing it copies `usercmd.ini` to `usercmd.ini.s3tcmd-bak`; `WritePrivateProfileStringW` touches
only these sections. `cmd=` is this DLL's folder with a leading `%COMMANDER_PATH%`, `%APPDATA%` or
`%LOCALAPPDATA%` substituted. The message afterwards explains the commands and reports which keys
run `em_s3_copy_uri` (read from `[Shortcuts]`). The `setup` command runs it on demand.

### 8.3 Version and package build

`src/version.h` is the only place of the version. `res/s3tcmd.rc` includes it, the code logs it,
and `build.bat` reads `S3TCMD_VERSION_STR` with `findstr` to generate `build\pluginst.inf` and the
zip name. Release builds pack `build\s3tcmd-<version>.zip` with Windows' `tar.exe`.

## 9. Code layout

```
src/
  plugin.cpp        exported Fs* functions; listing, cache, batch and command orchestration
  s3tcmd.def        export list
  version.h         version (code, resources, build script)
  util.*            UTF-8/ANSI conversion, command-line quoting, line splitting
  process.*         child processes: pipes, environment, job object, cancel, timeout
  aws_cli.*         RunAws (environment, logging), ListProfiles
  aws_s3.*          ListBuckets / ListFolder / ListTree / delete / markers + JSON parsers
  errors.*          CLI error classification and messages
  names.*           reversible S3 key segment <-> Windows-safe display name
  vfs_path.*        plugin path <-> (profile, bucket, key)
  dir_cache.*       directory cache, dirty groups, RefreshTracker
  dir_tree.*        flat listing -> per-directory listings with completeness
  disk_cache.*      persistent cache files
  transfer.*        download/upload planners and runners, cleanup
  progress.*        parser for `aws s3 cp` progress records
  progress_window.* own progress window on its own thread
  s3_uri.*          s3:// URI / alias path parsing, profile choice, TC "CD" payload
  setup.*           usercmd.ini setup and hotkey description
  clip_paths.*      s3tcmd-clip.exe logic (list files, path -> URI, root name, --cwd)
  clip_main.cpp     s3tcmd-clip.exe
  config.*          s3tcmd.ini
  log.*             optional debug log
res/                icon (TC shows the DLL's first icon) and version resource
sdk/                WFX SDK header (unmodified) + C++ wrapper
third_party/        nlohmann/json
tests/              unit tests (no network), integration tests (real AWS, configured locally)
tools/wfxhost.cpp   console host that loads the plugin like TC does
```

## 10. Testing

- **Unit tests** (`build\tests.exe`, run by every build): quoting, names, paths, JSON parsing of
  recorded CLI output, progress parsing, planners, tree building, cache and refresh logic, disk
  cache, list files, URIs, setup merge.
- **Integration tests** against real AWS, configured with environment variables (see
  `tests\it.local.example.bat`):
  - `build.bat it` is read-only: profiles, buckets, folder listing, paging, tree vs. folder
    listing, subtree and filtered downloads into `%TEMP%`, error classification.
  - `build.bat itw` writes only below a unique prefix and deletes it: batched uploads, a non-ASCII
    key, a marker, a recursive move, delete-objects, optionally AccessDenied with a read-only profile.
- **`wfxhost`** loads the plugin like TC and runs listings, Space, copies, uploads, deletes,
  renames and commands, also in parallel threads. It prints plugin messages and answers questions
  "no" (or "yes" with `WFXHOST_ANSWER=yes`).
- Manual checks in TC for everything that depends on TC's UI (dialogs, Background, hotkeys,
  address bar, installer).

## 11. Known limitations and ideas

- TC shows "Not Responding" during a long Space; the plugin's window shows progress and Cancel.
- No copy/move between profiles inside the plugin; no S3 versioning; no resume.
- Moving many individually selected files inside S3 costs one `aws s3 mv` per file (folders are
  one call). Batch it if it turns out slow.
- Listings restored from disk may be stale until Ctrl+R / `refresh-all`.
- After "go to s3://", TC's history also holds the `\\\S3\s3://…` alias; going back redirects again.
- Ideas: Properties (Alt+Enter → `head-object`), custom columns (storage class, ETag) through the
  content-field functions, a settings dialog.
- If a later version changes `param=` of the user commands, the setup has to detect and offer that
  change; today it keeps an existing `param=`.
