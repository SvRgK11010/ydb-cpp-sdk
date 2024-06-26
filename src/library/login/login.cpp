#include <jwt-cpp/jwt.h>
#include <src/library/digest/argonish/argon2.h>
#include <ydb-cpp-sdk/util/string/builder.h>
#include <src/library/string_utils/base64/base64.h>
#include <ydb-cpp-sdk/library/json/json_value.h>
#include <ydb-cpp-sdk/library/json/json_reader.h>
#include <src/library/json/json_writer.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <ydb-cpp-sdk/util/generic/singleton.h>
#include <ydb-cpp-sdk/util/string/builder.h>
#include <ydb-cpp-sdk/util/string/cast.h>
#include <src/util/string/hex.h>

#include <deque>

#include "login.h"

namespace NLogin {

struct TLoginProvider::TImpl {

    THolder<NArgonish::IArgon2Base> ArgonHasher;

    TImpl() {
        ArgonHasher = Default<NArgonish::TArgon2Factory>().Create(
            NArgonish::EArgon2Type::Argon2id, // Mixed version of Argon2
            2, // 2-pass computation
            (1<<11), // 2 mebibytes memory usage (in KiB)
            1 // number of threads and lanes
            );
    }
    void GenerateKeyPair(std::string& publicKey, std::string& privateKey);
    std::string GenerateHash(const std::string& password);
    bool VerifyHash(const std::string& password, const std::string& hash);
};

TLoginProvider::TLoginProvider()
    : Impl(new TImpl())
{}

TLoginProvider::~TLoginProvider()
{}

bool TLoginProvider::CheckAllowedName(const std::string& name) {
    return name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
}

TLoginProvider::TBasicResponse TLoginProvider::CreateUser(const TCreateUserRequest& request) {
    TBasicResponse response;

    if (!CheckAllowedName(request.User)) {
        response.Error = "Name is not allowed";
        return response;
    }
    auto itUserCreate = Sids.emplace(request.User, TSidRecord{.Type = ESidType::USER});
    if (!itUserCreate.second) {
        if (itUserCreate.first->second.Type == ESidType::USER) {
            response.Error = "User already exists";
        } else {
            response.Error = "Account already exists";
        }
        return response;
    }

    TSidRecord& user = itUserCreate.first->second;
    user.Name = request.User;
    user.Hash = Impl->GenerateHash(request.Password);

    return response;
}

bool TLoginProvider::CheckSubjectExists(const std::string& name, const ESidType::SidType& type) {
    auto itSidModify = Sids.find(name);
    return itSidModify != Sids.end() && itSidModify->second.Type == type;
}

bool TLoginProvider::CheckUserExists(const std::string& name) {
    return CheckSubjectExists(name, ESidType::USER);
}

TLoginProvider::TBasicResponse TLoginProvider::ModifyUser(const TModifyUserRequest& request) {
    TBasicResponse response;

    auto itUserModify = Sids.find(request.User);
    if (itUserModify == Sids.end() || itUserModify->second.Type != ESidType::USER) {
        response.Error = "User not found";
        return response;
    }

    TSidRecord& user = itUserModify->second;
    user.Hash = Impl->GenerateHash(request.Password);

    return response;
}

TLoginProvider::TRemoveUserResponse TLoginProvider::RemoveUser(const TRemoveUserRequest& request) {
    TRemoveUserResponse response;

    auto itUserModify = Sids.find(request.User);
    if (itUserModify == Sids.end() || itUserModify->second.Type != ESidType::USER) {
        if (!request.MissingOk) {
            response.Error = "User not found";
        }
        return response;
    }

    auto itChildToParentIndex = ChildToParentIndex.find(request.User);
    if (itChildToParentIndex != ChildToParentIndex.end()) {
        for (const std::string& parent : itChildToParentIndex->second) {
            auto itGroup = Sids.find(parent);
            if (itGroup != Sids.end()) {
                response.TouchedGroups.emplace_back(itGroup->first);
                itGroup->second.Members.erase(request.User);
            }
        }
        ChildToParentIndex.erase(itChildToParentIndex);
    }

    Sids.erase(itUserModify);

    return response;
}

TLoginProvider::TBasicResponse TLoginProvider::CreateGroup(const TCreateGroupRequest& request) {
    TBasicResponse response;

    if (request.Options.CheckName && !CheckAllowedName(request.Group)) {
        response.Error = "Name is not allowed";
        return response;
    }
    auto itGroupCreate = Sids.emplace(request.Group, TSidRecord{.Type = ESidType::GROUP});
    if (!itGroupCreate.second) {
        if (itGroupCreate.first->second.Type == ESidType::GROUP) {
            response.Error = "Group already exists";
        } else {
            response.Error = "Account already exists";
        }
        return response;
    }

    TSidRecord& group = itGroupCreate.first->second;
    group.Name = request.Group;

    return response;
}

TLoginProvider::TBasicResponse TLoginProvider::AddGroupMembership(const TAddGroupMembershipRequest& request) {
    TBasicResponse response;

    auto itGroupModify = Sids.find(request.Group);
    if (itGroupModify == Sids.end() || itGroupModify->second.Type != ESidType::GROUP) {
        response.Error = "Group not found";
        return response;
    }

    if (Sids.count(request.Member) == 0) {
        response.Error = "Member account not found";
        return response;
    }

    TSidRecord& group = itGroupModify->second;

    if (group.Members.count(request.Member)) {
        response.Notice = TStringBuilder() << "Role \"" << request.Member << "\" is already a member of role \"" << group.Name << "\"";
    } else {
        group.Members.insert(request.Member);
    }

    ChildToParentIndex[request.Member].insert(request.Group);

    return response;
}

TLoginProvider::TBasicResponse TLoginProvider::RemoveGroupMembership(const TRemoveGroupMembershipRequest& request) {
    TBasicResponse response;

    auto itGroupModify = Sids.find(request.Group);
    if (itGroupModify == Sids.end() || itGroupModify->second.Type != ESidType::GROUP) {
        response.Error = "Group not found";
        return response;
    }

    TSidRecord& group = itGroupModify->second;

    if (!group.Members.count(request.Member)) {
        response.Warning = TStringBuilder() << "Role \"" << request.Member << "\" is not a member of role \"" << group.Name << "\"";
    } else {
        group.Members.erase(request.Member);
    }

    ChildToParentIndex[request.Member].erase(request.Group);

    return response;
}

TLoginProvider::TRenameGroupResponse TLoginProvider::RenameGroup(const TRenameGroupRequest& request) {
    TRenameGroupResponse response;

    if (request.Options.CheckName && !CheckAllowedName(request.NewName)) {
        response.Error = "Name is not allowed";
        return response;
    }

    auto itGroupModify = Sids.find(request.Group);
    if (itGroupModify == Sids.end() || itGroupModify->second.Type != ESidType::GROUP) {
        response.Error = "Group not found";
        return response;
    }

    auto itGroupCreate = Sids.emplace(request.NewName, TSidRecord{.Type = ESidType::GROUP});
    if (!itGroupCreate.second) {
        if (itGroupCreate.first->second.Type == ESidType::GROUP) {
            response.Error = "Group already exists";
        } else {
            response.Error = "Account already exists";
        }
        return response;
    }

    TSidRecord& group = itGroupCreate.first->second;
    group.Name = request.NewName;

    auto itChildToParentIndex = ChildToParentIndex.find(request.Group);
    if (itChildToParentIndex != ChildToParentIndex.end()) {
        ChildToParentIndex[request.NewName] = itChildToParentIndex->second;
        for (const std::string& parent : ChildToParentIndex[request.NewName]) {
            auto itGroup = Sids.find(parent);
            if (itGroup != Sids.end()) {
                response.TouchedGroups.emplace_back(itGroup->first);
                itGroup->second.Members.erase(request.Group);
                itGroup->second.Members.insert(request.NewName);
            }
        }
        ChildToParentIndex.erase(itChildToParentIndex);
    }

    for (const std::string& member : itGroupModify->second.Members) {
        ChildToParentIndex[member].erase(request.Group);
        ChildToParentIndex[member].insert(request.NewName);
    }

    Sids.erase(itGroupModify);

    return response;
}

TLoginProvider::TRemoveGroupResponse TLoginProvider::RemoveGroup(const TRemoveGroupRequest& request) {
    TRemoveGroupResponse response;

    auto itGroupModify = Sids.find(request.Group);
    if (itGroupModify == Sids.end() || itGroupModify->second.Type != ESidType::GROUP) {
        if (!request.MissingOk) {
            response.Error = "Group not found";
        }
        return response;
    }

    auto itChildToParentIndex = ChildToParentIndex.find(request.Group);
    if (itChildToParentIndex != ChildToParentIndex.end()) {
        for (const std::string& parent : itChildToParentIndex->second) {
            auto itGroup = Sids.find(parent);
            if (itGroup != Sids.end()) {
                response.TouchedGroups.emplace_back(itGroup->first);
                itGroup->second.Members.erase(request.Group);
            }
        }
        ChildToParentIndex.erase(itChildToParentIndex);
    }

    for (const std::string& member : itGroupModify->second.Members) {
        ChildToParentIndex[member].erase(request.Group);
    }

    Sids.erase(itGroupModify);

    return response;
}

std::vector<std::string> TLoginProvider::GetGroupsMembership(const std::string& member) {
    std::vector<std::string> groups;
    std::unordered_set<std::string> visited;
    std::deque<std::string> queue;
    queue.push_back(member);
    while (!queue.empty()) {
        std::string member = queue.front();
        queue.pop_front();
        auto itChildToParentIndex = ChildToParentIndex.find(member);
        if (itChildToParentIndex != ChildToParentIndex.end()) {
            for (const std::string& parent : itChildToParentIndex->second) {
                if (visited.insert(parent).second) {
                    queue.push_back(parent);
                    groups.push_back(parent);
                }
            }
        }
    }
    return groups;
}

TLoginProvider::TLoginUserResponse TLoginProvider::LoginUser(const TLoginUserRequest& request) {
    TLoginUserResponse response;
    if (request.ExternalAuth.empty()) {
        auto itUser = Sids.find(request.User);
        if (itUser == Sids.end() || itUser->second.Type != ESidType::USER) {
            response.Error = "Invalid user";
            return response;
        }

        if (!Impl->VerifyHash(request.Password, itUser->second.Hash)) {
            response.Error = "Invalid password";
            return response;
        }
    }

    if (Keys.empty() || Keys.back().PrivateKey.empty()) {
        response.Error = "No key to generate token";
        return response;
    }

    const TKeyRecord& key = Keys.back();

    auto keyId = ToString(key.KeyId);
    const auto& publicKey = key.PublicKey;
    const auto& privateKey = key.PrivateKey;

    // encode jwt
    auto now = std::chrono::system_clock::now();
    auto expires_at = now + MAX_TOKEN_EXPIRE_TIME;
    if (request.Options.ExpiresAfter != std::chrono::system_clock::duration::zero()) {
        expires_at = std::min(expires_at, now + request.Options.ExpiresAfter);
    }
    auto algorithm = jwt::algorithm::ps256(publicKey, privateKey);

    auto token = jwt::create()
            .set_key_id(keyId)
            .set_subject(request.User)
            .set_issued_at(now)
            .set_expires_at(expires_at);
    if (!Audience.empty()) {
        token.set_audience(Audience);
    }

    if (!request.ExternalAuth.empty()) {
        token.set_payload_claim(EXTERNAL_AUTH_CLAIM_NAME, jwt::claim(request.ExternalAuth));
    } else {
        if (request.Options.WithUserGroups) {
            auto groups = GetGroupsMembership(request.User);
            token.set_payload_claim(GROUPS_CLAIM_NAME, jwt::claim(picojson::value(std::vector<picojson::value>(groups.begin(), groups.end()))));
        }
    }

    auto encoded_token = token.sign(algorithm);

    response.Token = std::string(encoded_token);

    return response;
}

std::deque<TLoginProvider::TKeyRecord>::iterator TLoginProvider::FindKeyIterator(ui64 keyId) {
    if (!Keys.empty() && keyId >= Keys.front().KeyId && keyId <= Keys.back().KeyId) {
        auto it = std::next(Keys.begin(), keyId - Keys.front().KeyId);
        if (it->KeyId == keyId) {
            return it;
        }
    }
    return Keys.end();
}

const TLoginProvider::TKeyRecord* TLoginProvider::FindKey(ui64 keyId) {
    auto it = FindKeyIterator(keyId);
    if (it != Keys.end()) {
        return &(*it);
    }
    return nullptr;
}

TLoginProvider::TValidateTokenResponse TLoginProvider::ValidateToken(const TValidateTokenRequest& request) {
    TLoginProvider::TValidateTokenResponse response;
    try {
        jwt::decoded_jwt decoded_token = jwt::decode(request.Token);
        if (!Audience.empty()) {
            // we check audience manually because we wan't this error instead of wrong key id in case of databases mismatch
            auto audience = decoded_token.get_audience();
            if (audience.empty() || std::string(*audience.begin()) != Audience) {
                response.Error = "Wrong audience";
                return response;
            }
        }
        auto keyId = FromStringWithDefault<ui64>(decoded_token.get_key_id());
        const TKeyRecord* key = FindKey(keyId);
        if (key != nullptr) {
            auto verifier = jwt::verify()
                .allow_algorithm(jwt::algorithm::ps256(key->PublicKey));
            if (!Audience.empty()) {
                verifier.with_audience(std::set<std::string>({Audience}));
            }
            verifier.verify(decoded_token);
            response.User = decoded_token.get_subject();
            response.ExpiresAt = decoded_token.get_expires_at();
            if (decoded_token.has_payload_claim(GROUPS_CLAIM_NAME)) {
                const jwt::claim& groups = decoded_token.get_payload_claim(GROUPS_CLAIM_NAME);
                if (groups.get_type() == jwt::json::type::array) {
                    const picojson::array& array = groups.as_array();
                    std::vector<std::string> groups;
                    groups.resize(array.size());
                    for (size_t i = 0; i < array.size(); ++i) {
                        groups[i] = array[i].get<std::string>();
                    }
                    response.Groups = groups;
                }
            }
            if (decoded_token.has_payload_claim(EXTERNAL_AUTH_CLAIM_NAME)) {
                const jwt::claim& externalAuthClaim = decoded_token.get_payload_claim(EXTERNAL_AUTH_CLAIM_NAME);
                if (externalAuthClaim.get_type() == jwt::json::type::string) {
                    response.ExternalAuth = externalAuthClaim.as_string();
                }
            } else if (!Sids.empty()) {
                auto itUser = Sids.find(std::string(decoded_token.get_subject()));
                if (itUser == Sids.end()) {
                    response.Error = "Token is valid, but subject wasn't found";
                }
            }
        } else {
            if (Keys.empty()) {
                response.Error = "Security state is empty";
                response.ErrorRetryable = true;
            } else if (keyId < Keys.front().KeyId) {
                response.Error = "The key of this token has expired";
            } else if (keyId > Keys.back().KeyId) {
                response.Error = "The key of this token is not available yet";
                response.ErrorRetryable = true;
            } else {
                response.Error = "Key not found";
            }
        }
    } catch (const jwt::error::token_verification_exception& e) {
        response.Error = e.what(); // invalid token signature
    } catch (const std::invalid_argument& e) {
        response.Error = "Token is not in correct format";
        response.TokenUnrecognized = true;
    } catch (const std::runtime_error& e) {
        response.Error = "Base64 decoding failed or invalid json";
        response.TokenUnrecognized = true;
    } catch (const std::exception& e) {
        response.Error = e.what();
    }
    return response;
}

std::string TLoginProvider::GetTokenAudience(const std::string& token) {
    try {
        jwt::decoded_jwt decoded_token = jwt::decode(token);
        auto audience = decoded_token.get_audience();
        if (!audience.empty()) {
            return std::string(*audience.begin());
        }
    }
    catch (...) {
    }
    return {};
}

std::chrono::system_clock::time_point TLoginProvider::GetTokenExpiresAt(const std::string& token) {
    try {
        jwt::decoded_jwt decoded_token = jwt::decode(token);
        return decoded_token.get_expires_at();
    }
    catch (...) {
    }
    return {};
}

bool TLoginProvider::IsItTimeToRotateKeys() const {
    return Keys.empty()
        || Keys.back().PrivateKey.empty()
        || KeysRotationTime + KEYS_ROTATION_PERIOD < std::chrono::system_clock::now();
}

void TLoginProvider::RotateKeys() {
    std::vector<ui64> keysExpired;
    std::vector<ui64> keysAdded;
    RotateKeys(keysExpired, keysAdded);
}

void TLoginProvider::RotateKeys(std::vector<ui64>& keysExpired, std::vector<ui64>& keysAdded) {
    std::string publicKey;
    std::string privateKey;
    Impl->GenerateKeyPair(publicKey, privateKey);
    ui64 newKeyId;
    if (Keys.empty()) {
        newKeyId = 1;
    } else {
        newKeyId = Keys.back().KeyId + 1;
    }
    auto now = std::chrono::system_clock::now();
    Keys.push_back({
        .KeyId = newKeyId,
        .PublicKey = publicKey,
        .PrivateKey = privateKey,
        .ExpiresAt = now + KEY_EXPIRE_TIME,
    });
    keysAdded.push_back(newKeyId);
    while (Keys.size() > MAX_SERVER_KEYS || (!Keys.empty() && Keys.front().ExpiresAt <= now)) {
        ui64 oldKeyId = Keys.front().KeyId;
        Keys.pop_front();
        keysExpired.push_back(oldKeyId);
    }
    KeysRotationTime = now;
}

void TLoginProvider::TImpl::GenerateKeyPair(std::string& publicKey, std::string& privateKey) {
    static constexpr int bits = 2048;
    publicKey.clear();
    privateKey.clear();
    BIGNUM* bne = BN_new();
    int ret = BN_set_word(bne, RSA_F4);
    if (ret == 1) {
        RSA* r = RSA_new();
        ret = RSA_generate_key_ex(r, bits, bne, nullptr);
        if (ret == 1) {
            BIO* bioPublic = BIO_new(BIO_s_mem());
            ret = PEM_write_bio_RSA_PUBKEY(bioPublic, r);
            if (ret == 1) {
                BIO* bioPrivate = BIO_new(BIO_s_mem());
                ret = PEM_write_bio_RSAPrivateKey(bioPrivate, r, nullptr, nullptr, 0, nullptr, nullptr);
                size_t privateSize = BIO_pending(bioPrivate);
                size_t publicSize = BIO_pending(bioPublic);
                privateKey.resize(privateSize);
                publicKey.resize(publicSize);
                BIO_read(bioPrivate, &privateKey[0], privateSize);
                BIO_read(bioPublic, &publicKey[0], publicSize);
                BIO_free(bioPrivate);
            }
            BIO_free(bioPublic);
        }
        RSA_free(r);
    }
    BN_free(bne);
}

std::string TLoginProvider::TImpl::GenerateHash(const std::string& password) {
    char salt[SALT_SIZE];
    char hash[HASH_SIZE];
    RAND_bytes(reinterpret_cast<unsigned char*>(salt), SALT_SIZE);
    ArgonHasher->Hash(
        reinterpret_cast<const ui8*>(password.data()),
        password.size(),
        reinterpret_cast<ui8*>(salt),
        SALT_SIZE,
        reinterpret_cast<ui8*>(hash),
        HASH_SIZE);
    NJson::TJsonValue json;
    json["type"] = "argon2id";
    json["salt"] = Base64Encode(std::string_view(salt, SALT_SIZE));
    json["hash"] = Base64Encode(std::string_view(hash, HASH_SIZE));
    return NJson::WriteJson(json, false);
}

bool TLoginProvider::TImpl::VerifyHash(const std::string& password, const std::string& hashJson) {
    NJson::TJsonValue json;
    if (!NJson::ReadJsonTree(hashJson, &json)) {
        return false;
    }
    std::string type = json["type"].GetStringRobust();
    if (type != "argon2id") {
        return false;
    }
    std::string salt = Base64Decode(json["salt"].GetStringRobust());
    std::string hash = Base64Decode(json["hash"].GetStringRobust());
    return ArgonHasher->Verify(
        reinterpret_cast<const ui8*>(password.data()),
        password.size(),
        reinterpret_cast<const ui8*>(salt.data()),
        salt.size(),
        reinterpret_cast<const ui8*>(hash.data()),
        hash.size());
}

NLoginProto::TSecurityState TLoginProvider::GetSecurityState() const {
    NLoginProto::TSecurityState state;
    state.set_audience(Audience);
    {
        auto& pbPublicKeys = *state.mutable_publickeys();
        pbPublicKeys.Clear();
        for (const TKeyRecord& key : Keys) {
            NLoginProto::TPublicKey& publicKey = *pbPublicKeys.Add();
            publicKey.set_keyid(key.KeyId);
            publicKey.set_keydatapem(key.PublicKey);
            publicKey.set_expiresat(std::chrono::duration_cast<std::chrono::milliseconds>(key.ExpiresAt.time_since_epoch()).count());
            // no private key here
        }
    }
    {
        auto& pbSids = *state.mutable_sids();
        pbSids.Clear();
        for (const auto& [sidName, sidInfo] : Sids) {
            NLoginProto::TSid& sid = *pbSids.Add();
            sid.set_type(sidInfo.Type);
            sid.set_name(sidInfo.Name);
            for (const auto& subSid : sidInfo.Members) {
                sid.add_members(subSid);
            }
            // no user hash here
        }
    }
    return state;
}

void TLoginProvider::UpdateSecurityState(const NLoginProto::TSecurityState& state) {
    Audience = state.audience();
    {
        auto now = std::chrono::system_clock::now();
        while (Keys.size() > MAX_CLIENT_KEYS || (!Keys.empty() && Keys.front().ExpiresAt <= now)) {
            Keys.pop_front();
        }

        if (!Keys.empty() && state.publickeys_size() != 0) {
            auto keyId = state.publickeys(0).keyid();
            auto itKey = FindKeyIterator(keyId);
            Keys.erase(itKey, Keys.end()); // erase tail which we are going to reinsert later
        }

        for (const auto& pbPublicKey : state.publickeys()) {
            Keys.push_back({
                .KeyId = pbPublicKey.keyid(),
                .PublicKey = pbPublicKey.keydatapem(),
                .ExpiresAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(pbPublicKey.expiresat())),
            });
        }
    }
    {
        Sids.clear();
        ChildToParentIndex.clear();
        for (const auto& pbSid : state.sids()) {
            TSidRecord& sid = Sids[pbSid.name()];
            sid.Type = pbSid.type();
            sid.Name = pbSid.name();
            sid.Hash = pbSid.hash();
            for (const auto& pbSubSid : pbSid.members()) {
                sid.Members.emplace(pbSubSid);
                ChildToParentIndex[pbSubSid].emplace(sid.Name);
            }
        }
    }
}

}
