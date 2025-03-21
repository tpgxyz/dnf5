/*
Copyright Contributors to the libdnf project.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 2.1 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "librepo.hpp"

#include "utils/bgettext/bgettext-mark-domain.h"

#include "libdnf/repo/repo_errors.hpp"

namespace libdnf::repo {

// Map string from config option proxy_auth_method to librepo LrAuth value
static constexpr struct {
    const char * name;
    LrAuth code;
} PROXYAUTHMETHODS[] = {
    {"none", LR_AUTH_NONE},
    {"basic", LR_AUTH_BASIC},
    {"digest", LR_AUTH_DIGEST},
    {"negotiate", LR_AUTH_NEGOTIATE},
    {"ntlm", LR_AUTH_NTLM},
    {"digest_ie", LR_AUTH_DIGEST_IE},
    {"ntlm_wb", LR_AUTH_NTLM_WB},
    {"any", LR_AUTH_ANY}};

/// Converts the given input string to a URL encoded string
/// All input characters that are not a-z, A-Z, 0-9, '-', '.', '_' or '~' are converted
/// to their "URL escaped" version (%NN where NN is a two-digit hexadecimal number).
/// @param src String to encode
/// @return URL encoded string
static std::string url_encode(const std::string & src) {
    auto no_encode = [](char ch) { return isalnum(ch) != 0 || ch == '-' || ch == '.' || ch == '_' || ch == '~'; };

    // compute length of encoded string
    auto len = src.length();
    for (auto ch : src) {
        if (!no_encode(ch)) {
            len += 2;
        }
    }

    // encode the input string
    std::string encoded;
    encoded.reserve(len);
    for (auto ch : src) {
        if (no_encode(ch)) {
            encoded.push_back(ch);
        } else {
            encoded.push_back('%');
            int hex;
            hex = static_cast<unsigned char>(ch) >> 4;
            hex += hex <= 9 ? '0' : 'a' - 10;
            encoded.push_back(static_cast<char>(hex));
            hex = static_cast<unsigned char>(ch) & 0x0F;
            hex += hex <= 9 ? '0' : 'a' - 10;
            encoded.push_back(static_cast<char>(hex));
        }
    }

    return encoded;
}

/// Format user password string
/// Returns user and password in user:password form. If quote is True,
/// special characters in user and password are URL encoded.
/// @param user Username
/// @param passwd Password
/// @param encode If quote is True, special characters in user and password are URL encoded.
/// @return User and password in user:password form
static std::string format_user_pass_string(const std::string & user, const std::string & passwd, bool encode) {
    if (encode) {
        return url_encode(user) + ":" + url_encode(passwd);
    } else {
        return user + ":" + passwd;
    }
}

LibrepoResult & LibrepoResult::operator=(LibrepoResult && other) noexcept {
    if (this != &other) {
        lr_result_free(result);
        result = other.result;
        other.result = nullptr;
    }
    return *this;
}

LibrepoHandle & LibrepoHandle::operator=(LibrepoHandle && other) noexcept {
    if (this != &other) {
        lr_handle_free(handle);
        handle = other.handle;
        other.handle = nullptr;
    }
    return *this;
}

template <typename C>
static void init_remote(LibrepoHandle & handle, const C & config) {
    handle.set_opt(LRO_USERAGENT, config.user_agent().get_value().c_str());

    auto minrate = config.minrate().get_value();
    auto maxspeed = config.throttle().get_value();
    if (maxspeed > 0 && maxspeed <= 1) {
        maxspeed *= static_cast<float>(config.bandwidth().get_value());
    }
    if (maxspeed != 0 && maxspeed < static_cast<float>(minrate)) {
        // TODO(lukash) not the best class for the error, possibly check in config parser?
        throw libdnf::repo::RepoDownloadError(
            M_("Maximum download speed is lower than minimum, "
               "please change configuration of minrate or throttle"));
    }
    handle.set_opt(LRO_LOWSPEEDLIMIT, static_cast<int64_t>(minrate));
    handle.set_opt(LRO_MAXSPEED, static_cast<int64_t>(maxspeed));

    long timeout = config.timeout().get_value();
    if (timeout > 0) {
        handle.set_opt(LRO_CONNECTTIMEOUT, timeout);
        handle.set_opt(LRO_LOWSPEEDTIME, timeout);
    }

    auto & ip_resolve = config.ip_resolve().get_value();
    if (ip_resolve == "ipv4") {
        handle.set_opt(LRO_IPRESOLVE, LR_IPRESOLVE_V4);
    } else if (ip_resolve == "ipv6") {
        handle.set_opt(LRO_IPRESOLVE, LR_IPRESOLVE_V6);
    }

    auto userpwd = config.username().get_value();
    if (!userpwd.empty()) {
        // TODO Use URL encoded form, needs support in librepo
        userpwd = format_user_pass_string(userpwd, config.password().get_value(), false);
        handle.set_opt(LRO_USERPWD, userpwd.c_str());
    }

    if (!config.sslcacert().get_value().empty()) {
        handle.set_opt(LRO_SSLCACERT, config.sslcacert().get_value().c_str());
    }
    if (!config.sslclientcert().get_value().empty()) {
        handle.set_opt(LRO_SSLCLIENTCERT, config.sslclientcert().get_value().c_str());
    }
    if (!config.sslclientkey().get_value().empty()) {
        handle.set_opt(LRO_SSLCLIENTKEY, config.sslclientkey().get_value().c_str());
    }
    long sslverify = config.sslverify().get_value() ? 1L : 0L;
    handle.set_opt(LRO_SSLVERIFYHOST, sslverify);
    handle.set_opt(LRO_SSLVERIFYPEER, sslverify);

    // === proxy setup ===
    if (!config.proxy().empty() && !config.proxy().get_value().empty()) {
        handle.set_opt(LRO_PROXY, config.proxy().get_value().c_str());
    }

    const std::string proxy_auth_method_str = config.proxy_auth_method().get_value();
    auto proxy_auth_method = LR_AUTH_ANY;
    for (auto & auth : PROXYAUTHMETHODS) {
        if (proxy_auth_method_str == auth.name) {
            proxy_auth_method = auth.code;
            break;
        }
    }
    handle.set_opt(LRO_PROXYAUTHMETHODS, static_cast<long>(proxy_auth_method));

    if (!config.proxy_username().empty()) {
        auto userpwd = config.proxy_username().get_value();
        if (!userpwd.empty()) {
            userpwd = format_user_pass_string(userpwd, config.proxy_password().get_value(), true);
            handle.set_opt(LRO_PROXYUSERPWD, userpwd.c_str());
        }
    }

    if (!config.proxy_sslcacert().get_value().empty()) {
        handle.set_opt(LRO_PROXY_SSLCACERT, config.proxy_sslcacert().get_value().c_str());
    }
    if (!config.proxy_sslclientcert().get_value().empty()) {
        handle.set_opt(LRO_PROXY_SSLCLIENTCERT, config.proxy_sslclientcert().get_value().c_str());
    }
    if (!config.proxy_sslclientkey().get_value().empty()) {
        handle.set_opt(LRO_PROXY_SSLCLIENTKEY, config.proxy_sslclientkey().get_value().c_str());
    }
    long proxy_sslverify = config.proxy_sslverify().get_value() ? 1L : 0L;
    handle.set_opt(LRO_PROXY_SSLVERIFYHOST, proxy_sslverify);
    handle.set_opt(LRO_PROXY_SSLVERIFYPEER, proxy_sslverify);
}


void LibrepoHandle::init_remote(const libdnf::ConfigMain & config) {
    repo::init_remote(*this, config);
}

void LibrepoHandle::init_remote(const libdnf::repo::ConfigRepo & config) {
    repo::init_remote(*this, config);
}

LibrepoResult LibrepoHandle::perform() {
    LibrepoResult result;
    GError * err_p{nullptr};
    if (!::lr_handle_perform(handle, result.get(), &err_p)) {
        throw LibrepoError(std::unique_ptr<GError>(err_p));
    }
    return result;
}

}  // namespace libdnf::repo
