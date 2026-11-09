#define _CRT_SECURE_NO_WARNINGS
#include "mod_catalog.h"
#include "mod_receipt.h"
#include "mod_zip.h"
#include "registry.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
using namespace tsuchi;
namespace {
std::string input;
std::map<std::string,std::vector<char>> outputs;
bool committed=false,aborted=false,cancelled_flag=false,fail_write=false;
uint32_t opened=0;
void* alloc(uint64_t n){return std::malloc(size_t(n));}
uint32_t open_in(void*,const char*,void** f,uint64_t* n){*f=&input;*n=input.size();return 0;}
uint32_t read_in(void*,void*,uint64_t off,void* out,uint32_t n,uint32_t* got){if(off>input.size()||n>input.size()-off)return 1;memcpy(out,input.data()+off,n);*got=n;return 0;}
void close_in(void*,void*){}
uint32_t begin(void*,const char*,void** s){*s=&input;return 0;}
uint32_t open_out(void*,void*,const char* name,uint64_t n,void** f){++opened;auto& v=outputs[name];v.resize(size_t(n));*f=&v;return 0;}
uint32_t write_out(void*,void* f,uint64_t off,const void* bytes,uint32_t n){if(fail_write)return 1;auto& v=*static_cast<std::vector<char>*>(f);assert(off<=v.size()&&n<=v.size()-off);memcpy(v.data()+off,bytes,n);return 0;}
uint32_t finish(void*,void*){return 0;}
uint32_t commit(void*,void*){committed=true;return 0;}
uint32_t abort(void*,void*){aborted=true;outputs.clear();return 0;}
bool stopped(void*){return cancelled_flag;}
void progress(void* p){if(p&&opened)cancelled_flag=true;}
}
int main(int argc,char** argv){
    assert(argc>=3);set_allocator({std::malloc,std::free});std::ifstream stream(argv[2],std::ios::binary);input.assign(std::istreambuf_iterator<char>(stream),{});
    if(!strcmp(argv[1],"zip")){
        fail_write=argc>3&&!strcmp(argv[3],"write");cancelled_flag=argc>3&&!strcmp(argv[3],"cancel");
        mod_zip::Platform io{nullptr,alloc,std::free,open_in,read_in,close_in,begin,open_out,write_out,finish,commit,abort};mod_zip::Status s{};
        mod_zip::Request request{};request.program_id=0x0100B7C00933A000;if(argc>3&&!strcmp(argv[3],"legacy"))strcpy(request.legacy_prefix,"external.0123456789abcdef01234567");
        auto result=mod_zip::extract_zip(io,request, {stopped,nullptr},s,progress,argc>3&&!strcmp(argv[3],"midcancel")?&io:nullptr);
        if(result.committed){assert(committed&&!aborted);for(const auto& file:outputs){uint8_t hash[32];sha256(file.second.data(),file.second.size(),hash);std::cout<<file.first<<" ";for(auto c:hash){const char* h="0123456789abcdef";std::cout<<h[c>>4]<<h[c&15];}std::cout<<"\n";}return 0;}
        assert(!committed&&(!opened||aborted));std::cout<<"refused "<<int(result.error)<<"\n";return 2;
    }
    if(!strcmp(argv[1],"hash")){
        Sha256 streamed;for(size_t i=0;i<input.size();i+=17)streamed.add(input.data()+i,std::min(size_t(17),input.size()-i));uint8_t one[32],two[32];sha256(input.data(),input.size(),one);streamed.finish(two);assert(!memcmp(one,two,32));return 0;
    }
    catalog::Source source=argc>3&&!strcmp(argv[3],"gb")?catalog::Source::gamebanana:catalog::Source::github;
    if(!strcmp(argv[1],"page")){
        catalog::Page page{};strcpy(page.items[0].name,"retained");page.count=1;
        bool ok=catalog::parse_page(source,input.data(),input.size(),1,page);
        if(!ok){assert(!strcmp(page.items[0].name,"retained"));return 2;}
        std::cout<<page.count<<" "<<page.more<<" "<<page.incomplete<<"\n";for(uint32_t i=0;i<page.count;++i)std::cout<<page.items[i].key<<" "<<page.items[i].name<<"\n";return 0;
    }
    if(!strcmp(argv[1],"files")){
        catalog::Item item{};strcpy(item.key,source==catalog::Source::github?"author/mod":"12");catalog::Files files{};
        if(!catalog::parse_files(source,item,input.data(),input.size(),argc>4,files))return 2;
        std::cout<<files.count<<"\n";for(uint32_t i=0;i<files.count;++i)std::cout<<files.items[i].name<<" "<<files.items[i].url<<"\n";return 0;
    }
    if(!strcmp(argv[1],"url")){char url[1024];assert(catalog::search_url(source,input.c_str(),2,url));std::cout<<url;return 0;}
    if(!strcmp(argv[1],"receipt")){
        catalog::Receipt r{};r.count=1;strcpy(r.folder,"0123456789abcdef0123456789abcdef");memset(r.digest,'a',64);strcpy(r.mods[0],"author.mod");catalog::seal(r);assert(catalog::receipt_valid(r));
        catalog::ReceiptV1 old{};memcpy(&old,&r,offsetof(catalog::ReceiptV1,checksum));memcpy(old.magic,"TSMOD01",8);sha256(&old,offsetof(catalog::ReceiptV1,checksum),old.checksum);
        catalog::Receipt upgraded{};assert(catalog::read_receipt(&old,sizeof old,upgraded));assert(!strcmp(upgraded.mods[0],"author.mod")&&!upgraded.updated&&!upgraded.revision&&!upgraded.previous[0]);
        old.checksum[0]^=1;assert(!catalog::read_receipt(&old,sizeof old,upgraded));
        auto changed=r;changed.count=2;assert(!catalog::receipt_valid(changed));changed=r;changed.mods[0][0]='.';catalog::seal(changed);assert(!catalog::receipt_valid(changed));changed=r;memset(changed.key,'a',sizeof changed.key);catalog::seal(changed);assert(!catalog::receipt_valid(changed));return 0;
    }
    return 3;
}
