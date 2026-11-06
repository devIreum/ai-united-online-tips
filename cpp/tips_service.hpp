#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
struct sqlite3;

namespace tips {
struct TipError : std::runtime_error {
    std::string code;
    int http;
    TipError(std::string code, std::string message, int http = 400)
        : std::runtime_error(std::move(message)), code(std::move(code)), http(http) {}
};
struct TipRequest {
    std::string qr_url;
    std::int64_t amount_kop = 30000;
    std::string pan; // Only test input to gateway emulator. Never persisted.
    std::string idempotency_key;
    std::string comment;
    bool cover_fee = false;
};
struct Payment {
    std::string id;
    std::string status;
    std::int64_t amount_kop = 0;
    std::int64_t fee_kop = 0;
    std::int64_t net_kop = 0;
    std::string pan_masked;
    std::string employee_id;
    bool replayed = false;
    std::string message;
};
struct Fee { std::int64_t charge, fee, net; };
Fee calc_fee(std::int64_t amount_kop, bool cover_fee);
std::map<std::string, std::int64_t> split_pool(std::int64_t total_kop, const std::map<std::string, int>& hours);
bool luhn_ok(const std::string& pan);
std::string mask_pan(const std::string& pan);
std::string uuid4();

class TipService {
public:
    TipService(std::string qr_key = "demo-qr-key", std::string salt = "demo-salt");
    ~TipService();
    TipService(const TipService&) = delete;
    TipService& operator=(const TipService&) = delete;
    void add_employee(const std::string& emp_id, const std::string& venue_id,
                      const std::string& name, const std::string& phone);
    std::string issue_qr(const std::string& role, const std::string& venue, const std::string& emp);
    Payment pay(const std::string& role, const TipRequest& req);
    std::vector<Payment> reconcile();
    std::vector<Payment> waiter_tips(const std::string& role, const std::string& emp);
    void set_status_for_test(const std::string& pid, const std::string& status);
    bool verify_audit() const;
    void corrupt_audit_for_test();
    std::string database_dump_for_test() const;
    int gateway_calls() const { return gateway_calls_; }
private:
    sqlite3* db_ = nullptr;
    std::string qr_key_, salt_;
    int gateway_calls_ = 0;
    std::map<std::string, std::string> gateway_status_;
    static void require(const std::string& role, const std::string& permission);
    std::string qr_signature(const std::string& payload) const;
    std::string verify_qr(const std::string& url, std::string& venue) const;
    void audit(const std::string& actor, const std::string& action,
               const std::string& object_id, const std::string& details);
    void set_status(const std::string& pid, const std::string& status,
                    const std::string& gateway_ref = "");
    Payment load(const std::string& pid) const;
};
} // namespace tips
