#define NOMINMAX
#include "chap_ms.h"
#include "chap_new.h"
#include "pppcrypt.h"
#include "magic.h"
#include "mppe.h"
#include "ccp.h"
#include "util.h"
#include "spdlog/spdlog.h"
#include "mbedtls/des.h"
#include "mbedtls/sha1.h"
#include "mbedtls/md4.h"
#include <string>
#include <array>
#include <cstdint>
// #include <locale>
// #include <codecvt>

/**
 * chapms_generate_challenge - generate a challenge for MS-CHAP.
 * For MS-CHAP the challenge length is fixed at 8 bytes.
 * The length goes in challenge[0] and the actual challenge starts
 * at challenge[1].
 */
void
chapms_generate_challenge(PppPcb& pcb, std::vector<uint8_t>& challenge)
{
    size_t ptr = 0;
    challenge[ptr++] = 8;
    if (strlen(MSCHAP_CHALLENGE) == 8) memcpy(challenge.data(), MSCHAP_CHALLENGE, 8);
    else magic_random_bytes(challenge, 8, ptr);
}

/**
 *
 */
void
chapms2_generate_challenge(PppPcb& pcb, std::vector<uint8_t>& challenge)
{
    size_t ptr = 0;
    challenge[ptr++] = 16;
    if (MSCHAP_CHALLENGE && strlen(MSCHAP_CHALLENGE) == 16) memcpy(
        challenge.data + ptr,
        MSCHAP_CHALLENGE,
        16);
    else magic_random_bytes(challenge, 16, ptr);
}


bool
chapms_verify_response(PppPcb& pcb,
                       int id,
                       std::string& name,
                       std::string& secret,
                       std::vector<uint8_t>& challenge,
                       std::vector<uint8_t>& response,
                       std::string& message,
                       const int message_space)
{
    //unsigned char md[MS_CHAP_RESPONSE_LEN];
    size_t chall_ptr = 0;
    size_t resp_ptr = 0;
    std::vector<uint8_t> md;
    md.reserve(MS_CHAP_RESPONSE_LEN);
    int diff;
    const int challenge_len = challenge[chall_ptr++]; /* skip length, is 8 */
    const int response_len = response[resp_ptr++];
    if (response_len != MS_CHAP_RESPONSE_LEN) {
        message.append(fmt::format("E=691 R=1 C={} V=0",
                       reinterpret_cast<char*>(challenge.data())));
        return false;
    }
    if (!response[MS_CHAP_USENT]) {
        /* Should really propagate this into the error packet. */
        spdlog::info("Peer request for LANMAN auth not supported");
        message.append(fmt::format("E=691 R=1 C={} V=0", reinterpret_cast<char*>(challenge.data())));
        return false;
    }
    // Generate the expected response.
    chap_ms(pcb, challenge, secret, md,chall_ptr,resp_ptr);
    /* Determine which part of response to verify against */
    if (!response[MS_CHAP_USENT])
        diff = memcmp(&response[MS_CHAP_LANMANRESP],
                      &md[MS_CHAP_LANMANRESP],
                      MS_CHAP_LANMANRESP_LEN);
    else {
        diff = memcmp(&response[MS_CHAP_NTRESP], &md[MS_CHAP_NTRESP], MS_CHAP_NTRESP_LEN);
    }
    if (diff == 0) {
        // ppp_slprintf(message, message_space, "Access granted");
        message = "access granted";
        return 1;
    } // ppp_slprintf(message,
    //              message_space,
    //              "E=691 R=1 C=%0.*B V=0",
    //              challenge_len,
    //              challenge);
    message = "E=691 R=1 C=";
    message += (const char*)challenge.data();
    message += " V=0";
    return 0;
}


