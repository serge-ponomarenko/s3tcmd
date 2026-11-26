#include "errors.h"

namespace s3tc {

namespace {

bool Contains(const std::wstring& s, const wchar_t* needle) {
    return s.find(needle) != std::wstring::npos;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

AwsError ClassifyAwsError(const std::wstring& msg) {
    AwsError e;
    e.detail = Trim(msg);
    const std::wstring noise = L"aws: [ERROR]: ";
    if (e.detail.rfind(noise, 0) == 0) e.detail = Trim(e.detail.substr(noise.size()));
    if (e.detail.empty()) return e;
    const std::wstring& m = e.detail;

    if (m == L"Cancelled") {
        e.kind = AwsErrorKind::Cancelled;
    } else if (Contains(m, L"Token has expired") || Contains(m, L"SSO session") ||
               Contains(m, L"sso login") || Contains(m, L"Error loading SSO Token") ||
               Contains(m, L"UnauthorizedSSOTokenError")) {
        e.kind = AwsErrorKind::SsoExpired;
    } else if (Contains(m, L"ExpiredToken") || Contains(m, L"security token included in the request is expired")) {
        e.kind = AwsErrorKind::CredentialsExpired;
    } else if (Contains(m, L"Unable to locate credentials")) {
        e.kind = AwsErrorKind::NoCredentials;
    } else if (Contains(m, L"could not be found") && Contains(m, L"profile")) {
        e.kind = AwsErrorKind::ProfileNotFound;
    } else if (Contains(m, L"NoSuchBucket")) {
        e.kind = AwsErrorKind::NoSuchBucket;
    } else if (Contains(m, L"NoSuchKey") || Contains(m, L"(404)") || Contains(m, L"Not Found")) {
        e.kind = AwsErrorKind::NoSuchKey;
    } else if (Contains(m, L"AccessDenied") || Contains(m, L"Access Denied") ||
               Contains(m, L"(403)") || Contains(m, L"InvalidAccessKeyId") ||
               Contains(m, L"SignatureDoesNotMatch") || Contains(m, L"not authorized")) {
        e.kind = AwsErrorKind::AccessDenied;
    } else if (Contains(m, L"Could not connect") || Contains(m, L"EndpointConnectionError") ||
               Contains(m, L"Connect timeout") || Contains(m, L"Read timeout")) {
        e.kind = AwsErrorKind::Network;
    } else {
        e.kind = AwsErrorKind::Other;
    }
    return e;
}

std::wstring DescribeAwsError(const AwsError& e, const std::wstring& profile) {
    std::wstring head;
    switch (e.kind) {
        case AwsErrorKind::None:
            return {};
        case AwsErrorKind::AccessDenied:
            head = L"Access denied for profile \"" + profile + L"\".";
            break;
        case AwsErrorKind::NoSuchBucket:
            head = L"The bucket does not exist.";
            break;
        case AwsErrorKind::NoSuchKey:
            head = L"The object does not exist.";
            break;
        case AwsErrorKind::SsoExpired:
            head = L"The SSO session for profile \"" + profile +
                   L"\" has expired.\nRun in a console:  aws sso login --profile " + profile;
            break;
        case AwsErrorKind::CredentialsExpired:
            head = L"The credentials of profile \"" + profile + L"\" have expired. Renew them and retry.";
            break;
        case AwsErrorKind::NoCredentials:
            head = L"No credentials are configured for profile \"" + profile + L"\".";
            break;
        case AwsErrorKind::ProfileNotFound:
            head = L"AWS profile \"" + profile + L"\" was not found.";
            break;
        case AwsErrorKind::Network:
            head = L"Cannot connect to AWS.";
            break;
        case AwsErrorKind::Cancelled:
            return L"Cancelled.";
        case AwsErrorKind::Other:
            head = L"AWS CLI error.";
            break;
    }
    return head + L"\n\n" + e.detail;
}

}  // namespace s3tc
