#include "ui_layout.h"
#include "json.h"

namespace tsuchi { namespace browser { namespace ui { namespace {
using json::Value;
bool same(const char* a,const char* b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
struct Parser {
    const char* begin;Diagnostic* diagnostic;
    bool fail(P4M_Result code,Value where,const char* field,const char* reason){
        if(diagnostic){diagnostic->code=code;diagnostic->offset=where?uint32_t(where.begin-begin):0;
            uint32_t i=0;while(*field&&i<120)diagnostic->message[i++]=*field++;
            if(i<126)diagnostic->message[i++]=':';if(i<126)diagnostic->message[i++]=' ';
            while(*reason&&i<127)diagnostic->message[i++]=*reason++;diagnostic->message[i]=0;}
        return false;
    }
    bool fields(Value object,const char* const* names,Value* out,uint32_t count,const char* field,uint32_t optional=0){
        if(!object||*object.begin!='{')return fail(P4M_INVALID,object,field,"expected object");
        uint32_t cursor=0;Value key,value;char name[64];bool cut=false;
        while(json::entry(object,cursor,key,value)){
            if(!json::text(key,name,sizeof(name),&cut)||cut)return fail(P4M_UNSUPPORTED,key,field,"unknown field");
            uint32_t n=0;while(n<count&&!same(name,names[n]))++n;
            if(n==count)return fail(P4M_UNSUPPORTED,key,field,"unknown field");
            if(out[n])return fail(P4M_DUPLICATE,key,field,"duplicate decoded key");
            out[n]=value;
        }
        for(uint32_t i=0;i<count;++i)if(!(optional&(1u<<i))&&!out[i])return fail(P4M_INVALID,object,names[i],"missing required field");
        return true;
    }
    bool text(Value value,char* out,uint32_t cap,const char* field,bool empty=false){
        bool cut=false;if(!json::text(value,out,cap,&cut))return fail(P4M_INVALID,value,field,"expected plain UTF-8 text");
        if(cut)return fail(P4M_CAPACITY,value,field,"UTF-8 label exceeds bounded storage");
        if(!empty&&!out[0])return fail(P4M_INVALID,value,field,"label must not be empty");
        for(const unsigned char* p=(const unsigned char*)out;*p;++p)if(*p<32&&*p!='\n'&&*p!='\r'&&*p!='\t')return fail(P4M_INVALID,value,field,"unsupported text control");
        return true;
    }
    bool literal(Value value,const char* expected,const char* field){
        char actual[64];if(!text(value,actual,sizeof(actual),field))return false;
        return same(actual,expected)||fail(P4M_UNSUPPORTED,value,field,"unsupported fixed role");
    }
    bool integer(Value value,uint32_t& out,uint32_t low,uint32_t high,const char* field){
        uint64_t n=0;if(!json::number(value,n)||n<low||n>high)return fail(P4M_INVALID,value,field,"integer is out of range");
        out=uint32_t(n);return true;
    }
    bool number(Value value,float& out,float low,float high,const char* field){
        double n=0;if(!json::real(value,n)||n<low||n>high)return fail(P4M_INVALID,value,field,"finite number is out of range");
        out=float(n);return true;
    }
    bool rect_fields(Value object,Rect& out,Value* extras,const char* const* extra_names,uint32_t count,const char* field,bool below_fold=false,uint32_t optional=0){
        const char* names[8]{"x","y","width","height",nullptr,nullptr,nullptr,nullptr};
        Value values[8]{};for(uint32_t i=0;i<count;++i)names[i+4]=extra_names[i];
        if(!fields(object,names,values,count+4,field,optional<<4))return false;
        if(!number(values[0],out.x,0,1920,field)||!number(values[1],out.y,0,1080,field)||
           !number(values[2],out.width,1,1920,field)||!number(values[3],out.height,1,1080,field))return false;
        if(out.x+out.width>1920||(!below_fold&&out.y+out.height>1080))return fail(P4M_INVALID,object,field,"rectangle exceeds its bounded canvas");
        for(uint32_t i=0;i<count;++i)extras[i]=values[i+4];return true;
    }
    bool rect(Value object,Rect& out,const char* field,bool below_fold=false){return rect_fields(object,out,nullptr,nullptr,0,field,below_fold);}
    bool text_mode(Value value,TextMode& out,const char* field){
        if(!value)return true;char mode[16];if(!text(value,mode,sizeof(mode),field))return false;
        if(same(mode,"literal"))out=TextMode::Literal;else if(same(mode,"rich"))out=TextMode::Rich;
        else return fail(P4M_UNSUPPORTED,value,field,"unsupported text mode");return true;
    }
    bool footer(Value value,Footer& out,const char* field){
        const char* names[]{"text","textMode"};Value v[2]{};
        return fields(value,names,v,2,field,2)&&text(v[0],out.text,sizeof(out.text),field,true)&&text_mode(v[1],out.text_mode,field);
    }
    bool description(Value value,Details& out){
        const char* names[]{"textMode","hint"};Value v[2]{};
        return rect_fields(value,out.description,v,names,2,"details.description",true,3)&&
            text_mode(v[0],out.description_mode,"details.description.textMode")&&
            (!v[1]||text(v[1],out.description_hint,sizeof(out.description_hint),"details.description.hint",true));
    }
    bool sidebar(Value value,Sidebar& out){
        const char* names[]{"x","y","width","height","hiddenX","durationMs","itemPitch","items"};Value v[8]{};
        if(!fields(value,names,v,8,"sidebar"))return false;
        if(!number(v[0],out.x,0,1920,"sidebar.x")||!number(v[1],out.y,0,1080,"sidebar.y")||
           !number(v[2],out.width,160,640,"sidebar.width")||!number(v[3],out.height,100,1080,"sidebar.height")||
           !number(v[4],out.hidden_x,-1920,0,"sidebar.hiddenX")||!integer(v[5],out.duration_ms,80,1000,"sidebar.durationMs")||
           !number(v[6],out.item_pitch,80,200,"sidebar.itemPitch"))return false;
        if(out.x+out.width>1920||out.y+out.height>1080||out.hidden_x+out.width>0)return fail(P4M_INVALID,value,"sidebar","visible or hidden bounds invalid");
        if(!v[7]||*v[7].begin!='[')return fail(P4M_INVALID,v[7],"sidebar.items","expected array");
        uint32_t cursor=0;Value item;while(json::next(v[7],cursor,item)){
            if(out.count==max_sidebar_items)return fail(P4M_CAPACITY,item,"sidebar.items","at most six items");
            const char* fields_names[]{"id","label","sort","category","action"};Value f[5]{};auto& row=out.items[out.count];
            if(!fields(item,fields_names,f,5,"sidebar.item",16)||!text(f[0],row.id,sizeof(row.id),"sidebar.id")||
               !text(f[1],row.label,sizeof(row.label),"sidebar.label")||!integer(f[3],row.category,0,UINT32_MAX,"sidebar.category"))return false;
            for(const char* p=row.id;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='_'||*p=='-'))return fail(P4M_INVALID,f[0],"sidebar.id","expected lowercase ASCII identifier");
            for(uint32_t i=0;i<out.count;++i)if(same(row.id,out.items[i].id))return fail(P4M_DUPLICATE,f[0],"sidebar.id","duplicate item ID");
            char sort[32];if(!text(f[2],sort,sizeof(sort),"sidebar.sort"))return false;
            if(same(sort,"latest"))row.sort=Sort::Latest;else if(same(sort,"popular"))row.sort=Sort::Popular;
            else if(same(sort,"downloads"))row.sort=Sort::Downloads;else return fail(P4M_UNSUPPORTED,f[2],"sidebar.sort","unsupported provider sort");
            if(f[4]){char action[16];if(!text(f[4],action,sizeof(action),"sidebar.action"))return false;
                if(same(action,"search"))row.action=SidebarAction::Search;
                else if(same(action,"installed"))row.action=SidebarAction::Installed;
                else if(!same(action,"query"))return fail(P4M_UNSUPPORTED,f[4],"sidebar.action","unsupported action");}
            ++out.count;
        }
        return (out.count&&out.count*out.item_pitch<=out.height)||fail(P4M_INVALID,value,"sidebar.items","one to six items must fit the panel");
    }
    bool browse(Value value,Browse& out){
        const char* names[]{"title","columns","viewport","hiddenViewport","card","sidebar","footer"};Value v[7]{};
        if(!fields(value,names,v,7,"browse")||!text(v[0],out.title,sizeof(out.title),"browse.title")||
           !integer(v[1],out.columns,2,3,"browse.columns")||!rect(v[2],out.viewport,"browse.viewport")||
           !rect(v[3],out.hidden_viewport,"browse.hiddenViewport"))return false;
        const char* card_names[]{"width","height","imageHeight","labelHeight","gapX","gapY","imageFit"};Value c[7]{};
        if(!fields(v[4],card_names,c,7,"browse.card",64)||!number(c[0],out.card.width,160,960,"card.width")||
           !number(c[1],out.card.height,180,900,"card.height")||!number(c[2],out.card.image_height,64,640,"card.imageHeight")||
           !number(c[3],out.card.label_height,48,160,"card.labelHeight")||!number(c[4],out.card.gap_x,0,160,"card.gapX")||
           !number(c[5],out.card.gap_y,0,160,"card.gapY")||!sidebar(v[5],out.sidebar)||!footer(v[6],out.footer,"browse.footer"))return false;
        if(c[6]){char mode[16];if(!text(c[6],mode,sizeof(mode),"card.imageFit"))return false;
            if(same(mode,"cover"))out.card.image_fit=ImageFit::Cover;else if(!same(mode,"contain"))return fail(P4M_UNSUPPORTED,c[6],"card.imageFit","unsupported image fit");}
        const float width=out.columns*out.card.width+(out.columns-1)*out.card.gap_x;
        if(width>out.viewport.width||width>out.hidden_viewport.width||out.card.image_height+out.card.label_height>out.card.height)
            return fail(P4M_INVALID,value,"browse.card","card metrics do not fit the grid");
        const Rect views[]{out.viewport,out.hidden_viewport};
        for(uint32_t i=0;i<2;++i){const float pitch=out.card.height+out.card.gap_y;uint32_t rows=uint32_t(views[i].height/pitch);
            if(rows*pitch<views[i].height)++rows;++rows;
            if(rows*out.columns>12)return fail(P4M_CAPACITY,value,"browse.viewport","visible rows exceed the twelve-entry pool");}
        if(out.sidebar.x+out.sidebar.width>out.viewport.x)return fail(P4M_INVALID,value,"sidebar","sidebar overlaps the grid");
        return true;
    }
    bool action_rect(Value value,ActionRect& out,const char* field){
        const char* names[]{"label"};Value v[1]{};
        return rect_fields(value,out.rect,v,names,1,field,true)&&text(v[0],out.label,sizeof(out.label),field);
    }
    bool details(Value value,Details& out){
        const char* names[]{"title","titleRect","viewport","gallery","metadata","description","download","files","cancel","previewStrip","footer","author"};Value v[12]{};
        // titleRect is accepted for manifests written before the mod's name
        // moved into the page header; it is no longer used.
        if(!fields(value,names,v,12,"details",2048|2)||!text(v[0],out.title,sizeof(out.title),"details.title")||
           (v[1]&&!rect(v[1],out.title_rect,"details.titleRect",true))||!rect(v[2],out.viewport,"details.viewport")||
           !rect(v[3],out.gallery,"details.gallery",true)||!rect(v[4],out.metadata,"details.metadata",true)||
           !description(v[5],out)||!action_rect(v[6],out.download,"details.download")||
           !action_rect(v[8],out.cancel,"details.cancel")||!footer(v[10],out.footer,"details.footer"))return false;
        if(v[11]&&!action_rect(v[11],out.author,"details.author"))return false;
        const char* file_names[]{"rows","pitch"};Value f[2]{};
        if(!rect_fields(v[7],out.files.rect,f,file_names,2,"details.files",true)||!integer(f[0],out.files.rows,1,2,"files.rows")||
           !number(f[1],out.files.pitch,48,200,"files.pitch"))return false;
        if(out.files.pitch<out.files.rect.height)return fail(P4M_INVALID,v[7],"details.files","file row pitch is invalid");
        const char* strip_names[]{"count","gap"};Value s[2]{};
        if(!rect_fields(v[9],out.preview_strip.rect,s,strip_names,2,"details.previewStrip",true)||
           !integer(s[0],out.preview_strip.count,1,5,"previewStrip.count")||!number(s[1],out.preview_strip.gap,0,80,"previewStrip.gap"))return false;
        if((out.preview_strip.rect.width-(out.preview_strip.count-1)*out.preview_strip.gap)/out.preview_strip.count<80)
            return fail(P4M_INVALID,v[9],"previewStrip","thumbnail width is too small");
        const Rect roles[]{out.gallery,out.metadata,out.description,out.download.rect,out.files.rect,out.cancel.rect,out.preview_strip.rect};
        for(uint32_t i=0;i<7;++i)if(roles[i].x<out.viewport.x||roles[i].y<out.viewport.y||
            roles[i].x+roles[i].width>out.viewport.x+out.viewport.width)return fail(P4M_INVALID,value,"details","role is outside the scroll-content horizontal bounds");
        return true;
    }
    bool profile(Value value,Profile& out,const Details& details){
        // mods and back are accepted from manifests written when the profile
        // was a page of its own with two buttons. The profile is now the top of
        // the author's page, above their mods, and those are no longer used.
        const char* names[]{"title","avatar","info","mods","back","footer"};Value v[6]{};
        if(!fields(value,names,v,6,"profile",8|16)||!text(v[0],out.title,sizeof out.title,"profile.title")||
            !rect(v[1],out.avatar,"profile.avatar",true)||!rect(v[2],out.info,"profile.info",true)||
            (v[3]&&!action_rect(v[3],out.mods,"profile.mods"))||(v[4]&&!action_rect(v[4],out.back,"profile.back"))||!footer(v[5],out.footer,"profile.footer"))return false;
        // The author button is in the mod page's header line and the picture
        // and details in the author page's left column: anywhere on the
        // 1920-wide page.
        const Rect roles[]{out.avatar,out.info,details.author.rect};
        for(const auto& role:roles)if(role.x<0||role.y<0||role.x+role.width>1920)
            return fail(P4M_INVALID,value,"profile","role is outside the page");
        out.enabled=true;return true;
    }
    bool layout(Value value,Layout& out){
        const char* names[]{"$schema","version","id","adapter","title","entry","header","status","browse","details","profile"};Value v[11]{};
        if(!fields(value,names,v,11,"get-mods",1025))return false;
        if(v[0]&&*v[0].begin!='"')return fail(P4M_INVALID,v[0],"$schema","expected metadata string");
        uint32_t version=0;if(!integer(v[1],version,0,UINT32_MAX,"version"))return false;
        if(version!=1)return fail(P4M_VERSION,v[1],"version","unsupported version");
        if(!literal(v[2],"get-mods","id")||!literal(v[3],"gamebanana","adapter")||
           !text(v[4],out.title,sizeof(out.title),"title")||!rect(v[6],out.header,"header")||!rect(v[7],out.status,"status"))return false;
        const char* entry_names[]{"label","slot"};Value e[2]{};
        if(!(fields(v[5],entry_names,e,2,"entry")&&text(e[0],out.entry.label,sizeof(out.entry.label),"entry.label")&&
            literal(e[1],"after-game-modes","entry.slot")&&browse(v[8],out.browse)&&details(v[9],out.details)))return false;
        if(bool(v[10])!=(out.details.author.rect.width>0))return fail(P4M_INVALID,value,"profile","profile and details.author must be provided together");
        return !v[10]||profile(v[10],out.profile,out.details);
    }
};
} // namespace
bool parse(const char* input,uint32_t bytes,Layout& out,Diagnostic* diagnostic){
    if(diagnostic)*diagnostic={};Parser p{input,diagnostic};
    if(!input||!bytes)return p.fail(P4M_INVALID,{},"get-mods","expected bounded UTF-8 JSON");
    if(bytes>max_json_bytes)return p.fail(P4M_CAPACITY,{},"get-mods","manifest exceeds 16 KiB");
    Value value;if(!json::document(input,bytes,value))return p.fail(P4M_INVALID,{},"get-mods","invalid strict JSON or UTF-8");
    Layout candidate{};if(!p.layout(value,candidate))return false;out=candidate;return true;
}
} } }