int
chapms2_verify_response(PppPcb* pcb,
                        int id,
                        std::string& name,
                        std::string& secret,
                        const unsigned char* challenge,
                        const unsigned char* response,
                        std::string& message,
                        int message_space)
{
    unsigned char md[MS_CHAP2_RESPONSE_LEN];
    char saresponse[MS_AUTH_RESPONSE_LENGTH + 1];
    const int challenge_len = *challenge++; /* skip length, is 16 */
    const int response_len = *response++;
    if (response_len != MS_CHAP2_RESPONSE_LEN) {
        // "E=691 R=1 C=%0.*B V=0 M=%s"
        message = "E=691 R=1 C=";
        message += (const char*)challenge;
        message += " V=0 M=";
        message += "Access denied"; // ppp_slprintf(message,
        //              message_space,
        //              "E=691 R=1 C=%0.*B V=0 M=%s",
        //              challenge_len,
        //              challenge,
        //              "Access denied");
        return 0; /* not even the right length */
    } /// Generate the expected response and our mutual auth.
    ChapMS2(pcb,
            challenge,
            &response[MS_CHAP2_PEER_CHALLENGE],
            name,
            secret,
            md,
            reinterpret_cast<unsigned char *>(saresponse),
            MS_CHAP2_AUTHENTICATOR); /* compare MDs and send the appropriate status */ /*
     * Per RFC 2759, success message must be formatted as
     *     "S=<auth_string> M=<message>"
     * where
     *     <auth_string> is the Authenticator Response (mutual auth)
     *     <message> is a text message
     *
     * However, some versions of Windows (win98 tested) do not know
     * about the M=<message> part (required per RFC 2759) and flag
     * it as an error (reported incorrectly as an encryption error
     * to the user).  Since the RFC requires it, and it can be
     * useful information, we supply it if the peer is a conforming
     * system.  Luckily (?), win98 sets the Flags field to 0x04
     * (contrary to RFC requirements) so we can use that to
     * distinguish between conforming and non-conforming systems.
     *
     * Special thanks to Alex Swiridov <say@real.kharkov.ua> for
     * help debugging this.
     */
    if (memcmp(&md[MS_CHAP2_NTRESP], &response[MS_CHAP2_NTRESP], MS_CHAP2_NTRESP_LEN) == 0
    ) {
        if (response[MS_CHAP2_FLAGS]) {
            // ppp_slprintf(message, message_space, "S=%s", saresponse);
            message = "S=";
            message += saresponse;
        }
        else {
            // ppp_slprintf(message,
            //              message_space,
            //              "S=%s M=%s",
            //              saresponse,
            //              "Access granted");
            message = "S=";
            message += saresponse;
            message += " M=";
            message += "Access granted";
        }
        return 1;
    } /*
     * Failure message must be formatted as
     *     "E=e R=r C=c V=v M=m"
     * where
     *     e = error code (we use 691, ERROR_AUTHENTICATION_FAILURE)
     *     r = retry (we use 1, ok to retry)
     *     c = challenge to use for next response, we reuse previous
     *     v = Change Password version supported, we use 0
     *     m = text message
     *
     * The M=m part is only for MS-CHAPv2.  Neither win2k nor
     * win98 (others untested) display the message to the user anyway.
     * They also both ignore the E=e code.
     *
     * Note that it's safe to reuse the same challenge as we don't
     * actually accept another response based on the error message
     * (and no clients try to resend a response anyway).
     *
     * Basically, this whole bit is useless code, even the small
     * implementation here is only because of overspecification.
     */ // ppp_slprintf(message,
    //                  message_space,
    //                  "E=691 R=1 C=%0.*B V=0 M=%s",
    //                  challenge_len,
    //                  challenge,
    //     "Access denied");
    // "E=691 R=1 C=%0.*B V=0 M=%s"
    message = "E=691 R=1 C=";
    message += reinterpret_cast<const char*>(challenge);
    message += " V=0 M=";
    message += "Access denied";
    return 0;
}


void
chapms_make_response(PppPcb& pcb,
                     std::vector<uint8_t>& response,
                     int id,
                     std::string& our_name,
                     std::vector<uint8_t>& challenge,
                     std::string& secret,
                     std::vector<uint8_t>& private_)
{
    size_t chall_ptr = 0;
    chall_ptr++;
    // challenge++; /* skip length, should be 8 */
    // *response++ = MS_CHAP_RESPONSE_LEN;
    size_t resp_ptr = 0;
    response[resp_ptr++] = MS_CHAP_RESPONSE_LEN;

    chap_ms(pcb, challenge, secret, response,chall_ptr,resp_ptr);
}


void
chapms2_make_response(PppPcb* pcb,
                      unsigned char* response,
                      int id,
                      std::string& our_name,
                      const unsigned char* challenge,
                      std::string& secret,
                      unsigned char* private_)
{
    challenge++; /* skip length, should be 16 */
    *response++ = MS_CHAP2_RESPONSE_LEN;
    ChapMS2(pcb,
            challenge,
            nullptr,
            our_name,
            secret,
            response,
            private_,
            MS_CHAP2_AUTHENTICATEE);
}


