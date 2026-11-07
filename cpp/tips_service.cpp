#include "tips_service.hpp"
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <tuple>
#include <utility>

namespace tips {
namespace {
std::string hex(const unsigned char* data, size_t n) {
    static const char* tab = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out += tab[data[i] >> 4]; out += tab[data[i] & 15]; }
    return out;
}
std::string sha256(const std::string& s) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);
    return hex(digest, sizeof(digest));
}
std::string hmac256(const std::string& key, const std::string& body) {
    unsigned char out[EVP_MAX_MD_SIZE]; unsigned int n = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(body.data()), body.size(), out, &n))
        throw std::runtime_error("HMAC failed");
    return hex(out, n);
}
std::int64_t now_sec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}
void run(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "SQLite error";
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Stmt() { if(s) sqlite3_finalize(s); }
    void bind(int index, const std::string& v) {
        if (sqlite3_bind_text(s,index,v.c_str(),-1,SQLITE_TRANSIENT)!=SQLITE_OK)
            throw std::runtime_error("SQLite bind failed");
    }
    void bind(int index, std::int64_t v) { sqlite3_bind_int64(s,index,v); }
    void execute() { if(sqlite3_step(s)!=SQLITE_DONE) throw std::runtime_error("SQLite write failed"); }
    bool next() { return sqlite3_step(s)==SQLITE_ROW; }
    std::string str(int i) const {
        const unsigned char* v = sqlite3_column_text(s,i);
        return v ? reinterpret_cast<const char*>(v) : "";
    }
    std::int64_t num(int i) const { return sqlite3_column_int64(s,i); }
};
std::map<std::string, std::string> parse_qs(const std::string& url) {
    size_t q=url.find('?');
    if(q==std::string::npos || url.substr(0,q)!="https://tips.example/t")
        throw TipError("QR_MALFORMED","QR URL is invalid");
    std::map<std::string,std::string> params;
    std::istringstream ss(url.substr(q+1));
    std::string part;
    while (std::getline(ss,part,'&')) {
        auto eq=part.find('=');
        if(eq==std::string::npos || params.count(part.substr(0,eq)))
            throw TipError("QR_MALFORMED","Invalid QR parameters");
        params[part.substr(0,eq)]=part.substr(eq+1);
    }
    return params;
}
const std::map<std::string,std::vector<std::string>> role_rights{
    {"GUEST", {"tip.pay"}}, {"WAITER", {"tip.view_own"}},
    {"MANAGER", {"qr.issue", "tip.view_venue", "payout.distribute"}},
    {"ADMIN", {"qr.issue", "venue.manage", "key.rotate"}},
    {"AUDITOR", {"audit.read"}}
};
const std::map<std::string,std::vector<std::string>> transitions{
    {"CREATED", {"PENDING"}},
    {"PENDING", {"SUCCEEDED","DECLINED","PENDING_UNKNOWN"}},
    {"PENDING_UNKNOWN", {"SUCCEEDED","DECLINED"}},
    {"SUCCEEDED", {"REFUNDED"}}, {"DECLINED", {}}, {"REFUNDED", {}}
};
constexpr const char* POOL_EMPLOYEE = "__POOL__";
} // namespace

