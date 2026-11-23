// Win32 verifier for bridge capability files and DeepSeek profile metadata.
// Commands carry paths only. The helper never reads or prints capability bytes.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <cstddef>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Rejection { const char* stage; const char* cause; };
struct HandleCloser { void operator()(void* h) const { CloseHandle(h); } };
struct LocalDeleter { void operator()(void* p) const { LocalFree(p); } };
using Handle = std::unique_ptr<void, HandleCloser>;
using Local = std::unique_ptr<void, LocalDeleter>;

static constexpr DWORD file_sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
static constexpr DWORD open_flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;

static std::string utf8(std::wstring_view input) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw Rejection{"input", "path"};
    std::string bytes(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), bytes.data(), size, nullptr, nullptr))
        throw Rejection{"input", "path"};
    return bytes;
}

static std::string quoted(std::wstring_view text) {
    std::string output = "\"";
    for (const char c : utf8(text)) {
        if (c == '\\' || c == '"') output += '\\';
        output += c;
    }
    output += '"';
    return output;
}

static std::vector<std::wstring> path_chain(const std::wstring& path) {
    if (path.size() < 3 || path.size() > 2048 || path[1] != L':' || path[2] != L'\\'
        || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        throw Rejection{"input", "path"};
    const auto root = path.substr(0, 3);
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
        throw Rejection{"input", "local-fixed-volume-required"};
    std::vector<std::wstring> chain{root};
    for (size_t start = 3; start < path.size();) {
        const size_t separator = path.find(L'\\', start);
        const size_t end = separator == std::wstring::npos ? path.size() : separator;
        const auto component = path.substr(start, end - start);
        if (component.empty() || component.back() == L'.' || component.back() == L' '
            || (separator != std::wstring::npos && end + 1 == path.size()))
            throw Rejection{"input", "path"};
        for (wchar_t c : component) {
            if (c < L' ' || c == 127 || std::wstring_view(L"<>:\"/|?*").find(c) != std::wstring_view::npos)
                throw Rejection{"input", "path"};
        }
        auto stem = component.substr(0, component.find(L'.'));
        for (auto& c : stem) c = static_cast<wchar_t>(std::towupper(c));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL"
            || stem == L"CONIN$" || stem == L"CONOUT$"
            || (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT")
                && std::wstring_view(L"123456789\u00b9\u00b2\u00b3").find(stem.back()) != std::wstring_view::npos))
            throw Rejection{"input", "path"};
        if (chain.size() >= 64) throw Rejection{"input", "path"};
        chain.push_back(path.substr(0, end));
        start = end + 1;
    }
    return chain;
}

static std::wstring sid_string(PSID sid) {
    LPWSTR value = nullptr;
    if (!sid || !IsValidSid(sid) || !ConvertSidToStringSidW(sid, &value))
        throw Rejection{"owner", "user"};
    Local buffer(value);
    return value;
}

static std::wstring current_user() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw Rejection{"input", "user"};
    Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size || size > 4096) throw Rejection{"input", "user"};
    std::vector<DWORD_PTR> buffer((size + sizeof(DWORD_PTR) - 1) / sizeof(DWORD_PTR));
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
        throw Rejection{"input", "user"};
    const auto user = sid_string(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
    if (user == L"S-1-5-18" || user == L"S-1-5-32-544") throw Rejection{"input", "user"};
    return user;
}

static Handle open_entry(const std::wstring& path, DWORD access, DWORD disposition = OPEN_EXISTING,
                         SECURITY_ATTRIBUTES* security = nullptr) {
    const HANDLE h = CreateFileW(path.c_str(), access, file_sharing, security, disposition, open_flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    return Handle(h);
}

static BY_HANDLE_FILE_INFORMATION inspect(const Handle& file, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info))
        throw Rejection{"attributes", "other"};
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw Rejection{"reparse", "reparse"};
    if (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        throw Rejection{"attributes", "kind"};
    return info;
}

struct Ace {
    std::wstring sid;
    BYTE type;
    BYTE flags;
    DWORD mask;
    bool operator==(const Ace&) const = default;
};
struct Snapshot {
    std::wstring owner;
    bool protected_dacl;
    std::vector<Ace> aces;
    bool operator==(const Snapshot&) const = default;
};

static Snapshot snapshot(const Handle& handle) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    if (GetSecurityInfo(handle.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &raw) != ERROR_SUCCESS)
        throw Rejection{"get-acl", "other"};
    Local descriptor(raw);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(raw, &control, &revision) || !(control & SE_DACL_PRESENT)
        || !dacl || !IsValidAcl(dacl) || dacl->AceCount > 128)
        throw Rejection{"raw-dacl", "acl"};
    Snapshot result{sid_string(owner), (control & SE_DACL_PROTECTED) != 0, {}};
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* entry = nullptr;
        if (!GetAce(dacl, index, &entry)) throw Rejection{"raw-dacl", "acl"};
        auto ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        constexpr auto offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
        if ((ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceType != ACCESS_DENIED_ACE_TYPE)
            || (ace->Header.AceFlags & ~31) || ace->Header.AceSize < offset + 8)
            throw Rejection{"access-rules", "acl"};
        auto sid = reinterpret_cast<SID*>(&ace->SidStart);
        if (GetSidLengthRequired(sid->SubAuthorityCount) > ace->Header.AceSize - offset)
            throw Rejection{"access-rules", "acl"};
        result.aces.push_back({sid_string(sid), ace->Header.AceType, ace->Header.AceFlags, ace->Mask});
    }
    return result;
}