int
chapms2_check_success(PppPcb* pcb, unsigned char* msg, int len, unsigned char* private_)
{
    if ((len < MS_AUTH_RESPONSE_LENGTH + 2) || strncmp((char *)msg, "S=", 2) != 0) {
        /* Packet does not start with "S=" */
        spdlog::error("MS-CHAPv2 Success packet is badly formed.");
        return 0;
    }
    msg += 2;
    len -= 2;
    if (len < MS_AUTH_RESPONSE_LENGTH || memcmp(msg, private_, MS_AUTH_RESPONSE_LENGTH)) {
        /* Authenticator Response did not match expected. */
        spdlog::error("MS-CHAPv2 mutual authentication failed.");
        return 0;
    } /* Authenticator Response matches. */
    msg += MS_AUTH_RESPONSE_LENGTH; /* Eat it */
    len -= MS_AUTH_RESPONSE_LENGTH;
    if ((len >= 3) && !strncmp((char *)msg, " M=", 3)) {
        msg += 3; /* Eat the delimiter */
    }
    else if (len) {
        /* Packet has extra text which does not begin " M=" */
        spdlog::error("MS-CHAPv2 Success packet is badly formed.");
        return 0;
    }
    return 1;
}


void
chapms_handle_failure(PppPcb* pcb, unsigned char* inp, int len)
{
    int err;
    char msg[64]; /* We want a null-terminated string for strxxx(). */
    len = std::min(len, 63);
    memcpy(msg, inp, len);
    msg[len] = 0;
    const char* p = msg; /*
     * Deal with MS-CHAP formatted failure messages; just print the
     * M=<message> part (if any).  For MS-CHAP we're not really supposed
     * to use M=<message>, but it shouldn't hurt.  See
     * chapms[2]_verify_response.
     */
    if (!strncmp(p, "E=", 2)) {
        err = strtol(p + 2, nullptr, 10); /* Remember the error code. */
    }
    else { goto print_msg; /* Message is badly formatted. */ }
    if (len && ((p = strstr(p, " M=")) != nullptr)) {
        /* M=<message> field found. */
        p += 3;
    }
    else {
        /* No M=<message>; use the error code. */
        switch (err) {
        case MS_CHAP_ERROR_RESTRICTED_LOGON_HOURS: p = "E=646 Restricted logon hours";
            break;
        case MS_CHAP_ERROR_ACCT_DISABLED: p = "E=647 Account disabled";
            break;
        case MS_CHAP_ERROR_PASSWD_EXPIRED: p = "E=648 Password expired";
            break;
        case MS_CHAP_ERROR_NO_DIALIN_PERMISSION: p = "E=649 No dialin permission";
            break;
        case MS_CHAP_ERROR_AUTHENTICATION_FAILURE: p = "E=691 Authentication failure";
            break;
        case MS_CHAP_ERROR_CHANGING_PASSWORD:
            /* Should never see this, we don't support Change Password. */ p =
                "E=709 Error changing password";
            break;
        default: spdlog::error("Unknown MS-CHAP authentication failure: %.*v", len, inp);
            return;
        }
    }
print_msg: if (p != nullptr) { spdlog::error("MS-CHAP authentication failed: %v", p); }
}


/**
 *
 */
std::tuple<bool, std::vector<uint8_t>>
challenge_response(std::vector<uint8_t>& challenge,
                   size_t challenge_offset,
                   std::vector<uint8_t>& password_hash)
{
    std::vector<uint8_t> response(256);
    std::array<uint8_t, 21> z_password_hash;
    std::array<uint8_t, 8> des_key;
    mbedtls_des_context des_ctx;
    // memset(z_password_hash, 0, sizeof(z_password_hash));
    // memcpy(z_password_hash, password_hash.data(), MD4_SIGNATURE_SIZE);
    pppcrypt_56_to_64_bit_key(z_password_hash.data() + 0, des_key.data());
    // lwip_des_init(&des);
    mbedtls_des_setkey_dec(&des_ctx, des_key.data());
    mbedtls_des_crypt_ecb(&des_ctx, challenge.data(), response.data() + 0);
    // lwip_des_free(&des);
    pppcrypt_56_to_64_bit_key(z_password_hash.data() + 7, des_key.data());
    // lwip_des_init(&des);
    mbedtls_des_setkey_enc(&des_ctx, des_key.data());
    mbedtls_des_crypt_ecb(&des_ctx, challenge.data(), response.data() + 8);
    // lwip_des_free(&des);
    pppcrypt_56_to_64_bit_key(z_password_hash.data() + 14, des_key.data());
    // lwip_des_init(&des);
    mbedtls_des_setkey_enc(&des_ctx, des_key.data());
    mbedtls_des_crypt_ecb(&des_ctx, challenge.data(), response.data() + 16);
    // lwip_des_free(&des);
    return std::make_tuple(true, response);
}


