#include "github.h"
#include "bridge.h"
#include "json.h"
#include "mod_catalog.h"
#include <string.h>
namespace tsuchi::browser { namespace {
using json::Value;
template<class T> T* find_char(T* s,char value){for(;*s;++s)if(*s==value)return s;return nullptr;}
void copy(char* out,uint32_t capacity,const char* text){catalog::plain(text,out,capacity);}
template<size_t N> void text(Value value,const char* key,char(&out)[N]){json::text(json::member(value,key),out,N);}
uint64_t number(Value value,const char* key){uint64_t n=0;json::number(json::member(value,key),n);return n;}
uint64_t date(Value value,const char* key){
 char s[32]{};text(value,key,s);if(strlen(s)<20||s[4]!='-'||s[7]!='-'||s[10]!='T')return 0;
 auto digits=[&](uint32_t at,uint32_t count){int n=0;while(count--){if(s[at]<'0'||s[at]>'9')return -1;n=n*10+s[at++]-'0';}return n;};
 int y=digits(0,4),m=digits(5,2),d=digits(8,2),h=digits(11,2),min=digits(14,2),sec=digits(17,2);
 if(y<1970||m<1||m>12||d<1||d>31||h<0||h>23||min<0||min>59||sec<0||sec>60)return 0;
 y-=m<=2;const int era=y/400,yoe=y-era*400,doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
 const int days=era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
 return uint64_t(days)*86400+uint64_t(h*3600+min*60+sec);
}
bool repo(Value v,Listing& out){
 char key[128]{};text(v,"full_name",key);catalog::Item item{};copy(item.key,sizeof item.key,key);char checked[1024]{};
 if(!catalog::files_url(catalog::Source::github,item,false,1,checked))return false;
 bool hidden=false;json::boolean(json::member(v,"private"),hidden);if(hidden)return false;json::boolean(json::member(v,"fork"),hidden);if(hidden)return false;
 out={};out.source=0;copy(out.source_key,sizeof out.source_key,key);out.id=source_id(0,key);if(!out.id)return false;
 text(v,"name",out.title);text(json::member(v,"owner"),"login",out.author);if(!out.title[0]||!out.author[0])return false;
 const char* slash=find_char(key,'/');if(!slash||size_t(slash-key)!=strlen(out.author)||memcmp(key,out.author,strlen(out.author)))return false;
 out.author_id=source_id(0,out.author);text(json::member(v,"owner"),"avatar_url",out.author_avatar_url);
 out.updated=date(v,"updated_at");out.created=date(v,"created_at");out.likes=number(v,"stargazers_count");out.statistics=Likes;out.has_files=true;
 copy(out.category,sizeof out.category,"GitHub");
 // GitHub's repository preview is the card image; no screenshot is fabricated.
 const char* prefix="https://opengraph.githubassets.com/tsuchi/";const size_t a=strlen(prefix),b=strlen(key);
 if(a+b<sizeof out.image.url){memcpy(out.image.url,prefix,a);memcpy(out.image.url+a,key,b+1);copy(out.image.card,sizeof out.image.card,out.image.url);copy(out.image.thumbnail,sizeof out.image.thumbnail,out.author_avatar_url);out.image.width=1200;out.image.height=600;}
 return true;
}
bool url_for(const char* prefix,const char* key,const char* suffix,char(&out)[1024]){
 if(!key||strlen(prefix)+strlen(key)+strlen(suffix)>=sizeof out)return false;size_t n=0;
 const char* parts[]={prefix,key,suffix};for(const char* s:parts){const size_t k=strlen(s);memcpy(out+n,s,k);n+=k;}out[n]=0;return true;
}
}
void GitHub::cancel(P4M_Transfer& h,Operation& o){if(h&&api_){api_->cancel(h);api_->release(h);}h=0;if(o.state==State::Loading)o.state=State::Cancelled;}
void GitHub::cancel_search(){cancel(search_handle_,search_op_);}void GitHub::cancel_detail(){cancel(detail_handle_,detail_op_);}void GitHub::cancel_author(){cancel(author_handle_,author_op_);}
P4M_Result GitHub::start(P4M_Transfer& h,Operation& o,const char* url,uint32_t generation){
 cancel(h,o);o={};o.generation=generation;if(!api_)return TSUCHI_NOT_READY;
 P4M_HttpsRequest request{sizeof request,P4M_HTTPS_MEMORY,url,nullptr,sizeof scratch_,15000,60000};
 const auto r=api_->get(owner_,&request,&h);o.api_result=r;o.state=r==TSUCHI_OK?State::Loading:State::Failed;o.error=r==TSUCHI_OK?Error::None:Error::Api;return r;
}
P4M_Result GitHub::search(const Query& q,uint32_t page,uint32_t generation){
 cancel_search();search_op_={};search_op_.generation=generation;search_op_.state=State::Failed;search_op_.error=Error::Api;
 batch_={};batch_.page=page;batch_.generation=generation;author_filter_=q.author_id;
 char url[1024]{};if(!catalog::search_url(catalog::Source::github,q.search,page,url))return TSUCHI_INVALID;
 // The same sections query both providers. Stars rank GitHub's popular entries.
 if(q.sort!=Sort::Latest){char* at=nullptr;for(char* p=url;*p;++p)if(strlen(p)>=12&&!memcmp(p,"sort=updated",12)){at=p;break;}if(at){const size_t tail=strlen(at+12);memmove(at+10,at+12,tail+1);memcpy(at,"sort=stars",10);}}
 if(q.author_id){const char* author=source_key(q.author_id);if(!author||find_char(author,'/'))return TSUCHI_INVALID;
  char* at=find_char(url,'&');if(!at)return TSUCHI_INVALID;const size_t add=strlen(author)+8;if(strlen(url)+add>=sizeof url)return TSUCHI_INVALID;
  memmove(at+add,at,strlen(at)+1);memcpy(at,"%20user:",8);memcpy(at+8,author,strlen(author));}
 return start(search_handle_,search_op_,url,generation);
}
P4M_Result GitHub::detail(uint32_t id,uint32_t generation){
 cancel_detail();detail_op_={};detail_op_.generation=generation;detail_op_.state=State::Failed;detail_op_.error=Error::Api;
 const char* key=source_key(id);char url[1024]{};if(!key||!find_char(key,'/')||!url_for("https://api.github.com/repos/",key,"",url))return TSUCHI_INVALID;
 detail_={};detail_.generation=generation;detail_.listing.id=id;release_stage_=false;return start(detail_handle_,detail_op_,url,generation);
}
P4M_Result GitHub::author(uint32_t id,uint32_t generation){cancel_author();author_op_={};author_op_.generation=generation;author_op_.state=State::Failed;author_op_.error=Error::Api;const char* key=source_key(id);char url[1024]{};
 if(!key||find_char(key,'/')||!url_for("https://api.github.com/users/",key,"",url))return TSUCHI_INVALID;
 author_={};author_.id=id;author_.generation=generation;return start(author_handle_,author_op_,url,generation);
}
bool GitHub::response(P4M_Transfer& h,Operation& o,uint32_t& bytes){
 if(!h||o.state!=State::Loading)return false;P4M_HttpsStatus s{};s.size=sizeof s;auto r=api_->status(h,&s);o.transfer=s;
 if(r==TSUCHI_OK&&(s.state==P4M_HTTPS_QUEUED||s.state==P4M_HTTPS_RUNNING))return false;
 bool ok=r==TSUCHI_OK&&s.state==P4M_HTTPS_SUCCEEDED&&s.received&&s.received<=sizeof scratch_;
 if(ok)ok=api_->read(h,0,scratch_,uint32_t(s.received),&bytes)==TSUCHI_OK&&bytes==s.received;
 api_->release(h);h=0;o.state=ok?State::Ready:State::Failed;o.error=ok?Error::None:Error::Transport;return ok;
}
void GitHub::pump(){
 uint32_t bytes=0;
 if(response(search_handle_,search_op_,bytes)){
  catalog::Page validated{};Value root;
  bool ok=catalog::parse_page(catalog::Source::github,scratch_,bytes,batch_.page,validated)&&json::document(scratch_,bytes,root);
  batch_.count=0;batch_.has_more=validated.more;batch_.total=uint32_t(number(root,"total_count"));batch_.perpage=6;
  uint32_t pos=0;Value value;if(ok){const auto items=json::member(root,"items");while(json::next(items,pos,value)&&batch_.count<6){Listing l{};
   char key[128]{};text(value,"full_name",key);bool included=false;for(uint32_t i=0;i<validated.count;++i)if(!strcmp(key,validated.items[i].key))included=true;if(!included)continue;
   if(!repo(value,l)||(author_filter_&&l.author_id!=author_filter_)){ok=false;break;}batch_.listings[batch_.count++]=l;}}
  if(!ok){search_op_.state=State::Failed;search_op_.error=Error::Schema;batch_.count=0;}
 }
 if(response(detail_handle_,detail_op_,bytes)){
  if(!release_stage_){Value root;const auto id=detail_.listing.id;bool ok=json::document(scratch_,bytes,root)&&repo(root,detail_.listing)&&detail_.listing.id==id;
   if(ok){text(root,"description",detail_.description);copy(detail_.tagline,sizeof detail_.tagline,detail_.description);detail_.previews[0]=detail_.listing.image;detail_.preview_count=detail_.listing.image.url[0]?1:0;
    catalog::Item item{};copy(item.key,sizeof item.key,detail_.listing.source_key);char url[1024]{};ok=catalog::files_url(catalog::Source::github,item,false,1,url);
    if(ok){release_stage_=true;start(detail_handle_,detail_op_,url,detail_.generation);}}
   if(!ok){detail_op_.state=State::Failed;detail_op_.error=Error::Schema;}
  }else{catalog::Item item{};copy(item.key,sizeof item.key,detail_.listing.source_key);detail_.file_count=0;bool ok=true;
   for(uint32_t skip=0;skip<max_files&&ok;skip+=catalog::kFiles){catalog::Files files{};ok=catalog::parse_files(catalog::Source::github,item,scratch_,bytes,false,files,skip);if(!ok)break;
    for(uint32_t i=0;i<files.count&&detail_.file_count<max_files;++i){const auto& f=files.items[i];auto& out=detail_.files[detail_.file_count++];out={};out.submission_id=detail_.listing.id;out.id=file_id(0,f.identity);out.size=f.bytes;out.updated=f.updated;
     copy(out.identity,sizeof out.identity,f.identity);copy(out.digest,sizeof out.digest,f.digest);copy(out.name,sizeof out.name,f.name);copy(out.url,sizeof out.url,f.url);copy(out.version,sizeof out.version,f.release);}
    detail_.files_truncated=files.more_files||files.more||skip+files.count>max_files;if(!files.more_files)break;}
   detail_.listing.has_files=detail_.file_count!=0;if(!ok){detail_op_.state=State::Failed;detail_op_.error=Error::Schema;}
  }
 }
 if(response(author_handle_,author_op_,bytes)){Value root;char login[96]{};bool ok=json::document(scratch_,bytes,root);if(ok){text(root,"login",login);ok=source_id(0,login)==author_.id;}
  if(ok){text(root,"name",author_.name);if(!author_.name[0])copy(author_.name,sizeof author_.name,login);text(root,"bio",author_.title);text(root,"avatar_url",author_.avatar_url);text(root,"html_url",author_.profile_url);author_.joined=date(root,"created_at");author_.has_joined=author_.joined!=0;author_.subscribers=number(root,"followers");author_.has_subscribers=true;}
  else{author_op_.state=State::Failed;author_op_.error=Error::Schema;}
 }
}
}
