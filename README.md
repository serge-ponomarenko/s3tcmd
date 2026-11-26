# S3Tcmd: AWS S3 file system plugin for Total Commander

Work with AWS S3 buckets in Total Commander (64-bit) like with any other folder. All AWS access goes
through the official [AWS CLI v2](https://aws.amazon.com/cli/), so every authentication method the
CLI supports works: profiles, SSO, assumed roles, and environment credentials. The plugin stores no
credentials.

Features: browse, download, upload, create folders, delete, and rename/move/copy inside S3. Also:
background transfers, a persistent listing cache, fast whole-tree listings (Space), copying `s3://`
paths and presigned URLs with a hotkey, and going to a pasted `s3://` URI. See the
[changelog](CHANGELOG.md).

## Requirements

- Total Commander 64-bit (tested with 11.58)
- AWS CLI v2 on `PATH` (or configured via `AwsExe`), with at least one working profile

## Install

1. Download `s3tcmd-<version>.zip` from the
   [Releases](https://github.com/serge-ponomarenko/s3tcmd/releases) page (or build it, see below).
   In Total Commander, go to the zip and press **Enter**. TC shows *"This archive contains the following Total Commander
   plugin/addon … Do you want to install it?"*: answer **Yes**, and accept or change the folder
   under *Install to:*. The default comes from `pluginbasedir` in `wincmd.ini`. TC copies
   `s3tcmd.wfx64`, `s3tcmd-clip.exe` and `README.md` there and registers the plugin as **S3**.
2. Open *Network Neighborhood* (`\\\`) → **S3**. The first time (and after an upgrade), the plugin
   offers to add the user commands `em_s3_copy_uri` and `em_s3_presign` to `usercmd.ini` (see
   [Copy s3:// paths](#copy-s3-paths-and-presigned-urls-with-a-hotkey)). If you answer **Yes**:
   - only these two sections are written, and a backup `usercmd.ini.s3tcmd-bak` is made first;
   - `cmd=` points to the folder the plugin was installed to;
   - TC uses the commands at once, without a restart.

   The plugin asks once per version. Type `setup` on the command line inside the plugin to run
   it again later.
3. Optional: bind a hotkey to `em_s3_copy_uri`. The plugin never changes your hotkeys.

**Upgrade:** open the zip of the new version the same way. TC finds the existing installation
(*"A plugin with this name is already installed … overwrite?"* → **Yes**). This works while the
plugin is loaded, and the new version is active after restarting TC.

Manual install: *Configuration → Options → Plugins → File system plugins → Configure → Add*, select
`s3tcmd.wfx64`, and keep `s3tcmd-clip.exe` in the same folder. For development, `install.bat`
copies the build to `%APPDATA%\GHISLER\plugins\wfx\s3tcmd\` (close TC first, because it keeps
the DLL loaded).

## Usage

```
\\\S3\<profile>\<bucket>\<folder>\...\<file>
```

- **Navigation:** listings are cached; re-entering a folder does not call AWS. **Ctrl+R** re-reads
  the current folder.
- **F5 / F3 / F4 / Enter:** download, view, edit, open. Multi-file and folder copies are batched
  into as few `aws s3 cp` calls as possible (a whole folder is one `--recursive` call). Progress and
  Cancel work as usual.
- **F5 into S3 (upload):** batched the same way. A whole local folder is one `aws s3 cp --recursive`,
  and only empty folders get a folder marker object.
- **F7:** creates a folder (a zero-byte `name/` marker object).
- **F8:** deletes files and folders. All keys of one operation go into `delete-objects` calls, up to
  1000 keys each.
- **F6 / Shift+F6 / copy between two plugin panels:** server-side `aws s3 mv` / `cp`, within one
  profile (across buckets too). A folder is moved with one `--recursive` call, and its folder
  markers are carried over.
- **F6 to/from a local disk:** `aws s3 mv` per file (upload/download, then delete the source).
- **Background:** the F5/F6 progress dialog has TC's *Background* button. A background transfer
  runs in its own thread, and you can keep browsing (S3 included) meanwhile.
- **F2** (inline rename) works like F6 within the same folder.
- **Safety:** `ReadOnly=1` (globally, or per profile in `[Profile:name]`) refuses every write
  operation. Write errors such as AccessDenied are reported once per operation.
- **Space** (folder size), the F5 pre-scan, and search fetch a whole selected folder tree with one
  listing call. See the tip below.

### Tip: press Space to make a whole folder tree instant

Each `aws.exe` call costs 1.5–2 s, so plain navigation pays that price once for every folder you
open. Pressing **Space** on a folder (TC's *calculate folder size*) makes the plugin fetch the
**entire subtree with a single listing call**, then cache every subfolder in it. After that:

- **navigation anywhere inside that tree is instant**, with no AWS calls;
- **F5** on that folder (or anything inside it) needs no listing at all, and a whole-folder copy
  runs as one `aws s3 cp --recursive`.

This works best for folders with **many subfolders but a moderate number of files**, which is
typical of build outputs and dated folder layouts. Listing speed is ~1,500–1,800 keys/s. Large trees
are listed in chunks of 20,000 keys. TC shows no progress for Space and waits ("Not Responding")
until the plugin returns, so the plugin opens **its own small progress window** after 1 s, with the
key count and **Cancel**. Inside F5/F6 the count appears in TC's own dialog instead. Cancel stops
the running chunk, and every folder listed completely up to then stays cached. Trees are capped by
`PrefetchMaxKeys` (default 200,000), with the same partial caching.

The command `prefetch` (on the TC command line) does the same for the current folder without
calculating sizes.

**Listings survive restarts.** All cached listings, including these trees, are stored on disk
(`%LOCALAPPDATA%\S3Tcmd\cache`, one file per bucket) and reloaded on first use. A tree fetched
yesterday is still instant today. They may be out of date: Ctrl+R in a folder re-reads just that
folder, and `refresh-all` drops the whole cache (memory and disk).

### Go to an `s3://` URI

Paste a URI into the panel's **address bar** (click the path above the file list), after the
plugin root:

```
\\\S3\s3://my-bucket/some/folder/          the profile is chosen automatically
\\\S3\my-profile\s3://my-bucket/some/folder/   with an explicit profile
```

Inside the plugin, the **command line** also accepts `cd s3://my-bucket/some/folder/` (or
`go s3://...`). A bare `s3://...` does not work there: TC does not pass it to the plugin.

- The panel briefly shows an empty folder and then changes to the real path
  (`\\\S3\<profile>\<bucket>\...`), so Backspace, history and Ctrl+D work as usual.
- A URI of a file (`.../report.csv`) opens its folder with the cursor on the file.
- **Choosing the profile:** the plugin looks for the bucket in the bucket lists of all profiles
  (cached; profiles not listed yet are asked in parallel once). An account that owns the bucket
  wins over one that only has it in `PinnedBuckets`; on the command line the current panel's
  profile wins if it can see the bucket.
- Unknown bucket: a message, and the panel stays in the plugin.

### Copy `s3://` paths (and presigned URLs) with a hotkey

TC's *Copy names with full path* (`cm_CopyFullNamesToClip`, e.g. Ctrl+Shift+C) copies plugin
paths like `\\\S3\my-profile\my-bucket\dir\`, and a plugin cannot change that. The helper
`s3tcmd-clip.exe` (installed next to the plugin) does it instead, from a TC user command:

- names inside S3 become `s3://bucket/key`, folders end with `/`, one line per selected item;
- **all other paths are copied unchanged**, so the same hotkey keeps working on local drives;
- with the cursor on `..` (nothing selected) it copies the current folder.

Setup: the plugin offers to add these user commands to `usercmd.ini` (next to `wincmd.ini`,
usually `%APPDATA%\GHISLER\`) when you first open it (see *Install*), or run `setup` on the
command line inside the plugin. Done by hand, the sections are:

```ini
[em_s3_copy_uri]
cmd=%APPDATA%\GHISLER\plugins\wfx\s3tcmd\s3tcmd-clip.exe
param=uri %Y%y %UL --cwd %P
menu=Copy names with full path (s3:// inside S3)

[em_s3_presign]
cmd=%APPDATA%\GHISLER\plugins\wfx\s3tcmd\s3tcmd-clip.exe
param=presign %UL
menu=Copy presigned URL (S3 files)
```

Then bind a hotkey to the user command `em_s3_copy_uri` yourself, e.g. Ctrl+Shift+C instead of
`cm_CopyFullNamesToClip`: *Configuration → Options → Misc → Redefine hotkeys*: choose the key, then
the command `em_s3_copy_uri` (category *usercmd.ini*). Or, with TC closed, in `wincmd.ini`:

```ini
[Shortcuts]
CS+C=em_s3_copy_uri
```

Notes:
- `%UL` passes the selection as a UTF-8 list file. `--cwd %P` must stay last.
- `em_s3_presign` runs `aws s3 presign` with the profile from the path, for the selected files
  (folders are skipped), and copies one HTTPS URL per line. The URLs are valid for
  `PresignExpirySec` (default 3600 s, at most 7 days). The bucket region is looked up once and
  cached in `%LOCALAPPDATA%\S3Tcmd\bucket-regions.ini`. It takes about 1.5–3 s. This command was
  tested from the command line, not in TC.
- `setup` fixes `cmd=` when it points to a file that no longer exists (plugin moved). It keeps your
  own `param=` and `menu=`.

### Commands and names

- Commands typed on the TC command line while inside the plugin:

  | Command | Effect |
  |---|---|
  | `refresh` | re-read the current folder (same as Ctrl+R) |
  | `refresh-all` | drop all cached listings, in memory and on disk |
  | `prefetch` | fetch the whole tree below the current folder with one listing (like Space) |
  | `setup` | add/fix `em_s3_copy_uri` / `em_s3_presign` in `usercmd.ini` (asks first), or show them |
  | `cd s3://bucket/key`, `go s3://bucket/key` | go to that folder or file |
  | `help` | list commands |

- S3 key characters that Windows does not allow in file names (`\ / : * ? " < > |`, control
  characters, trailing dot/space) are shown as look-alike Unicode characters (e.g. `：`). The mapping
  is reversible.

## Configuration

`s3tcmd.ini` lives next to Total Commander's `wincmd.ini` (usually `%APPDATA%\GHISLER\`). Missing
`[General]` keys are written with defaults on first use.

```ini
[General]
AwsExe=aws.exe          ; name on PATH or full path
LogFile=                ; debug log of every plugin call and aws.exe invocation (empty = off;
                        ; for troubleshooting, it grows quickly)
CacheTtlSec=0           ; 0 = cached listings never expire (refresh manually)
RefreshHeuristic=1      ; a repeated listing of the same folder is treated as Ctrl+R
MaxListItems=50000      ; cap for one folder listing (a marker entry shows truncation)
PrefetchMaxKeys=200000  ; cap for one recursive tree listing (Space, F5, search); 0 = off
BatchDownloads=1        ; batch multi-file downloads
ReadOnly=0              ; 1 = refuse all write operations (upload, mkdir, delete, rename)
DisableImds=1           ; set AWS_EC2_METADATA_DISABLED=true for aws.exe: avoids 2-12 s timeouts
                        ; when a profile has no region; set 0 on EC2 with an instance role
DiskCache=1             ; keep listings on disk across TC restarts
DiskCacheDir=%LOCALAPPDATA%\S3Tcmd\cache
DiskCacheMaxAgeDays=30  ; listings older than this are not loaded (0 = no limit)
PresignExpirySec=3600   ; lifetime of URLs copied by em_s3_presign (s3tcmd-clip.exe)

[Profile:my-profile]
PinnedBuckets=bucket-a, bucket-b ; always shown, e.g. cross-account buckets missing from list-buckets
Hide=0                           ; 1 = hide this profile in the plugin root
ReadOnly=0                       ; 1 = refuse write operations for this profile
```

Tip: a bucket can also be opened directly by typing its path, e.g. `cd \\\S3\my-profile\some-bucket`.

## Limitations

- Copy/move between different AWS profiles inside the plugin is not supported (no shared
  credentials for a server-side copy). Use a local folder in between.
- Folder rename/move costs a few calls (tree listing, `mv --recursive`, marker fix-up). Moving
  many individually selected files costs one `aws s3 mv` per file.
- Every AWS call starts `aws.exe` (~1.5–2 s). Caching and batching hide most of this.
- In a batched F5, TC's own total bar and speed are not meaningful. The real progress
  (`[X of Y, speed, N files]`) is shown in the source line, and the per-file bar shows the batch
  percentage.
- `--max-items` caps files only, so a folder with a huge number of direct subfolders is not capped.
- Expired SSO sessions are reported with a hint (`aws sso login --profile …`); the login itself is
  not started by the plugin.
- While a long Space runs, TC itself shows "Not Responding" (it waits for the plugin). The plugin's
  own window shows progress and Cancel.
- Listings restored from the disk cache can be out of date: use Ctrl+R / `refresh-all`.
- No S3 versioning (old versions, restore) and no resume of interrupted transfers.

## Build and test

Requires Visual Studio 2019 or newer (Build Tools are enough) with *Desktop development with C++*.
`build.bat` finds it with `vswhere`, or uses the environment of a Developer Command Prompt.

```
build.bat          release build + unit tests  -> build\s3tcmd.wfx64, build\s3tcmd-clip.exe,
                   build\pluginst.inf and the install package build\s3tcmd-<version>.zip
build.bat debug    debug CRT, no optimization, no package
build.bat it       also run the read-only integration tests against real AWS
build.bat itw      also run the write integration tests (unique prefix, deleted afterwards)
```

The integration tests need your own AWS profile and bucket. Copy `tests\it.local.example.bat` to
`tests\it.local.bat` (ignored by git) and fill in the values; `build.bat` calls it. Without the
settings the integration tests are skipped.

`build\wfxhost.exe <plugin> <ini-dir> [--parallel] <path|list:|exec:|calc:|copy:|get1:|put:|mkdir:|del:|ren:...>...`
loads the plugin like Total Commander does. It is useful for debugging without TC (`list:` is a
panel listing, `exec:<dir>|<command>` a command-line command). Plugin messages and questions are
printed; questions are answered "no", or "yes" with `WFXHOST_ANSWER=yes`.

`build\s3tcmd-clip.exe uri|presign --out <file> --paths <path>...` writes the result to a file
instead of the clipboard and message boxes (for tests; list files work too).

**Release:** bump the version in `src/version.h` (the only place), add it to
[CHANGELOG.md](CHANGELOG.md), run `build.bat`, and attach `build\s3tcmd-<version>.zip` to a GitHub
release.

See [docs/DESIGN.md](docs/DESIGN.md) for the design and the measured behaviour of Total Commander
and the AWS CLI.

## License

[MIT](LICENSE). Third-party code: [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (MIT),
vendored in `third_party/`, and the Total Commander WFX SDK header `sdk/fsplugin.h`
(© Christian Ghisler); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

S3Tcmd is not affiliated with Ghisler Software GmbH or Amazon Web Services.