std::tuple<bool, std::vector<uint8_t>>
challenge_hash(std::vector<uint8_t>& peer_challenge,
               std::vector<uint8_t>& rchallenge,
               std::string& username)
{
    std::vector<uint8_t> challenge;
    mbedtls_sha1_context sha1_context;
    uint8_t sha1_hash[SHA1_SIGNATURE_SIZE] = {};
    const char* user = username.c_str(); /* remove domain from "domain\username" */
    // TODO: re-write to remove domain from username
    // if
    // if ((user = strrchr(username, '\\')) != nullptr)
    //     ++user;
    // else
    //     user = username;
    mbedtls_sha1_init(&sha1_context);
    mbedtls_sha1_starts_ret(&sha1_context);
    mbedtls_sha1_update_ret(&sha1_context, peer_challenge.data(), 16);
    mbedtls_sha1_update_ret(&sha1_context, rchallenge.data(), 16);
    mbedtls_sha1_update_ret(&sha1_context,
                            reinterpret_cast<const unsigned char*>(user),
                            strlen(user));
    mbedtls_sha1_finish_ret(&sha1_context, sha1_hash);
    mbedtls_sha1_free(&sha1_context);
    std::copy(sha1_hash, sha1_hash + SHA1_SIGNATURE_SIZE - 1, challenge);
    std::make_tuple(true, challenge);
}


std::vector<uint8_t>
nt_password_hash(std::vector<uint8_t>& secret)
{
    std::vector<uint8_t> hash;
    mbedtls_md4_context md4Context;
    mbedtls_md4_init(&md4Context);
    mbedtls_md4_starts_ret(&md4Context);
    mbedtls_md4_update_ret(&md4Context, secret.data(), secret.size());
    mbedtls_md4_finish_ret(&md4Context, hash.data());
    mbedtls_md4_free(&md4Context);
    return hash;
}


void
chap_ms_nt(std::vector<uint8_t>& r_challenge,
           std::string& secret,
           std::vector<uint8_t>& nt_response,
           size_t challenge_offset,
           size_t response_offset)
{
    std::wstring unicode_password;
    unicode_password.reserve(MAX_NT_PASSWORD * 2);
    // uint8_t unicodePassword[MAX_NT_PASSWORD * 2];
    // uint8_t PasswordHash[MD4_SIGNATURE_SIZE];
    std::vector<uint8_t> password_hash;
    password_hash.reserve(MD4_SIGNATURE_SIZE);
    std::wstring_convert<std::codecvt_utf8<char>> converter;
    unicode_password = converter.from_bytes(secret);
    std::vector<uint8_t> unicode_password_vector(unicode_password.begin(), unicode_password.end());
    /* Hash the Unicode version of the secret (== password). */
    password_hash = nt_password_hash(unicode_password_vector);
    challenge_response(r_challenge, 0, password_hash);
}


/**
 *
 */
std::tuple<bool, std::vector<uint8_t>>
chap_ms2_nt(std::vector<uint8_t>& rchallenge,
            std::vector<uint8_t>& peer_challenge,
            std::string& username,
            std::string& secret)
{
    bool ok;
    std::vector<uint8_t> challenge;
    std::vector<uint8_t> response;
    std::tie(ok, challenge) = challenge_hash(peer_challenge, rchallenge, username);
    /* Hash the Unicode version of the secret (== password). */
    std::wstring converted_secret;
    std::tie(ok, converted_secret) = ascii_to_unicode(secret);
    if (!ok) { return std::make_tuple(false, response); }
    std::vector<uint8_t> converted_secret_vec(converted_secret.begin(),
                                              converted_secret.end());
    auto password_hash = nt_password_hash(converted_secret_vec);
    return challenge_response(challenge, 0, password_hash);
}


