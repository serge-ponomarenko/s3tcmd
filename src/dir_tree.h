// Builds per-directory listings from one flat (recursive) S3 listing of a subtree.
// Used to answer Total Commander's recursive walks (Space/CALCSIZE, F5 pre-scan, search) with a
// single aws.exe call instead of one call per subfolder.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "aws_s3.h"

namespace s3tc {

struct TreeDir {
    std::vector<S3Entry> entries;  // direct children (dirs and files)
    // False if the flat listing was truncated inside this directory's subtree, i.e. some
    // descendants may be missing. A complete directory has complete descendants too.
    bool complete = true;
};

// `objects`: keys relative to the subtree root, in S3 (lexicographic) order, as returned by a
// flat listing. `truncated`: the listing stopped early; the last object marks where.
// Returns directories keyed by their key prefix relative to the subtree root: "" = root, "a/",
// "a/b/", and "/" for an empty-named folder (key "/x"). Prefixes are unambiguous, plain joined
// names are not ("" + "/" + "" would collide with the root).
std::map<std::wstring, TreeDir> BuildDirectoryTree(const std::vector<S3Entry>& objects, bool truncated);

// Splits a directory key ("a//b/") into its segments ({"a", "", "b"}); "" gives {}.
std::vector<std::wstring> SplitDirKey(const std::wstring& rel);

}  // namespace s3tc
