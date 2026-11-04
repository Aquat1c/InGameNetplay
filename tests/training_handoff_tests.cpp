#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
int failures=0, pending=0, drains=0, resumes=0, publications=0;
bool loaded=true, exportsPresent=true, drainSucceeds=true, suspended=false;
int currentStatus=0;
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;} } while(false)
uint32_t __cdecl Prepare(){++pending;return 1;}
void __cdecl Finish(){--pending;}
HMODULE TestGetModuleHandleA(const char*){return loaded?reinterpret_cast<HMODULE>(1):nullptr;}
FARPROC TestGetProcAddress(HMODULE,const char* name){
    if(!exportsPresent)return nullptr;
    return std::strstr(name,"Finish")?reinterpret_cast<FARPROC>(&Finish):reinterpret_cast<FARPROC>(&Prepare);
}
namespace mod {void Log(const char*,...) {}}
namespace state_export {
bool IsSuspendedForOnlineSimulation(){return suspended;}
bool SuspendForOnlineSimulation(){++drains;suspended=true;return drainSucceeds;}
void ResumeControlPlaneUpdates(){++resumes;suspended=false;}
void PublishTrainingHandoffBoundary(int status){CHECK(suspended);CHECK(pending==1);CHECK(status==currentStatus);++publications;}
}
int GetStatus(){return currentStatus;}
#define GetModuleHandleA TestGetModuleHandleA
#define GetProcAddress TestGetProcAddress
#include "netplay/bridge/training_handoff.inl"
#undef GetModuleHandleA
#undef GetProcAddress
}

int main(){
    {TrainingHandoff backgroundListener(false);CHECK(backgroundListener.Ready());}
    CHECK(pending==0 && drains==0 && publications==0);
    loaded=false;
    {TrainingHandoff handoff;CHECK(handoff.Ready());}
    CHECK(pending==0 && drains==0);
    loaded=true;exportsPresent=false;
    {TrainingHandoff handoff;CHECK(!handoff.Ready());}
    CHECK(pending==0 && drains==0);
    exportsPresent=true;drainSucceeds=false;
    {TrainingHandoff handoff;CHECK(!handoff.Ready());CHECK(pending==0);}
    CHECK(!suspended && resumes==1 && publications==0);
    suspended=true;
    {TrainingHandoff handoff;CHECK(!handoff.Ready());CHECK(pending==0);}
    CHECK(suspended && resumes==1 && publications==0);
    suspended=false;drainSucceeds=true;currentStatus=1;
    {TrainingHandoff handoff;CHECK(handoff.Ready());CHECK(pending==1 && suspended);}
    CHECK(pending==0 && !suspended && publications==1 && resumes==2);
    suspended=true;currentStatus=0;
    {TrainingHandoff handoff;CHECK(handoff.Ready());CHECK(pending==1);}
    CHECK(pending==0 && suspended && publications==2 && resumes==2 && drains==4);
    return failures?1:0;
}