std::tuple<bool, std::vector<uint8_t>>
chap_ms_lanman(std::vector<uint8_t>& rchallenge,
               std::string& secret,
               size_t rchallenge_offset)
{
    std::vector<uint8_t> ucase_password(MAX_NT_PASSWORD);
    std::vector<uint8_t> password_hash(MD4_SIGNATURE_SIZE);
    mbedtls_des_context des_ctx;
    std::array<uint8_t, 8> des_key;
    /* LANMan password is case insensitive */
    for (auto& c : secret) { ucase_password.push_back(toupper(c)); }
    pppcrypt_56_to_64_bit_key(ucase_password.data() + 0, des_key.data());
    // lwip_des_init(&des);
    mbedtls_des_setkey_enc(&des_ctx, des_key.data());
    mbedtls_des_crypt_ecb(&des_ctx, (uint8_t*)STD_TEXT, password_hash.data() + 0);
    // lwip_des_free(&des);
    pppcrypt_56_to_64_bit_key(ucase_password.data() + 7, des_key.data());
    // lwip_des_init(&des);
    mbedtls_des_setkey_enc(&des_ctx, des_key.data());
    mbedtls_des_crypt_ecb(&des_ctx,
                          (uint8_t*)STD_TEXT,
                          password_hash.data() + LANMAN_KEY_LEN);
    // lwip_des_free(&des);
    return challenge_response(rchallenge, 0, password_hash);
}


/**
 * password_hash_hash should be MD4_SIGNATURE_SIZE bytes long
 * nt_response should be 24 bytes long
 * peer_challenge should be 16 bytes long
 * auth_response should MS_AUTH_RESPONSE_LENGTH + 1
 */
std::tuple<bool, std::vector<uint8_t>>
gen_authenticator_resp(std::vector<uint8_t>& password_hash_hash,
                       std::vector<uint8_t>& nt_response,
                       std::vector<uint8_t>& peer_challenge,
                       std::vector<uint8_t>& rchallenge,
                       std::string& username)
{
    mbedtls_sha1_context sha1_ctx;
    std::array<uint8_t, SHA1_SIGNATURE_SIZE> digest{};
    std::array<uint8_t, 8> challenge{};
    std::vector<uint8_t> auth_response(SHA1_SIGNATURE_SIZE);
    mbedtls_sha1_init(&sha1_ctx);
    mbedtls_sha1_starts_ret(&sha1_ctx);
    mbedtls_sha1_update_ret(&sha1_ctx, password_hash_hash.data(), MD4_SIGNATURE_SIZE);
    mbedtls_sha1_update_ret(&sha1_ctx, nt_response.data(), 24);
    mbedtls_sha1_update_ret(&sha1_ctx, const_cast<uint8_t*>(MAGIC_1), sizeof(MAGIC_1));
    mbedtls_sha1_finish_ret(&sha1_ctx, digest.data());
    mbedtls_sha1_free(&sha1_ctx);
    challenge_hash(peer_challenge, rchallenge, username);
    mbedtls_sha1_init(&sha1_ctx);
    mbedtls_sha1_starts_ret(&sha1_ctx);
    mbedtls_sha1_update_ret(&sha1_ctx, digest.data(), sizeof(digest));
    mbedtls_sha1_update_ret(&sha1_ctx, challenge.data(), 8);
    mbedtls_sha1_update_ret(&sha1_ctx, const_cast<uint8_t*>(MAGIC_2), sizeof(MAGIC_2));
    mbedtls_sha1_finish_ret(&sha1_ctx, digest.data());
    mbedtls_sha1_free(&sha1_ctx); /* Convert to ASCII hex string. */
    std::copy(digest.begin(), digest.end(), auth_response.begin());
    // for (int i = 0; i < std::max((MS_AUTH_RESPONSE_LENGTH / 2), (int)sizeof(digest)); i++
    // ) { sprintf((char *)&auth_response[i * 2], "%02X", digest[i]); }
    return std::make_tuple(true, auth_response);
}


/**
 * NT response length is 24 bytes
 * Peer challenge length is 16 bytes
 * Auth response length is MS_AUTH_RESPONSE_LENGTH + 1
 */
