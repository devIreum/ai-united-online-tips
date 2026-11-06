#include "tips_service.hpp"
#include <iostream>
#include <string>
using namespace tips;
namespace {
std::string rub(std::int64_t kop) {return std::to_string(kop/100)+"."+(kop%100<10?"0":"")+std::to_string(kop%100)+" RUB";}
}
int main() {
    TipService s;
    s.add_employee("emp-1","venue-1","Anna","+79990001122");
    auto qr=s.issue_qr("MANAGER","venue-1","emp-1");
    std::cout<<"AI United | Online tips | C++ demo\nQR issued and signed\n\n";
    auto scenario=[&](int n,const std::string& title,const std::string& expected,
                      std::string pan,std::int64_t kop,std::string key) {
        TipRequest req{qr,kop,pan,key,"",false};
        std::string result;
        try {auto p=s.pay("GUEST",req);result=p.status+(p.replayed?" (replay)":"");}
        catch(const TipError& e) {result=e.code;}
        bool pass=result.find(expected)==0;
        std::cout<<(pass?"[PASS] ":"[FAIL] ")<<n<<". "<<title<<" | expected="<<expected<<" | actual="<<result<<"\n";
        return pass;
    };
    bool ok=true;
    ok &= scenario(1,"Success","SUCCEEDED","4242424242424242",30000,uuid4());
    ok &= scenario(2,"Bank decline","DECLINED","4000000000000002",30000,uuid4());
    ok &= scenario(3,"Invalid amount","AMOUNT_OUT_OF_RANGE","4242424242424242",1000,uuid4());
    auto key=uuid4();
    ok &= scenario(4,"First click","SUCCEEDED","4242424242424242",30000,key);
    ok &= scenario(4,"Duplicate click","SUCCEEDED (replay)","4242424242424242",30000,key);
    ok &= scenario(5,"Gateway timeout","PENDING_UNKNOWN","4000000000000119",30000,uuid4());
    auto matched=s.reconcile();
    std::cout<<"Reconciliation: "<<(matched.empty()?"none":matched.front().status)<<"\n";
    std::cout<<"Gateway calls: "<<s.gateway_calls()<<" for 6 requests\n";
    std::cout<<"Audit valid: "<<(s.verify_audit()?"yes":"no")<<"\n";
    auto shares=split_pool(100000,{{"Anna",8},{"Ivan",6},{"Oleg",4}});
    std::cout<<"Pool 1000 RUB: Anna="<<rub(shares["Anna"])<<", Ivan="<<rub(shares["Ivan"])
             <<", Oleg="<<rub(shares["Oleg"])<<"\n";
    return ok?0:1;
}
