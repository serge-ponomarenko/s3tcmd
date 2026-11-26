// Classifies AWS CLI error output into categories with user-facing messages.
#pragma once

#include <string>

namespace s3tc {

enum class AwsErrorKind {
    None,
    AccessDenied,
    NoSuchBucket,
    NoSuchKey,
    SsoExpired,          // SSO token or session expired / missing
    CredentialsExpired,  // temporary STS credentials expired
    NoCredentials,       // nothing configured for the profile
    ProfileNotFound,
    Network,
    Cancelled,
    Other,
};

struct AwsError {
    AwsErrorKind kind = AwsErrorKind::None;
    std::wstring detail;  // trimmed CLI message
};

AwsError ClassifyAwsError(const std::wstring& cliMessage);

// Message for a dialog; `profile` is used in hints such as "aws sso login --profile X".
std::wstring DescribeAwsError(const AwsError& e, const std::wstring& profile);

}  // namespace s3tc
