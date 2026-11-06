#include "tips_service.hpp"
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
using namespace tips;
namespace {
void check(bool b,const std::string& msg){if(!b)throw std::runtime_error(msg);}
template<class F> void throws_code(F callback,const std::string& code){
    try {callback();} catch(const TipError& e){check(e.code==code,"Wrong error: "+e.code);return;}
    throw std::runtime_error("Expected error: "+code);
}
struct Setup {
    TipService s;std::string qr;
    Setup(){s.add_employee("emp-1","venue-1","Anna","+79990001122");qr=s.issue_qr("MANAGER","venue-1","emp-1");}
    TipRequest request(std::string pan="4242424242424242",std::int64_t amount=30000,std::string key=uuid4()){
        return {qr,amount,pan,key,"",false};
    }
};
}
int main(){
    int passed=0;int failed=0;
    auto test=[&](const std::string& name,const std::function<void()>& f){
        try{f();++passed;std::cout<<"[PASS] "<<name<<"\n";}
        catch(const std::exception& e){++failed;std::cout<<"[FAIL] "<<name<<" - "<<e.what()<<"\n";}
    };
    test("1. Successful payment",[]{Setup x;auto p=x.s.pay("GUEST",x.request());
        check(p.status=="SUCCEEDED"&&p.fee_kop==1500&&p.net_kop==28500,"Wrong payment");
        check(p.pan_masked=="4242 **** **** 4242","PAN not masked");});
    test("2. Bank decline",[]{Setup x;auto p=x.s.pay("GUEST",x.request("4000000000000002"));
        check(p.status=="DECLINED","Not declined");check(x.s.waiter_tips("WAITER","emp-1").empty(),"Declined payment credited");});
    test("3. Invalid input",[]{Setup x;
        throws_code([&]{x.s.pay("GUEST",x.request("4242424242424242",1000));},"AMOUNT_OUT_OF_RANGE");
        throws_code([&]{x.s.pay("GUEST",x.request("4242424242424241"));},"CARD_INVALID");
        auto req=x.request();auto pos=req.qr_url.find("emp=emp-1");check(pos!=std::string::npos,"Invalid test QR");req.qr_url.replace(pos,9,"emp=emp-2");
        throws_code([&]{x.s.pay("GUEST",req);},"QR_BAD_SIGNATURE");
        throws_code([&]{x.s.pay("GUEST",x.request("4242424242424242",30000,"123"));},"IDEM_KEY_INVALID");
        check(x.s.gateway_calls()==0,"Invalid input reached gateway");});
    test("4. Idempotency and key conflict",[]{Setup x;auto req=x.request();
        auto a=x.s.pay("GUEST",req),b=x.s.pay("GUEST",req);
        check(a.id==b.id&&b.replayed&&x.s.gateway_calls()==1,"Double charge");
        req.amount_kop=50000;throws_code([&]{x.s.pay("GUEST",req);},"IDEM_CONFLICT");});
    test("5. Timeout and reconciliation",[]{Setup x;auto p=x.s.pay("GUEST",x.request("4000000000000119"));
        check(p.status=="PENDING_UNKNOWN","Not pending");
        auto fixed=x.s.reconcile();check(fixed.size()==1&&fixed[0].status=="SUCCEEDED","Failed reconciliation");});
    test("6. RBAC",[]{Setup x;throws_code([&]{x.s.issue_qr("WAITER","venue-1","emp-1");},"FORBIDDEN");});
    test("7. Forbidden transition",[]{Setup x;auto p=x.s.pay("GUEST",x.request("4000000000000002"));
        throws_code([&]{x.s.set_status_for_test(p.id,"SUCCEEDED");},"BAD_TRANSITION");});
    test("8. Guest covers fee",[]{auto f=calc_fee(30000,true);check(f.charge==31500&&f.fee==1500&&f.net==30000,"Fee incorrect");});
    test("9. Pool preserves kopecks",[]{auto s=split_pool(100001,{{"Anna",8},{"Ivan",6},{"Oleg",4}});
        std::int64_t sum=0;for(const auto& [id,v]:s)sum+=v;check(sum==100001,"Money lost");});
    test("10. Audit detects tampering",[]{Setup x;x.s.pay("GUEST",x.request());
        check(x.s.verify_audit(),"Audit invalid before modification");x.s.corrupt_audit_for_test();
        check(!x.s.verify_audit(),"Tampering undetected");});
    test("11. PAN and phone are not persisted",[]{Setup x;x.s.pay("GUEST",x.request());
        auto dump=x.s.database_dump_for_test();
        check(dump.find("4242424242424242")==std::string::npos,"PAN in SQLite");
        check(dump.find("+79990001122")==std::string::npos,"Phone in SQLite");});
    std::cout<<"RESULT: "<<passed<<" passed, "<<failed<<" failed\n";
    return failed==0?0:1;
}
