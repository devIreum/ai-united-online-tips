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
    s.add_employee("emp-2","venue-1","Ivan","+79990002233");
    s.add_employee("emp-3","venue-1","Oleg","+79990003344");
    auto qr=s.issue_qr("MANAGER","venue-1","emp-1");
    std::cout<<"AI United | Online tips | C++ demo\nQR issued and signed\n\n";
    auto scenario=[&](int n,const std::string& title,const std::string& expected,
                      std::string pan,std::int64_t kop,std::string key, bool to_pool=false) {
        TipRequest req{qr,kop,pan,key,"",false,to_pool};
        std::string result;
        try {auto p=s.pay("GUEST",req);result=p.status+(p.replayed?" (replay)":"")+(to_pool?" [pool]":"");}
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
    ok &= scenario(6,"Payment to common pool","SUCCEEDED [pool]","4242424242424242",100000,uuid4(),true);
    auto balance=s.pool_balance("MANAGER","venue-1");
    std::cout<<"Pool balance before distribution: "<<rub(balance.pending_kop)<<"\n";
    auto dist=s.distribute_pool("MANAGER","venue-1",{{"emp-1",8},{"emp-2",6},{"emp-3",4}});
    std::cout<<"Pool distribution "<<rub(dist.total_kop)<<": Anna="<<rub(dist.shares["emp-1"])
             <<", Ivan="<<rub(dist.shares["emp-2"])<<", Oleg="<<rub(dist.shares["emp-3"])<<"\n";
    std::cout<<"Gateway calls: "<<s.gateway_calls()<<" for 7 requests\n";
    std::cout<<"Audit valid: "<<(s.verify_audit()?"yes":"no")<<"\n";
    auto shares=split_pool(100000,{{"Anna",8},{"Ivan",6},{"Oleg",4}});
    std::cout<<"Standalone pool 1000 RUB: Anna="<<rub(shares["Anna"])<<", Ivan="<<rub(shares["Ivan"])
             <<", Oleg="<<rub(shares["Oleg"])<<"\n";
    return ok?0:1;
}
