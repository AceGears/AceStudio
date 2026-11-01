// acestudio-licence: AceGears' tool for licence keys. It is not shipped to
// customers. See README.md in this folder.
//
//   acestudio-licence new-keypair <folder>
//   acestudio-licence public --secret <licence-secret.key>
//   acestudio-licence issue --secret <file | dev> --name "Name" [--email E] [--edition standard]
//                           [--expires YYYY-MM-DD] [--id ID] [--log issued.csv]
//   acestudio-licence verify <key or @file> [--public HEX]

#include "acestudio/core/Licence.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

using namespace acestudio;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage:\n"
                 "  acestudio-licence new-keypair <folder>\n"
                 "  acestudio-licence public --secret <licence-secret.key>\n"
                 "  acestudio-licence issue --secret <file> --name NAME [--email E] [--edition standard]\n"
                 "                          [--expires YYYY-MM-DD] [--id ID] [--log issued.csv]\n"
                 "  acestudio-licence verify <key or @file> [--public HEX]\n";
    return 1;
}

bool randomBytes(std::uint8_t* out, std::size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::ifstream f("/dev/urandom", std::ios::binary);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n)));
#endif
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::optional<LicenceSecretKey> readSecret(const std::string& file) {
    std::ifstream f(file);
    std::string hex((std::istreambuf_iterator<char>(f)), {});
    return key32FromHex(hex);
}

std::string csv(const std::string& s) {
    std::string out = "\"";
    for (char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0 && i + 1 < argc) opt[a.substr(2)] = argv[++i];
        else pos.push_back(a);
    }

    if (cmd == "new-keypair") {
        if (pos.size() != 1) return usage();
        const fs::path dir = pos[0];
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path secretFile = dir / "licence-secret.key", publicFile = dir / "licence-public.txt";
        if (fs::exists(secretFile, ec) || fs::exists(publicFile, ec)) {
            std::cerr << "error: " << dir.string() << " already holds a key pair; it is never overwritten\n";
            return 2;
        }
        LicenceSecretKey seed{};
        if (!randomBytes(seed.data(), seed.size())) {
            std::cerr << "error: the system random generator failed\n";
            return 2;
        }
        const LicencePublicKey pub = licencePublicKeyFor(seed);
        {
            std::ofstream s(secretFile);
            s << toHex(seed.data(), seed.size()) << "\n";
        }
        std::ofstream(publicFile) << toHex(pub.data(), pub.size()) << "\n";
        std::fill(seed.begin(), seed.end(), std::uint8_t{0});
        std::cout << "Wrote " << secretFile.string() << " (SECRET: keep it offline and backed up; anyone who has it can make keys)\n"
                  << "Wrote " << publicFile.string() << "\n"
                  << "Public key (set ACESTUDIO_LICENCE_PUBLIC_KEY to this when building releases):\n"
                  << toHex(pub.data(), pub.size()) << "\n";
        return 0;
    }
    if (cmd == "public") {
        auto seed = readSecret(opt["secret"]);
        if (!seed) {
            std::cerr << "error: --secret must be a licence-secret.key file\n";
            return 2;
        }
        const LicencePublicKey pub = licencePublicKeyFor(*seed);
        std::cout << toHex(pub.data(), pub.size()) << "\n";
        return 0;
    }
    if (cmd == "issue") {
        // --secret dev: the public development key (for development builds and tests only).
        auto seed = opt["secret"] == "dev" ? std::optional<LicenceSecretKey>(developmentLicenceSeed()) : readSecret(opt["secret"]);
        if (!seed || opt["name"].empty()) return usage();
        LicenceKey k;
        k.name = opt["name"];
        k.email = opt["email"];
        k.edition = opt.count("edition") ? opt["edition"] : "standard";
        k.expires = opt["expires"];
        k.issued = today();
        if (opt.count("id")) {
            k.id = opt["id"];
        } else {
            std::uint8_t r[6];
            randomBytes(r, sizeof r);
            k.id = "AG-" + toHex(r, sizeof r);
        }
        const std::string key = signLicenceKey(k, *seed);
        // Self-check against the matching public key.
        if (!verifyLicenceKey(key, licencePublicKeyFor(*seed), k.issued)) {
            std::cerr << "error: the new key did not verify\n";
            return 3;
        }
        if (opt.count("log")) {
            const bool fresh = !fs::exists(opt["log"]);
            std::ofstream log(opt["log"], std::ios::app);
            if (fresh) log << "id,issued,name,email,edition,expires\n";
            log << csv(k.id) << "," << k.issued << "," << csv(k.name) << "," << csv(k.email) << "," << csv(k.edition) << "," << k.expires << "\n";
        }
        std::cout << key << "\n";
        return 0;
    }
    if (cmd == "verify") {
        if (pos.size() != 1) return usage();
        std::string text = pos[0];
        if (!text.empty() && text[0] == '@') {
            std::ifstream f(text.substr(1));
            text.assign(std::istreambuf_iterator<char>(f), {});
        }
        LicencePublicKey pub = licencePublicKey();
        if (opt.count("public")) {
            auto p = key32FromHex(opt["public"]);
            if (!p) return usage();
            pub = *p;
        }
        auto k = verifyLicenceKey(text, pub, today());
        if (!k) {
            std::cerr << k.error().message << "\n";
            return 4;
        }
        std::cout << "valid: " << k.value().name << " <" << k.value().email << ">, " << k.value().edition << ", id " << k.value().id
                  << ", issued " << k.value().issued << (k.value().expires.empty() ? "" : ", expires " + k.value().expires) << "\n";
        return 0;
    }
    return usage();
}