std::tuple<bool, std::vector<uint8_t>>
gen_authenticator_response_plain(std::string& secret,
                                   std::vector<uint8_t>& NTResponse,
                                   std::vector<uint8_t>& PeerChallenge,
                                   std::vector<uint8_t>& rchallenge,
                                   std::string& username,
                                   std::vector<uint8_t>& authResponse)
{
    std::vector<uint8_t> auth_response(MS_AUTH_RESPONSE_LENGTH + 1);
    // uint8_t unicodePassword[MAX_NT_PASSWORD * 2];
    std::array<uint8_t, MAX_NT_PASSWORD * 2> unicode_password = {};
    // uint8_t PasswordHash[MD4_SIGNATURE_SIZE];
    std::array<uint8_t, MD4_SIGNATURE_SIZE> password_hash = {};
    // uint8_t PasswordHashHash[MD4_SIGNATURE_SIZE];
    std::array<uint8_t, MD4_SIGNATURE_SIZE> password_hash_hash = {};
    /* Hash (x2) the Unicode version of the secret (== password). */
    bool ok;
    ascii_to_unicode(secret.c_str(), secret.length(), unicodePassword);
    nt_password_hash(unicodePassword);
    nt_password_hash(PasswordHash);
    gen_authenticator_resp(PasswordHashHash,
                           NTResponse,
                           PeerChallenge,
                           rchallenge,
                           username);
}


/**
 * Set mppe_xxxx_key from MS-CHAP credentials. (see RFC 3079)
 */
bool
set_start_key(PppPcb& pcb, std::vector<uint8_t>& rchallenge, std::string& secret)
{
    // uint8_t unicodePassword[MAX_NT_PASSWORD * 2];

    std::vector<uint8_t> unicode_password_vec;

    uint8_t PasswordHash[MD4_SIGNATURE_SIZE];
    uint8_t PasswordHashHash[MD4_SIGNATURE_SIZE];
    mbedtls_sha1_context sha1Context;
    uint8_t Digest[SHA1_SIGNATURE_SIZE]; /* >= MPPE_MAX_KEY_LEN */
    /* Hash (x2) the Unicode version of the secret (== password). */
    ascii_to_unicode(secret.c_str(), secret.length(), unicodePassword);
    nt_password_hash(unicodePassword);
    nt_password_hash(PasswordHash);
    mbedtls_sha1_init(&sha1Context);
    mbedtls_sha1_starts_ret(&sha1Context);
    mbedtls_sha1_update_ret(&sha1Context, PasswordHashHash, MD4_SIGNATURE_SIZE);
    mbedtls_sha1_update_ret(&sha1Context, PasswordHashHash, MD4_SIGNATURE_SIZE);
    mbedtls_sha1_update_ret(&sha1Context, rchallenge.data(), 8);
    mbedtls_sha1_finish_ret(&sha1Context, Digest);
    mbedtls_sha1_free(&sha1Context); /* Same key in both directions. */
    mppe_set_key(pcb.mppe_comp, Digest);
    mppe_set_key(pcb.mppe_decomp, Digest);
    pcb.mppe_keys_set = true;
}

/**
 * Set mppe_xxxx_key from MS-CHAPv2 credentials. (see RFC 3079)
 */
void
SetMasterKeys(PppPcb* pcb, std::string& secret, uint8_t NTResponse[24], int IsServer)
{
    uint8_t unicodePassword[MAX_NT_PASSWORD * 2];
    uint8_t PasswordHash[MD4_SIGNATURE_SIZE];
    uint8_t PasswordHashHash[MD4_SIGNATURE_SIZE];
    mbedtls_sha1_context sha1Context;
    uint8_t MasterKey[SHA1_SIGNATURE_SIZE]; /* >= MPPE_MAX_KEY_LEN */
    uint8_t Digest[SHA1_SIGNATURE_SIZE]; /* >= MPPE_MAX_KEY_LEN */
    const uint8_t* s; /* Hash (x2) the Unicode version of the secret (== password). */
    ascii_to_unicode(secret.c_str(), secret.length(), unicodePassword);
    nt_password_hash(unicodePassword);
    nt_password_hash(PasswordHash);
    mbedtls_sha1_init(&sha1Context);
    mbedtls_sha1_starts_ret(&sha1Context);
    mbedtls_sha1_update_ret(&sha1Context, PasswordHashHash, MD4_SIGNATURE_SIZE);
    mbedtls_sha1_update_ret(&sha1Context, NTResponse, 24);
    mbedtls_sha1_update_ret(&sha1Context, Magic4, sizeof(Magic4));
    mbedtls_sha1_finish_ret(&sha1Context, MasterKey);
    mbedtls_sha1_free(&sha1Context); /*
     * generate send key
     */
    if (IsServer) s = Magic3;
    else { s = Magic5; }
    mbedtls_sha1_init(&sha1Context);
    mbedtls_sha1_starts_ret(&sha1Context);
    mbedtls_sha1_update_ret(&sha1Context, MasterKey, 16);
    mbedtls_sha1_update_ret(&sha1Context, MPPE_SHA1_PAD1, SHA1_PAD_SIZE);
    mbedtls_sha1_update_ret(&sha1Context, s, 84);
    mbedtls_sha1_update_ret(&sha1Context, MPPE_SHA1_PAD2, SHA1_PAD_SIZE);
    mbedtls_sha1_finish_ret(&sha1Context, Digest);
    mbedtls_sha1_free(&sha1Context);
    mppe_set_key(&pcb->mppe_comp, Digest); /*
     * generate recv key
     */
    if (IsServer) { s = Magic5; }
    else { s = Magic3; }
    lwip_sha1_init(&sha1Context);
    mbedtls_sha1_starts_ret(&sha1Context);
    mbedtls_sha1_update_ret(&sha1Context, MasterKey, 16);
    mbedtls_sha1_update_ret(&sha1Context, MPPE_SHA1_PAD1, SHA1_PAD_SIZE);
    mbedtls_sha1_update_ret(&sha1Context, s, 84);
    mbedtls_sha1_update_ret(&sha1Context, MPPE_SHA1_PAD2, SHA1_PAD_SIZE);
    mbedtls_sha1_finish_ret(&sha1Context, Digest);
    mbedtls_sha1_free(&sha1Context);
    mppe_set_key(&pcb->mppe_decomp, Digest);
    pcb->mppe_keys_set = true;
}