static Snapshot private_snapshot(const Handle& file, const std::wstring& user, bool directory) {
    inspect(file, directory);
    auto value = snapshot(file);
    if (value.owner != user || !value.protected_dacl || value.aces.size() != 2)
        throw Rejection{"validate", "acl"};
    bool user_rule = false;
    bool system_rule = false;
    for (const auto& ace : value.aces) {
        if (ace.type != ACCESS_ALLOWED_ACE_TYPE || ace.mask != FILE_ALL_ACCESS
            || ace.flags != (directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0))
            throw Rejection{"validate", "acl"};
        if (ace.sid == user && !user_rule) user_rule = true;
        else if (ace.sid == L"S-1-5-18" && !system_rule) system_rule = true;
        else throw Rejection{"validate", "acl"};
    }
    if (!user_rule || !system_rule) throw Rejection{"validate", "acl"};
    return value;
}

static Local new_descriptor(const std::wstring& user, bool directory) {
    const std::wstring flags = directory ? L"OICI" : L"";
    const std::wstring sddl = L"O:" + user + L"D:P(A;" + flags + L";FA;;;" + user + L")(A;" + flags + L";FA;;;SY)";
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr))
        throw Rejection{"acl-build", "acl"};
    return Local(raw);
}

static void protect(const Handle& file, const std::wstring& user, bool directory, const Local& descriptor) {
    inspect(file, directory);
    if (snapshot(file).owner != user) throw Rejection{"existing-owner", "owner"};
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl, &defaulted) || !present || !dacl
        || SetSecurityInfo(file.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                           nullptr, nullptr, dacl, nullptr) != ERROR_SUCCESS)
        throw Rejection{"set-acl", "acl"};
}

static std::string capability_entry(const std::wstring& path, bool directory, const Snapshot& value) {
    const std::string kind = directory ? "true" : "false";
    const std::string inheritance = directory ? "3" : "0";
    std::string result = "{\"path\":" + quoted(path) + ",\"directory\":" + kind
        + ",\"reparse\":false,\"rawAceCount\":2,\"protected\":true,\"owner\":" + quoted(value.owner) + ",\"rules\":[";
    bool first = true;
    for (const auto& ace : value.aces) {
        if (!first) result += ',';
        first = false;
        result += "{\"sid\":" + quoted(ace.sid) + ",\"type\":0,\"inherited\":false,\"rights\":2032127,\"inheritance\":"
            + inheritance + ",\"propagation\":0}";
    }
    return result + "]}";
}

