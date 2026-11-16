#include <cstdio>
#include <fstream>
#include <sstream>
#include "host.h"
#include "objects.h"
#include "tsuchi_js.h"
#include "reflect_world.h"
#include "memory_backend.h"
#include "reflection.h"
#include "registry.h"
void global_handle(JSContext* ctx, const char* name, const char* kind, uint64_t id) {
    tsuchi_js::Value global(ctx,JS_GetGlobalObject(ctx));
    JS_SetPropertyStr(ctx,global,name,tsuchi_js::push_handle(ctx,kind,tsuchi::game::to_handle(id)));
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    tsuchi_js::set_error_handler([](const char* message) { std::fprintf(stderr,"%s\n",message); });
    build_world(); tsuchi::reflect_start(); tsuchi::memory_start();
    TSUCHI_Mod* mod=tsuchi::mod_create("reflection.javascript");
    auto* state=tsuchi_js::create(mod,&tsuchi::host); if(!state) return 1;
    auto* ctx=tsuchi_js::context(state);
    global_handle(ctx,"red","actor",w.red); global_handle(ctx,"body","object",w.body);
    global_handle(ctx,"spare","object",w.spare); global_handle(ctx,"wordy","object",w.wordy);
    const fake::Engine before=fake::engine();
    std::ifstream file(argv[1],std::ios::binary); std::ostringstream buffer; buffer<<file.rdbuf(); const auto source=buffer.str();
    if(!tsuchi_js::evaluate(ctx,source.c_str(),source.size(),"reflect.js",false)) { tsuchi_js::report(ctx); return 1; }
    if(tsuchi::owned_count(mod)!=0 || fake::engine().initialized==before.initialized || w.heard!=u"Captain") return 1;
    const char* kinds[]={"dialog","pause_hold","hook_id","prepared_call"};
    for(const char* kind:kinds) {
        tsuchi_js::Value resource(ctx,tsuchi_js::push_handle(ctx,kind,9007199254740993ULL)); uint64_t id=0;
        if(tsuchi_js::to_handle(ctx,resource,&id)) return 1;
    }
    tsuchi::mod_stop(mod); tsuchi_js::destroy(state); return 0;
}