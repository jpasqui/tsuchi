#include "shader_archive_manifest.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
static int allocations_left=-1;
static unsigned allocations_live=0;
namespace tsuchi {
void* allocate(size_t bytes) { if(!allocations_left)return nullptr;if(allocations_left>0)--allocations_left;void* p=std::malloc(bytes);if(p)++allocations_live;return p; }
void release(void* block) { if(block)--allocations_live;std::free(block); }
}
namespace a=tsuchi::shaders::archive;
using tsuchi::Json;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) {std::cerr<<"failed at "<<__LINE__<<'\n';return 1;} } while(false)
static std::string hex(const uint8_t* bytes,size_t count) {std::string out;for(size_t i=0;i<count;++i){out+="0123456789abcdef"[bytes[i]>>4];out+="0123456789abcdef"[bytes[i]&15];}return out;}
static std::string digest(const std::string& text){uint8_t out[20];a::sha1(reinterpret_cast<const uint8_t*>(text.data()),uint32_t(text.size()),out);return hex(out,20);}
static void word(std::vector<uint8_t>& bytes,uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(value>>(8*i)));}
static void set(std::vector<uint8_t>& bytes,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=uint8_t(value>>(8*i));}
static std::vector<uint8_t> archive(const std::string& payload,bool compressed=false,bool duplicateMap=false) {
    uint8_t hash[20];a::sha1(reinterpret_cast<const uint8_t*>(payload.data()),uint32_t(payload.size()),hash);
    const unsigned maps=duplicateMap?2:1;
    std::vector<uint8_t> bytes;word(bytes,2);word(bytes,maps);bytes.insert(bytes.end(),20*maps,0);
    word(bytes,1);bytes.insert(bytes.end(),hash,hash+20);word(bytes,maps);
    for(unsigned i=0;i<maps;++i){word(bytes,0);word(bytes,1);word(bytes,0);word(bytes,1);}word(bytes,1);
    word(bytes,0);word(bytes,0);word(bytes,uint32_t(payload.size())+(compressed?1:0));word(bytes,uint32_t(payload.size()));bytes.push_back(3);
    word(bytes,1);word(bytes,0);word(bytes,0);word(bytes,uint32_t(payload.size())+(compressed?1:0));word(bytes,0);
    word(bytes,1);word(bytes,0);if(compressed)bytes.push_back(uint8_t(payload.size()<<4));bytes.insert(bytes.end(),payload.begin(),payload.end());return bytes;
}
static std::string metadata(const std::string& payload) {
    return "{\"library\":\"kadan_test\",\"archive\":\"ShaderArchive-kadan_test-GLSL_SWITCH.ushaderbytecode\",\"materials\":[{\"material\":\"/Game/Mods/Test/M.M\",\"game_material\":\"/Game/Carrot4/Game\",\"map_id\":\"0000000000000000000000000000000000000000\",\"archive_map\":\"0000000000000000000000000000000000000000\",\"map_shaders\":2,\"shaders\":[{\"place\":1,\"hash\":\""+digest(payload)+"\"}],\"content\":{\"old_size\":16,\"new_size\":28,\"operations\":[{\"offset\":0,\"length\":4},{\"bytes\":\"4e4e4e4e4e4e4e4e4e4e4e4e\"},{\"offset\":4,\"length\":12}],\"names\":[{\"name\":\"Tint\",\"number\":0,\"offsets\":[4]}]}}]}";
}
static tsuchi::shaders::Read read(const std::vector<uint8_t>& bytes,const std::string& text,std::vector<uint8_t>* decoded=nullptr) {
    const Json* json=tsuchi::read_json(text.data(),text.size());if(!json)return tsuchi::shaders::Read::bad_value;
    uint8_t* output=nullptr;uint32_t count=0;const auto result=a::read(*json,bytes.data(),uint32_t(bytes.size()),output,count);
    if(decoded&&result==tsuchi::shaders::Read::ok)decoded->assign(output,output+count);
    tsuchi::release(output);tsuchi::release_json(json);return result;
}
int main() {
    using tsuchi::shaders::Read;
    CHECK(digest("")=="da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(digest("abc")=="a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(digest(std::string(1000,'a'))=="291e9a6c66994949b57ba5e650361e98fc36b1ba");
    const std::string payload="program",text=metadata(payload);const auto bytes=archive(payload);
    std::vector<uint8_t> decoded;CHECK(read(bytes,text,&decoded)==Read::ok);
    tsuchi::shaders::Material rows[1];tsuchi::shaders::Shader shaders[1];
    tsuchi::shaders::File file{decoded.data(),uint32_t(decoded.size()),rows,1,0,shaders,1,0};
    CHECK(tsuchi::shaders::read(&file)==Read::ok&&file.material_count==1&&file.shader_count==1);
    CHECK(shaders[0].place==1&&std::string(reinterpret_cast<char*>(decoded.data()+shaders[0].payload),shaders[0].size)==payload);
    std::string content(28,'?'),original="0123456789abcdef";
    tsuchi::shaders::build_content(decoded.data(),rows[0],reinterpret_cast<const uint8_t*>(original.data()),reinterpret_cast<uint8_t*>(&content[0]));
    CHECK(content=="0123NNNNNNNNNNNN456789abcdef");
    CHECK(read(archive(payload,true),text)==Read::ok);
    for(size_t cut=0;cut<bytes.size();++cut)CHECK(read(std::vector<uint8_t>(bytes.begin(),bytes.begin()+ptrdiff_t(cut)),text)!=Read::ok);
    auto bad=bytes;bad.back()^=1;CHECK(read(bad,text)==Read::bad_value);
    bad=bytes;bad[92]=0;CHECK(read(bad,text)==Read::bad_value);
    bad=bytes;set(bad,4,0xffffffff);CHECK(read(bad,text)==Read::bad_value);
    bad=bytes;set(bad,76,0xffffffff);set(bad,80,0xffffffff);CHECK(read(bad,text)==Read::bad_value);
    bad=bytes;set(bad,84,0x100001);CHECK(read(bad,text)==Read::bad_value);
    bad=bytes;set(bad,88,0x100001);CHECK(read(bad,text)==Read::bad_value);
    bad=bytes;set(bad,117,1);CHECK(read(bad,text)==Read::bad_value);
    bad=archive(payload,true);bad[121]=0;CHECK(read(bad,text)==Read::bad_value);
    auto wrong=text;wrong.replace(wrong.find("\"place\":1"),9,"\"place\":2");CHECK(read(bytes,wrong)==Read::bad_value);
    wrong=text;wrong.replace(wrong.find("\"offsets\":[4]"),13,"\"offsets\":[28]");CHECK(read(bytes,wrong)==Read::bad_value);
    wrong=text;wrong.replace(wrong.find("\"length\":12"),11,"\"length\":13");CHECK(read(bytes,wrong)==Read::bad_value);
    wrong=text;wrong.replace(wrong.find("ShaderArchive-"),14,"../bad-folder/");CHECK(read(bytes,wrong)==Read::bad_value);
    wrong=text;wrong[wrong.find("\"archive_map\":\"")+15]='1';CHECK(read(bytes,wrong)==Read::bad_value);
    wrong=text;wrong.replace(wrong.find("\"archive_map\":"),14,"\"ignored_map\":");CHECK(read(bytes,wrong)==Read::bad_value);
    bad=bytes;bad[8]=1;CHECK(read(bad,text)==Read::bad_value);
    CHECK(read(archive(payload,false,true),text)==Read::bad_value);
    // Allocation failures in either decode pass, or the output allocation,
    // report memory exhaustion and publish no partial result.
    const auto compressed=archive(payload,true);
    const auto* parsed=tsuchi::read_json(text.data(),text.size());CHECK(parsed);
    const unsigned before=allocations_live;
    for(int left=0;left<3;++left){allocations_left=left;uint8_t* output=nullptr;uint32_t count=0;
        CHECK(a::read(*parsed,compressed.data(),uint32_t(compressed.size()),output,count)==Read::full);
        CHECK(output==nullptr&&count==0&&allocations_live==before);allocations_left=-1;}
    allocations_left=0;uint8_t* output=nullptr;uint32_t count=0;
    CHECK(a::read(*parsed,bytes.data(),uint32_t(bytes.size()),output,count)==Read::full);
    CHECK(output==nullptr&&count==0&&allocations_live==before);allocations_left=-1;
    tsuchi::release_json(parsed);CHECK(allocations_live==0);
    std::cout<<checks<<" creator archive checks passed\n";
}
