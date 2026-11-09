#pragma once
#include <stdint.h>
namespace tsuchi { namespace browser { namespace detail_presentation {
constexpr uint32_t max_files=32;
struct Order {
    uint32_t ids[max_files+3]{},count=0;
    bool contains(uint32_t id) const {for(uint32_t i=0;i<count;++i)if(ids[i]==id)return true;return false;}
    uint32_t move(uint32_t id,int direction) const {
        if(!count)return id;
        for(uint32_t i=0;i<count;++i)if(ids[i]==id)return ids[direction<0?(i?i-1:0):direction>0?(i+1<count?i+1:i):i];
        return ids[direction<0?count-1:0];
    }
};
// Files are one scrolling group. Traverse every file before leaving the group,
// even when only two rows are visible; the other roles follow their JSON y.
inline Order order(uint32_t files,bool author,bool download,bool cancel,const float (&y)[4]){
    Order out;if(files>max_files)return out;
    uint32_t roles[]{0,1,2,3};
    for(uint32_t i=1;i<4;++i){const uint32_t role=roles[i];uint32_t j=i;while(j&&y[role]<y[roles[j-1]]){roles[j]=roles[j-1];--j;}roles[j]=role;}
    for(uint32_t role:roles){if(role==0){for(uint32_t i=0;i<files;++i)out.ids[out.count++]=i;}
        else if(role==1&&download)out.ids[out.count++]=files;
        else if(role==2&&cancel)out.ids[out.count++]=files+1;
        else if(role==3&&author)out.ids[out.count++]=files+2;}
    return out;
}
inline uint32_t window(uint32_t first,uint32_t selected,uint32_t count,uint32_t slots){
    if(!slots||count<=slots)return 0;
    const uint32_t limit=count-slots;if(first>limit)first=limit;
    if(selected>=count)return first;
    if(selected<first)first=selected;else if(selected-first>=slots)first=selected-slots+1;
    return first;
}
// The page has two sides and the focus is on one of them. Content is the
// screenshots with the description below; Actions is the author and the
// install controls. Left and right cross between the sides and do nothing
// else. On the content side up and down scroll the page. L and R (Previous,
// Next) step through the screenshots from either side.
enum class Pane : uint32_t { Content, Actions };
enum class Move : uint32_t { Left, Right, Up, Down, Previous, Next };
struct Focus {Pane pane=Pane::Actions;uint32_t action=0,preview=0;};
struct Step {Focus focus;int scroll=0;}; // scroll: -1 up, +1 down, on the content side
inline Step move(Focus at,Move direction,const Order& actions,uint32_t previews){
    Step out{at,0};
    if(direction==Move::Previous){if(at.preview)--out.focus.preview;return out;}
    if(direction==Move::Next){if(at.preview+1<previews)++out.focus.preview;return out;}
    if(at.pane==Pane::Actions){
        if(direction==Move::Up)out.focus.action=actions.move(at.action,-1);
        else if(direction==Move::Down)out.focus.action=actions.move(at.action,1);
        else if(direction==Move::Left)out.focus.pane=Pane::Content;
        return out;
    }
    if(direction==Move::Up)out.scroll=-1;
    else if(direction==Move::Down)out.scroll=1;
    else if(direction==Move::Right&&actions.count)out.focus.pane=Pane::Actions;
    return out;
}
// Where the focus starts on a mod: the install button when it can be used,
// otherwise the first control, otherwise the content.
inline Focus initial(const Order& actions,uint32_t install){
    Focus out;if(!actions.count){out.pane=Pane::Content;return out;}
    out.action=actions.contains(install)?install:actions.ids[0];return out;
}
struct Scroll {float outer,inner;};
inline float clamp(float value,float maximum){return !(value>0)?0:value>maximum?maximum:value;}
// Reveal the description with the page first, then move its wrapped contents.
// Reverse in the opposite order so all input methods follow the same path.
inline Scroll scroll(Scroll at,float outer_limit,float inner_limit,float delta){
    if(!(outer_limit>0))outer_limit=0;if(!(inner_limit>0))inner_limit=0;
    at.outer=clamp(at.outer,outer_limit);at.inner=clamp(at.inner,inner_limit);
    if(delta>0){const float step=delta<outer_limit-at.outer?delta:outer_limit-at.outer;at.outer+=step;at.inner=clamp(at.inner+delta-step,inner_limit);}
    else if(delta<0){const float step=-delta<at.inner?-delta:at.inner;at.inner-=step;at.outer=clamp(at.outer+delta+step,outer_limit);}
    return at;
}
}}}