std::string uuid4() {
    unsigned char b[16];
    if(RAND_bytes(b,16)!=1) throw std::runtime_error("Random generator failed");
    b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;
    auto s=hex(b,16);
    return s.substr(0,8)+"-"+s.substr(8,4)+"-"+s.substr(12,4)+"-"+s.substr(16,4)+"-"+s.substr(20);
}
bool luhn_ok(const std::string& pan) {
    if(pan.size()<13 || pan.size()>19) return false;
    int sum=0; bool twice=false;
    for(auto it=pan.rbegin();it!=pan.rend();++it) {
        if(*it<'0'||*it>'9') return false;
        int n=*it-'0';
        if(twice) { n*=2; if(n>9) n-=9; }
        sum+=n; twice=!twice;
    }
    return sum%10==0;
}
std::string mask_pan(const std::string& pan) {
    return pan.substr(0,4)+" **** **** "+pan.substr(pan.size()-4);
}
Fee calc_fee(std::int64_t amount, bool cover_fee) {
    if(amount<0 || amount>std::numeric_limits<std::int64_t>::max()/2)
        throw TipError("AMOUNT_OUT_OF_RANGE","Invalid amount");
    std::int64_t fee=amount*5/100;
    return cover_fee ? Fee{amount+fee,fee,amount} : Fee{amount,fee,amount-fee};
}
std::map<std::string,std::int64_t> split_pool(std::int64_t total,
                                               const std::map<std::string,int>& hours) {
    if(total<0) throw TipError("POOL_INVALID","Pool cannot be negative");
    std::int64_t sum=0;
    for(const auto& [id,h]:hours) { if(h<0) throw TipError("POOL_INVALID","Hours cannot be negative"); sum+=h; }
    if(sum<=0) throw TipError("POOL_EMPTY","No shifts");
    std::map<std::string,std::int64_t> out;
    std::int64_t assigned=0;
    for(const auto& [id,h]:hours) {
        auto share=static_cast<std::int64_t>((static_cast<__int128>(total)*h)/sum);
        out[id]=share;assigned+=share;
    }
    std::vector<std::pair<std::string,int>> ordered(hours.begin(),hours.end());
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){
        if(a.second!=b.second) return a.second>b.second;
        return a.first<b.first;
    });
    for(std::int64_t i=0;i<total-assigned;++i) ++out[ordered.at(static_cast<size_t>(i%ordered.size())).first];
    return out;
}
TipService::TipService(std::string qr_key, std::string salt)
    : qr_key_(std::move(qr_key)), salt_(std::move(salt)) {
    if(sqlite3_open(":memory:",&db_)!=SQLITE_OK) throw std::runtime_error("Cannot open database");
    run(db_,R"SQL(
CREATE TABLE employee (id TEXT PRIMARY KEY, venue_id TEXT NOT NULL, name TEXT NOT NULL, phone_hash TEXT NOT NULL, active INTEGER NOT NULL);
CREATE TABLE payment (id TEXT PRIMARY KEY, idem_key TEXT NOT NULL UNIQUE, req_hash TEXT NOT NULL,
  venue_id TEXT NOT NULL, employee_id TEXT NOT NULL, amount_kop INTEGER NOT NULL,
  fee_kop INTEGER NOT NULL, net_kop INTEGER NOT NULL, pan_masked TEXT NOT NULL,
  status TEXT NOT NULL, gateway_ref TEXT, created_at INTEGER NOT NULL);
CREATE TABLE audit (seq INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, actor TEXT NOT NULL,
  action TEXT NOT NULL, object_id TEXT NOT NULL, details TEXT NOT NULL, prev_hash TEXT NOT NULL, hash TEXT NOT NULL);
CREATE TABLE pool_distribution (id TEXT PRIMARY KEY, venue_id TEXT NOT NULL, total_kop INTEGER NOT NULL, created_at INTEGER NOT NULL);
CREATE TABLE pool_distribution_item (distribution_id TEXT NOT NULL, employee_id TEXT NOT NULL, share_kop INTEGER NOT NULL,
  PRIMARY KEY (distribution_id, employee_id));
)SQL");
}
TipService::~TipService(){ if(db_) sqlite3_close(db_); }
void TipService::require(const std::string& role,const std::string& p) {
    auto it=role_rights.find(role);
    if(it==role_rights.end()||std::find(it->second.begin(),it->second.end(),p)==it->second.end())
        throw TipError("FORBIDDEN","Insufficient permissions",403);
}
void TipService::add_employee(const std::string& emp_id,const std::string& venue_id,
                              const std::string& name,const std::string& phone) {
    Stmt s(db_,"INSERT INTO employee VALUES(?,?,?,?,1)");
    s.bind(1,emp_id);s.bind(2,venue_id);s.bind(3,name);s.bind(4,sha256(salt_+phone));s.execute();
}
std::string TipService::qr_signature(const std::string& payload) const { return hmac256(qr_key_,payload); }
std::string TipService::issue_qr(const std::string& role,const std::string& venue,const std::string& emp) {
    require(role,"qr.issue");
    auto exp=std::to_string(now_sec()+30LL*24*3600);
    std::string payload="venue="+venue+"&emp="+emp+"&exp="+exp;
    auto url="https://tips.example/t?"+payload+"&sig="+qr_signature(payload);
    audit(role,"qr.issue",emp,"venue="+venue);
    return url;
}
std::string TipService::verify_qr(const std::string& url,std::string& venue) const {
    auto params=parse_qs(url);
    for(auto key: {"venue","emp","exp","sig"}) if(!params.count(key)) throw TipError("QR_MALFORMED","Missing QR field");
    const auto payload="venue="+params.at("venue")+"&emp="+params.at("emp")+"&exp="+params.at("exp");
    auto expected=qr_signature(payload);
    if(params.at("sig").size()!=expected.size() || CRYPTO_memcmp(params.at("sig").data(),expected.data(),expected.size())!=0)
        throw TipError("QR_BAD_SIGNATURE","QR signature mismatch",403);
    if(std::stoll(params.at("exp"))<now_sec()) throw TipError("QR_EXPIRED","QR expired",410);
    venue=params.at("venue");
    return params.at("emp");
}
void TipService::audit(const std::string& actor,const std::string& action,
                       const std::string& object_id,const std::string& details) {
    std::string prev;
    { Stmt q(db_,"SELECT hash FROM audit ORDER BY seq DESC LIMIT 1"); if(q.next()) prev=q.str(0); }
    auto ts=now_sec();
    auto h=sha256(prev+"|"+std::to_string(ts)+"|"+actor+"|"+action+"|"+object_id+"|"+details);
    Stmt i(db_,"INSERT INTO audit(ts,actor,action,object_id,details,prev_hash,hash) VALUES(?,?,?,?,?,?,?)");
    i.bind(1,ts);i.bind(2,actor);i.bind(3,action);i.bind(4,object_id);i.bind(5,details);i.bind(6,prev);i.bind(7,h);i.execute();
}
bool TipService::verify_audit() const {
    Stmt q(db_,"SELECT ts,actor,action,object_id,details,prev_hash,hash FROM audit ORDER BY seq");
    std::string prev;
    while(q.next()) {
        auto calc=sha256(q.str(5)+"|"+q.str(0)+"|"+q.str(1)+"|"+q.str(2)+"|"+q.str(3)+"|"+q.str(4));
        if(q.str(5)!=prev || q.str(6)!=calc) return false;
        prev=q.str(6);
    }
    return true;
}
void TipService::set_status(const std::string& pid,const std::string& status,const std::string& gateway_ref) {
    Stmt q(db_,"SELECT status FROM payment WHERE id=?");q.bind(1,pid);
    if(!q.next()) throw std::runtime_error("Payment not found");
    const auto current=q.str(0);
    auto it=transitions.find(current);
    if(it==transitions.end()||std::find(it->second.begin(),it->second.end(),status)==it->second.end())
        throw TipError("BAD_TRANSITION","Transition is forbidden",409);
    Stmt u(db_,"UPDATE payment SET status=?, gateway_ref=? WHERE id=?");
    u.bind(1,status);u.bind(2,gateway_ref);u.bind(3,pid);u.execute();
    audit("system","payment.status",pid,current+"->"+status);
}
Payment TipService::load(const std::string& pid) const {
    Stmt q(db_,"SELECT id,status,amount_kop,fee_kop,net_kop,pan_masked,employee_id FROM payment WHERE id=?");q.bind(1,pid);
    if(!q.next()) throw std::runtime_error("Payment not found");
    Payment p; p.id=q.str(0); p.status=q.str(1); p.amount_kop=q.num(2); p.fee_kop=q.num(3); p.net_kop=q.num(4); p.pan_masked=q.str(5); p.employee_id=q.str(6); return p;
}
Payment TipService::pay(const std::string& role,const TipRequest& req) {
    require(role,"tip.pay");
    if(!std::regex_match(req.idempotency_key,std::regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")))
        throw TipError("IDEM_KEY_INVALID","Expected UUID");
    auto fingerprint=sha256(req.qr_url+"|"+std::to_string(req.amount_kop)+"|"+req.pan+"|"+
                            (req.cover_fee?"1":"0")+"|"+req.comment+"|"+(req.to_pool?"1":"0"));
    Stmt prev(db_,"SELECT id,req_hash FROM payment WHERE idem_key=?");prev.bind(1,req.idempotency_key);
    if(prev.next()) {
        if(prev.str(1)!=fingerprint) throw TipError("IDEM_CONFLICT","Key already used with different data",409);
        const auto id=prev.str(0);audit(role,"payment.replay",id,"same request");
        auto result=load(id);result.replayed=true;return result;
    }
    std::string venue;
    auto emp=verify_qr(req.qr_url,venue);
    if(req.amount_kop<5000 || req.amount_kop>1000000)
        throw TipError("AMOUNT_OUT_OF_RANGE","Tip must be 50 to 10000 RUB");
    if(!luhn_ok(req.pan)) throw TipError("CARD_INVALID","Card failed checksum");
    if(req.comment.size()>800) throw TipError("COMMENT_TOO_LONG","Comment is too long");
    Stmt employee(db_,"SELECT active FROM employee WHERE id=? AND venue_id=?");
    employee.bind(1,emp);employee.bind(2,venue);
    if(!employee.next()||employee.num(0)!=1) throw TipError("EMPLOYEE_NOT_FOUND","Employee not found",404);
    auto [charge,fee,net]=calc_fee(req.amount_kop,req.cover_fee);
    auto target_emp = req.to_pool ? std::string(POOL_EMPLOYEE) : emp;
    auto pid=uuid4();
    Stmt created(db_,"INSERT INTO payment VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    created.bind(1,pid);created.bind(2,req.idempotency_key);created.bind(3,fingerprint);
    created.bind(4,venue);created.bind(5,target_emp);created.bind(6,charge);created.bind(7,fee);
    created.bind(8,net);created.bind(9,mask_pan(req.pan));created.bind(10,"CREATED");
    created.bind(11,"");created.bind(12,now_sec());created.execute();
    audit(role,"payment.create",pid,(req.to_pool?"pool":"direct")+std::string(" charge=")+std::to_string(charge));
    set_status(pid,"PENDING");
    ++gateway_calls_;
    auto ref="gw_"+pid.substr(0,8);
    if(req.pan=="4000000000000002") {
        gateway_status_[pid]="declined";set_status(pid,"DECLINED",ref);
    } else if(req.pan=="4000000000000119") {
        gateway_status_[pid]="succeeded";set_status(pid,"PENDING_UNKNOWN",ref);
        auto p=load(pid);p.message="Payment is being verified; do not retry";return p;
    } else {
        gateway_status_[pid]="succeeded";set_status(pid,"SUCCEEDED",ref);
    }
    return load(pid);
}
std::vector<Payment> TipService::reconcile() {
    std::vector<std::string> ids;
    Stmt q(db_,"SELECT id FROM payment WHERE status='PENDING_UNKNOWN'");
    while(q.next()) ids.push_back(q.str(0));
    std::vector<Payment> out;
    for(auto& id:ids) {
        auto it=gateway_status_.find(id);
        if(it==gateway_status_.end()) continue;
        auto target=it->second=="succeeded"?"SUCCEEDED":"DECLINED";
        set_status(id,target,"gw_"+id.substr(0,8));out.push_back(load(id));
    }
    return out;
}
std::vector<Payment> TipService::waiter_tips(const std::string& role,const std::string& emp) {
    require(role,"tip.view_own");
    Stmt s(db_,"SELECT id FROM payment WHERE employee_id=? AND status='SUCCEEDED'");s.bind(1,emp);
    std::vector<Payment> out;while(s.next())out.push_back(load(s.str(0)));return out;
}
PoolBalance TipService::pool_balance(const std::string& role, const std::string& venue) {
    require(role, "payout.distribute");
    PoolBalance out; out.venue_id = venue;
    Stmt q1(db_,"SELECT COALESCE(SUM(net_kop),0) FROM payment WHERE venue_id=? AND employee_id=? AND status='SUCCEEDED'");
    q1.bind(1, venue); q1.bind(2, POOL_EMPLOYEE);
    if(q1.next()) out.pending_kop = q1.num(0);
    Stmt q2(db_,"SELECT COALESCE(SUM(total_kop),0) FROM pool_distribution WHERE venue_id=?");
    q2.bind(1, venue);
    if(q2.next()) out.distributed_kop = q2.num(0);
    return out;
}
PoolDistribution TipService::distribute_pool(const std::string& role, const std::string& venue,
                                             const std::map<std::string, int>& hours) {
    require(role, "payout.distribute");
    if(hours.empty()) throw TipError("POOL_EMPTY", "No shifts");
    Stmt q(db_,"SELECT COALESCE(SUM(net_kop),0) FROM payment WHERE venue_id=? AND employee_id=? AND status='SUCCEEDED'");
    q.bind(1, venue); q.bind(2, POOL_EMPLOYEE);
    std::int64_t total = 0;
    if(q.next()) total = q.num(0);
    Stmt spent(db_,"SELECT COALESCE(SUM(total_kop),0) FROM pool_distribution WHERE venue_id=?");
    spent.bind(1, venue);
    std::int64_t distributed = 0;
    if(spent.next()) distributed = spent.num(0);
    auto available = total - distributed;
    if(available <= 0) throw TipError("POOL_EMPTY", "No undistributed tips in the pool");
    for(const auto& [emp, h] : hours) {
        Stmt e(db_,"SELECT active FROM employee WHERE id=? AND venue_id=?");
        e.bind(1, emp); e.bind(2, venue);
        if(!e.next() || e.num(0)!=1) throw TipError("EMPLOYEE_NOT_FOUND", "Employee not found in venue", 404);
        if(h < 0) throw TipError("POOL_INVALID", "Hours cannot be negative");
    }
    auto shares = split_pool(available, hours);
    auto distribution_id = uuid4();
    Stmt ins(db_,"INSERT INTO pool_distribution VALUES(?,?,?,?)");
    ins.bind(1, distribution_id); ins.bind(2, venue); ins.bind(3, available); ins.bind(4, now_sec()); ins.execute();
    for(const auto& [emp, share] : shares) {
        Stmt item(db_,"INSERT INTO pool_distribution_item VALUES(?,?,?)");
        item.bind(1, distribution_id); item.bind(2, emp); item.bind(3, share); item.execute();
    }
    std::ostringstream details;
    details << "venue=" << venue << "; total=" << available << "; members=" << shares.size();
    audit(role, "pool.distribute", distribution_id, details.str());
    PoolDistribution out; out.venue_id = venue; out.distribution_id = distribution_id; out.total_kop = available; out.shares = std::move(shares);
    return out;
}
void TipService::set_status_for_test(const std::string& pid,const std::string& s){set_status(pid,s);} 
void TipService::corrupt_audit_for_test(){run(db_,"UPDATE audit SET details='modified' WHERE seq=2");}
std::string TipService::database_dump_for_test() const {
    sqlite3_int64 size=0;
    unsigned char* bytes=sqlite3_serialize(db_,"main",&size,0);
    if(!bytes)throw std::runtime_error("Serialize failed");
    std::string dump(reinterpret_cast<const char*>(bytes),static_cast<size_t>(size));
    sqlite3_free(bytes);return dump;
}
} // namespace tips
