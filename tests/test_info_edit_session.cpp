#include "InfoEditSession.h"
#include <iostream>
int main() {
    int failed=0;
    auto check=[&](bool ok,const char* label) { if(!ok) { ++failed; std::cerr<<"FAIL: "<<label<<"\n"; } };
    InfoEditSession edit;
    InfoEditValues original{"old description","old keyword","old category"};
    InfoEditValues changed{"new description","old keyword","old category"};
    edit.Begin(original);
    check(!edit.RequestLeave(original) && !edit.active,"unchanged leaves directly");
    for (const char* reason : {"file","page","hide","database","exit"}) {
        edit.Begin(original);
        check(edit.RequestLeave(changed) && edit.active && edit.awaitingChoice,reason);
        check(!edit.Resolve(true,false) && edit.active && edit.awaitingChoice,"failed save keeps draft and prompt");
        check(edit.Resolve(true,true) && !edit.active && !edit.awaitingChoice,"successful save permits departure");
        edit.Begin(original); edit.RequestLeave(changed);
        check(edit.Resolve(false,false) && !edit.active,"no discards without a save");
    }
    edit.Begin(original);
    check(edit.Dirty(changed) && !edit.Dirty(original),"reverted changes do not prompt");
    std::cout<<failed<<" failures\n";
    return failed?1:0;
}