void
chap_ms(PppPcb& pcb,
        std::vector<uint8_t>& challenge,
        std::string& secret,
        std::vector<uint8_t>& response,
        const size_t challenge_offset,
        const size_t response_offset)
{
    response.erase(response.begin() + challenge_offset);
    chap_ms_nt(challenge, secret, response, challenge_offset, MS_CHAP_NTRESP + response_offset);
    chap_ms_lanman(challenge, secret, );
    /* preferred method is set by option  */
    response[MS_CHAP_USENT] = !ms_lanman;
    set_start_key(pcb, rchallenge, secret);
}

/*
 * If PeerChallenge is NULL, one is generated and the PeerChallenge
 * field of response is filled in.  Call this way when generating a response.
 * If PeerChallenge is supplied, it is copied into the PeerChallenge field.
 * Call this way when verifying a response (or debugging).
 * Do not call with PeerChallenge = response.
 *
 * The PeerChallenge field of response is then used for calculation of the
 * Authenticator Response.
 */
void
ChapMS2(PppPcb* pcb,
        const uint8_t* rchallenge,
        const uint8_t* PeerChallenge,
        std::string& user,
        std::string& secret,
        unsigned char* response,
        uint8_t authResponse[],
        int authenticator)
{
    /* ARGSUSED */
    zero_mem(response, MS_CHAP2_RESPONSE_LEN);
    /* Generate the Peer-Challenge if requested, or copy it if supplied. */
    if (!PeerChallenge) { magic_random_bytes(&response[MS_CHAP2_PEER_CHALLENGE], , ); }
    else {
        memcpy(&response[MS_CHAP2_PEER_CHALLENGE], PeerChallenge, MS_CHAP2_PEER_CHAL_LEN);
    } /* Generate the NT-Response */
    chap_ms2_nt(rchallenge,
                &response[MS_CHAP2_PEER_CHALLENGE],
                user,
                secret); /* Generate the Authenticator Response. */
    ge_authenticator_response_plain(secret,
                                       &response[MS_CHAP2_NTRESP],
                                       &response[MS_CHAP2_PEER_CHALLENGE],
                                       rchallenge,
                                       user,
                                       authResponse);
    SetMasterKeys(pcb, secret, &response[MS_CHAP2_NTRESP], authenticator);
}


const struct ChapDigestType CHAP_MS_DIGEST = {
    CHAP_MICROSOFT,
    /* code */
    chapms_generate_challenge,
    chapms_verify_response,
    chapms_make_response,
    nullptr,
    /* check_success */
    chapms_handle_failure,
};
const struct ChapDigestType CHAP_MS_2_DIGEST = {
    CHAP_MICROSOFT_V2,
    /* code */
    chapms2_generate_challenge,
    chapms2_verify_response,
    chapms2_make_response,
    chapms2_check_success,
    chapms_handle_failure,
};
