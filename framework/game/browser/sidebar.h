#pragma once
#include "provider.h"

namespace tsuchi { namespace browser {

// Presentation-only, game-thread state. The adapter retains the Controller's
// stable selection/viewport; no query, network, installation or UObject lives
// here. This is not a callable retail-widget ABI.
enum class BrowseFocus : uint32_t { Sidebar, Grid };
enum class SidebarPhase : uint32_t { Hidden, Opening, Shown, Closing };
enum class SectionKind : uint32_t { Sort, Category };
struct SectionKey {
    SectionKind kind=SectionKind::Sort;
    uint32_t value=0;
    bool operator==(SectionKey other) const {return kind==other.kind&&value==other.value;}
};
struct SidebarAnimation {
    uint64_t host=0,serial=0;
    bool show=false;
    explicit operator bool() const {return host&&serial;}
};

class Sidebar {
public:
    static constexpr uint32_t MAX_SECTIONS=4+max_categories;
    // Host identity is independent of catalogue query generations. A sort,
    // category refresh or thumbnail completion must not restart animations.
    void open() {
        bump(host_);active_=true;detail_=false;focus_=BrowseFocus::Sidebar;
        phase_=SidebarPhase::Hidden;pending_={};desired_=true;
        selected_={};count_=0;
    }
    void close() {
        bump(host_);active_=detail_=desired_=false;pending_={};
        phase_=SidebarPhase::Hidden;count_=0;
    }
    bool active() const {return active_;}
    uint64_t host() const {return host_;}
    BrowseFocus focus() const {return focus_;}
    SidebarPhase phase() const {return phase_;}
    bool in_detail() const {return detail_;}
    SectionKey selected() const {return selected_;}
    uint32_t count() const {return count_;}
    const SectionKey* section(uint32_t index) const {return index<count_?&sections_[index]:nullptr;}

    // Stable keys survive lazy category refreshes/reordering. Duplicates and
    // category ID zero are ignored; zero always means the single All row.
    void categories(const Categories& categories) {
        if(!active_)return;
        count_=0;
        for(uint32_t i=0;i<3;++i)sections_[count_++]={SectionKind::Sort,i};
        sections_[count_++]={SectionKind::Category,0};
        for(uint32_t i=0;i<categories.count&&i<max_categories;++i){
            const uint32_t id=categories.records[i].id;if(!id)continue;
            bool seen=false;
            for(uint32_t n=3;n<count_;++n)if(sections_[n].value==id)seen=true;
            if(!seen)sections_[count_++]={SectionKind::Category,id};
        }
        bool found=false;for(uint32_t i=0;i<count_;++i)if(sections_[i]==selected_)found=true;
        if(!found)selected_={SectionKind::Category,0};
    }
    bool move(int32_t direction) {
        if(!active_||detail_||focus_!=BrowseFocus::Sidebar||!direction)return false;
        for(uint32_t i=0;i<count_;++i)if(sections_[i]==selected_){
            if(direction<0&&i){selected_=sections_[i-1];return true;}
            if(direction>0&&i+1<count_){selected_=sections_[i+1];return true;}
            return false;
        }
        return false;
    }
    bool choose(SectionKey key) {
        if(!active_||detail_||focus_!=BrowseFocus::Sidebar)return false;
        for(uint32_t i=0;i<count_;++i)if(sections_[i]==key){selected_=key;return true;}
        return false;
    }
    bool selected_query(const Query& current,Query& next) const {
        if(!active_||detail_||focus_!=BrowseFocus::Sidebar)return false;
        bool found=false;for(uint32_t i=0;i<count_;++i)if(sections_[i]==selected_)found=true;
        if(!found)return false;
        next=current;
        if(selected_.kind==SectionKind::Sort){next.sort=Sort(selected_.value);return next.sort!=current.sort;}
        next.category_id=selected_.value;return next.category_id!=current.category_id;
    }
    static bool is_active(SectionKey key,const Query& query) {
        return key.kind==SectionKind::Sort?key.value==uint32_t(query.sort):key.value==query.category_id;
    }
    bool enter_grid() {
        if(!active_||detail_)return false;
        focus_=BrowseFocus::Grid;desired_=false;return true;
    }
    bool enter_sidebar() {
        if(!active_||detail_)return false;
        focus_=BrowseFocus::Sidebar;desired_=true;return true;
    }
    bool open_detail() {
        if(!active_||detail_||focus_!=BrowseFocus::Grid)return false;
        detail_=true;return true;
    }
    bool back_from_detail() {
        if(!active_||!detail_)return false;
        detail_=false;return true; // originating grid focus/section stay intact
    }

    // Adapter requests once, binds this token to the exact native animation,
    // and calls complete only after that animation's verified finish event.
    // Reversals wait for the pending finish. No timer invents completion, and
    // repeated input cannot replay an animation each frame.
    SidebarAnimation next_animation() {
        if(!active_||detail_||pending_)return {};
        if(desired_&&phase_==SidebarPhase::Hidden){
            bump(serial_);pending_={host_,serial_,true};phase_=SidebarPhase::Opening;
        }else if(!desired_&&phase_==SidebarPhase::Shown){
            bump(serial_);pending_={host_,serial_,false};phase_=SidebarPhase::Closing;
        }
        return pending_;
    }
    bool complete(SidebarAnimation token) {
        if(!active_||!pending_||token.host!=pending_.host||token.serial!=pending_.serial||token.show!=pending_.show)return false;
        phase_=token.show?SidebarPhase::Shown:SidebarPhase::Hidden;pending_={};return true;
    }
private:
    static void bump(uint64_t& value){if(!++value)++value;}
    uint64_t host_=0,serial_=0;
    bool active_=false,detail_=false,desired_=false;
    BrowseFocus focus_=BrowseFocus::Sidebar;
    SidebarPhase phase_=SidebarPhase::Hidden;
    SidebarAnimation pending_{};
    SectionKey selected_{},sections_[MAX_SECTIONS]{};
    uint32_t count_=0;
};

}}