static std::string capability(const std::wstring& operation, const std::vector<std::wstring>& chain,
                              const std::wstring& user) {
    const bool directory = operation == L"prepare-directory" || operation == L"verify-directory";
    const bool prepare = operation == L"prepare-directory";
    const bool create = operation == L"create-file";
    const bool repair = operation == L"protect-file" || prepare;
    if (chain.size() < (directory ? 2u : 3u)) throw Rejection{"input", "path"};
    const size_t parent = chain.size() - 2;
    std::vector<Handle> ancestors;
    // Keep checked ancestors open without delete sharing through the operation.
    for (size_t i = 0; i <= parent; ++i) {
        const bool check_parent_acl = !directory && i == parent;
        const DWORD access = FILE_READ_ATTRIBUTES | (check_parent_acl ? READ_CONTROL : 0);
        auto handle = open_entry(chain[i], access);
        if (!handle && prepare && i > 0 && GetLastError() == ERROR_FILE_NOT_FOUND) {
            if (!CreateDirectoryW(chain[i].c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                throw Rejection{"create-directory", "other"};
            handle = open_entry(chain[i], access);
        }
        inspect(handle, true);
        ancestors.push_back(std::move(handle));
    }
    std::string parent_json;
    if (!directory) parent_json = capability_entry(chain[parent], true, private_snapshot(ancestors.back(), user, true));
    Local descriptor;
    if (create || repair) descriptor = new_descriptor(user, directory);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    const auto& target = chain.back();
    if (prepare && !CreateDirectoryW(target.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw Rejection{"create-directory", "other"};
    const DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL | (repair ? WRITE_DAC : 0);
    auto file = open_entry(target, access, create ? CREATE_NEW : OPEN_EXISTING, create ? &security : nullptr);
    if (!file && create) throw Rejection{"create-file", "other"};
    if (repair) protect(file, user, directory, descriptor);
    const auto entry = capability_entry(target, directory, private_snapshot(file, user, directory));
    return "{\"version\":1,\"operation\":" + quoted(operation) + ",\"currentSid\":" + quoted(user)
        + ",\"directory\":" + (directory ? entry : parent_json) + ",\"item\":" + (directory ? "null" : entry) + "}";
}

static bool trusted_profile_sid(const std::wstring& sid, const std::wstring& user) {
    return sid == user || sid == L"S-1-5-18" || sid == L"S-1-5-32-544";
}

static std::string binding_hash(const std::wstring& operation, const std::wstring& target, const std::wstring& nonce) {
    if (nonce.size() != 32 || nonce.find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
        throw Rejection{"input", "operation"};
    std::string input = utf8(operation) + '\0' + utf8(target) + '\0' + utf8(nonce);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw Rejection{"input", "other"};
    unsigned char digest[32]{};
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(input.data()),
        static_cast<ULONG>(input.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) throw Rejection{"input", "other"};
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}

static std::string profile(const std::wstring& operation, const std::vector<std::wstring>& chain,
                           const std::wstring& user, const std::wstring& nonce) {
    const bool directory = operation == L"verify-directory";
    if (!directory && operation != L"verify-file") throw Rejection{"input", "operation"};
    std::vector<Handle> entries;
    std::vector<BY_HANDLE_FILE_INFORMATION> before;
    for (size_t i = 0; i < chain.size(); ++i) {
        const bool target = i + 1 == chain.size();
        auto handle = open_entry(chain[i], FILE_READ_ATTRIBUTES | (target ? READ_CONTROL : 0));
        before.push_back(inspect(handle, !target || directory));
        entries.push_back(std::move(handle));
    }
    auto value = snapshot(entries.back());
    if (!trusted_profile_sid(value.owner, user)) throw Rejection{"owner", "owner"};
    const DWORD read_mask = 0xa01200a9;
    for (const auto& ace : value.aces) {
        if (ace.type == ACCESS_ALLOWED_ACE_TYPE && !trusted_profile_sid(ace.sid, user) && (ace.mask & ~read_mask))
            throw Rejection{"validate", "acl"};
    }
    if (value != snapshot(entries.back())) throw Rejection{"validate", "acl"};
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto after = inspect(entries[i], i + 1 < entries.size() || directory);
        if (after.dwFileAttributes != before[i].dwFileAttributes
            || CompareFileTime(&after.ftCreationTime, &before[i].ftCreationTime) != 0
            || CompareFileTime(&after.ftLastWriteTime, &before[i].ftLastWriteTime) != 0)
            throw Rejection{"validate", "other"};
    }
    const auto binding = binding_hash(operation, chain.back(), nonce);
    std::string result = "{\"schema\":\"dsh-profile-metadata-acl-v1\",\"operation\":" + quoted(operation)
        + ",\"binding\":\"" + binding + "\",\"currentSid\":" + quoted(user)
        + ",\"ancestorCount\":" + std::to_string(entries.size()) + ",\"ancestorsNoReparse\":true,\"item\":{\"directory\":"
        + (directory ? "true" : "false") + ",\"reparse\":false,\"ownerSid\":" + quoted(value.owner)
        + ",\"daclPresent\":true,\"protected\":" + (value.protected_dacl ? "true" : "false")
        + ",\"aceCount\":" + std::to_string(value.aces.size()) + ",\"aces\":[";
    bool first = true;
    for (const auto& ace : value.aces) {
        if (!first) result += ',';
        first = false;
        result += "{\"sid\":" + quoted(ace.sid) + ",\"type\":" + std::to_string(ace.type)
            + ",\"mask\":" + std::to_string(ace.mask) + ",\"flags\":" + std::to_string(ace.flags) + "}";
    }
    return result + "]}}";
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4 && argc != 5) throw Rejection{"input", "operation"};
        const std::wstring mode = argv[1], operation = argv[2];
        const auto chain = path_chain(argv[3]);
        const auto user = current_user();
        std::string result;
        if (mode == L"profile" && argc == 5) result = profile(operation, chain, user, argv[4]);
        else if (mode == L"capability" && argc == 4
                 && (operation == L"verify-file" || operation == L"verify-directory"
                     || operation == L"prepare-directory" || operation == L"create-file" || operation == L"protect-file"))
            result = capability(operation, chain, user);
        else throw Rejection{"input", "operation"};
        std::fputs(result.c_str(), stdout);
        std::fputc('\n', stdout);
        return 0;
    } catch (const Rejection& error) {
        std::fprintf(stderr, "DSH_ACL_REJECTED:{\"stage\":\"%s\",\"cause\":\"%s\"}\n", error.stage, error.cause);
    } catch (...) {
        std::fputs("DSH_ACL_REJECTED\n", stderr);
    }
    return 1;
}
