#include "auth.h"
#include "logs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace sapisrv {

namespace {

class TokenBucket {
public:
    TokenBucket(double qps, double burst)
        : qps_(qps), capacity_(burst), tokens_(burst), last_(clock_now()) {}

    // Returns true if the request is allowed; false if rate-limited.
    // On false, retry_after_seconds is populated.
    bool try_consume(int& retry_after_seconds) {
        std::lock_guard<std::mutex> lk(m_);
        refill_locked();
        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            retry_after_seconds = 0;
            return true;
        }
        double need = 1.0 - tokens_;
        retry_after_seconds = qps_ > 0
            ? std::max(1, static_cast<int>(std::ceil(need / qps_)))
            : 60;
        return false;
    }

private:
    static auto clock_now() { return std::chrono::steady_clock::now(); }
    void refill_locked() {
        auto t = clock_now();
        double dt = std::chrono::duration<double>(t - last_).count();
        tokens_ = std::min(capacity_, tokens_ + dt * qps_);
        last_ = t;
    }
    double qps_, capacity_, tokens_;
    std::chrono::steady_clock::time_point last_;
    std::mutex m_;
};

struct KeyConfig {
    std::string name;
    double qps;
    double burst;
};

struct PublicConfig {
    double qps = 0.5;
    double burst = 3.0;
};

// Process-wide auth state.
PublicConfig g_public;
std::unordered_map<std::string, KeyConfig> g_keys;
std::unordered_map<std::string, std::unique_ptr<TokenBucket>> g_key_buckets;
std::unordered_map<std::string, std::unique_ptr<TokenBucket>> g_ip_buckets;
std::mutex g_buckets_mu;

TokenBucket& key_bucket(const std::string& key, const KeyConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_buckets_mu);
    auto& b = g_key_buckets[key];
    if (!b) b = std::make_unique<TokenBucket>(cfg.qps, cfg.burst);
    return *b;
}

TokenBucket& ip_bucket(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_buckets_mu);
    auto& b = g_ip_buckets[ip];
    if (!b) b = std::make_unique<TokenBucket>(g_public.qps, g_public.burst);
    return *b;
}

std::string deny_body(const char* reason) {
    return std::string("{\"error\":\"") + reason + "\"}";
}

}  // namespace

void load_auth_config(const std::wstring& keys_json_path) {
    g_keys.clear();
    g_key_buckets.clear();
    g_public = {};

    std::ifstream f(keys_json_path);
    if (!f) {
        log::info("auth: %ls not present, using public-only defaults (qps=%g burst=%g)",
                  keys_json_path.c_str(), g_public.qps, g_public.burst);
        return;
    }
    nlohmann::json j;
    f >> j;

    if (j.contains("public_tier")) {
        const auto& p = j["public_tier"];
        if (p.contains("qps"))   g_public.qps   = p["qps"].get<double>();
        if (p.contains("burst")) g_public.burst = p["burst"].get<double>();
    }
    if (j.contains("keys")) {
        for (auto it = j["keys"].begin(); it != j["keys"].end(); ++it) {
            KeyConfig kc;
            kc.name  = it->value("name", std::string{});
            kc.qps   = it->value("qps", 1.0);
            kc.burst = it->value("burst", 5.0);
            g_keys[it.key()] = kc;
        }
    }
    log::info("auth: loaded %zu key(s); public qps=%g burst=%g",
              g_keys.size(), g_public.qps, g_public.burst);
}

AuthDecision check_auth(const AuthRequest& req) {
    AuthDecision d;
    if (!req.api_key.empty()) {
        auto it = g_keys.find(req.api_key);
        if (it == g_keys.end()) {
            d.allowed = false;
            d.status = 401;
            d.status_text = "Unauthorized";
            d.body = deny_body("invalid api key");
            return d;
        }
        int retry = 0;
        if (!key_bucket(req.api_key, it->second).try_consume(retry)) {
            d.allowed = false;
            d.status = 429;
            d.status_text = "Too Many Requests";
            d.body = deny_body("rate limit exceeded for this api key");
            d.retry_after_seconds = retry;
            return d;
        }
        return d;
    }

    // Unauthenticated: per-IP bucket from public_tier.
    std::string ip = req.source_ip.empty() ? std::string("unknown") : req.source_ip;
    int retry = 0;
    if (!ip_bucket(ip).try_consume(retry)) {
        d.allowed = false;
        d.status = 429;
        d.status_text = "Too Many Requests";
        d.body = deny_body("public-tier rate limit exceeded; supply an api key for higher quota");
        d.retry_after_seconds = retry;
        return d;
    }
    return d;
}

}  // namespace sapisrv
