#include "controller.h"
namespace tsuchi { namespace browser {
namespace {
bool contains(const Batch& b,uint32_t id){if(!id)return false;for(uint32_t i=0;i<b.count;++i)if(b.listings[i].id==id)return true;return false;}
bool finite_offset(float v){return v>=-4096.f&&v<=4096.f;}
bool same_text(const char* a,const char* b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
bool same_query(const Query& a,const Query& b){return a.category_id==b.category_id&&a.sort==b.sort&&a.author_id==b.author_id&&same_text(a.search,b.search);}
// The entry to overwrite: a free one, else the one used longest ago.
template<class T,uint32_t N> T* oldest(T (&entries)[N]){
    T* out=&entries[0];
    for(auto& e:entries){if(!e.used)return &e;if(e.touched<out->touched)out=&e;}
    return out;
}
void decimal(char* dst,uint32_t& n,uint32_t value){char t[10];uint32_t k=0;do{t[k++]=char('0'+value%10);value/=10;}while(value);while(k)dst[n++]=t[--k];}
const char* extension(const char* name){
    uint32_t end=0;while(end<256&&name[end])++end;
    if(end==256)return ".bin";
    uint32_t start=end;while(start&&name[start-1]!='.')--start;
    if(!start||end-start>4)return ".bin";
    char ext[5]{};for(uint32_t i=0;i<end-start;++i){const char c=name[start+i];ext[i]=c>='A'&&c<='Z'?char(c-'A'+'a'):c;}
    const char* known[]{"zip","rar","7z","pak","ucas","utoc"};
    const char* suffix[]{".zip",".rar",".7z",".pak",".ucas",".utoc"};
    for(uint32_t i=0;i<6;++i){uint32_t k=0;while(ext[k]&&ext[k]==known[i][k])++k;if(!ext[k]&&!known[i][k])return suffix[i];}
    return ".bin";
}
}
// The native builder disables thread-safe local-static guards for this source;
// every entry point runs on the game thread and no C++ runtime is linked.
Controller& controller(){static Controller instance;return instance;}
P4M_Result Controller::initialize(const P4M_HttpsApi* api,const ImageBackend& images){
    if(initialized_)shutdown();
    const auto result=provider_.initialize(api,"p4m.downloader",scratch_,sizeof scratch_);
    if(result!=P4M_OK)return result;
    images_.attach(api,"p4m.downloader",images);images_.reset(1);initialized_=true;return P4M_OK;
}
Controller::CachedBatch* Controller::cached_batch(const Query& q,uint32_t page){
    for(auto& e:batch_cache_)if(e.used&&e.batch.page==page&&same_query(e.query,q)){
        if(!fresh(e.stored)){e.used=false;return nullptr;}
        e.touched=++cache_clock_;return &e;
    }
    return nullptr;
}
void Controller::store_batch(const Batch& batch){
    CachedBatch* e=nullptr;
    for(auto& c:batch_cache_)if(c.used&&c.batch.page==batch.page&&same_query(c.query,query_))e=&c;
    if(!e)e=oldest(batch_cache_);
    e->used=true;e->stored=now_;e->touched=++cache_clock_;e->query=query_;e->batch=batch;
}
void Controller::forget_batches(const Query& q){for(auto& e:batch_cache_)if(e.used&&same_query(e.query,q))e.used=false;}
Controller::CachedDetail* Controller::cached_detail(uint32_t id){
    for(auto& e:detail_cache_)if(e.used&&e.detail.listing.id==id){
        if(!fresh(e.stored)){e.used=false;return nullptr;}
        e.touched=++cache_clock_;return &e;
    }
    return nullptr;
}
void Controller::store_detail(const Detail& detail){
    CachedDetail* e=nullptr;
    for(auto& c:detail_cache_)if(c.used&&c.detail.listing.id==detail.listing.id)e=&c;
    if(!e)e=oldest(detail_cache_);
    e->used=true;e->stored=now_;e->touched=++cache_clock_;e->detail=detail;
}
Controller::CachedAuthor* Controller::cached_author(uint32_t id){
    for(auto& e:author_cache_)if(e.used&&e.author.id==id){
        if(!fresh(e.stored)){e.used=false;return nullptr;}
        e.touched=++cache_clock_;return &e;
    }
    return nullptr;
}
void Controller::store_author(const AuthorProfile& author){
    CachedAuthor* e=nullptr;
    for(auto& c:author_cache_)if(c.used&&c.author.id==author.id)e=&c;
    if(!e)e=oldest(author_cache_);
    e->used=true;e->stored=now_;e->touched=++cache_clock_;e->author=author;
}
void Controller::shutdown(){cancel_download();close();provider_.shutdown();images_.reset(0);initialized_=false;}
P4M_Result Controller::open(const Query& q){
    if(!initialized_)return P4M_NOT_READY;
    char url[P4M_HTTPS_URL_BYTES];if(!batch_url(q,1,url,sizeof url))return P4M_INVALID;
    close();categories_due_=true;
    return begin_browse(q);
}
void Controller::reset_view(){
    provider_.cancel_batch();provider_.cancel_detail();
    ++generation_;if(!generation_)++generation_;
    provider_.cancel_counts();counts_failed_=false;
    screen_=Screen::Closed;pages_=0;pending_page_=0;cached_=nullptr;
    selected_=anchor_=visible_last_=0;row_offset_=0;error_=Error::None;
    detail_={};detail_state_=State::Idle;consumed_=detail_consumed_=true;changed();++anchor_revision_;
    remove_candidate_=0;
    // Archive transfers survive navigation; shutdown explicitly retires them.
}
void Controller::close(){reset_view();provider_.cancel_author();return_.active=false;author_={};author_state_=State::Idle;}
P4M_Result Controller::begin_browse(const Query& q){reset_view();screen_=Screen::Browse;query_=q;return request(1);}
P4M_Result Controller::change_query(const Query& q){
    char url[P4M_HTTPS_URL_BYTES];if(!batch_url(q,1,url,sizeof url))return P4M_INVALID;
    if(return_.active&&q.author_id&&q.author_id==author_.id)return begin_browse(q);
    return open(q);
}
P4M_Result Controller::open_installed(){
    if(!initialized_||!installer_||!installer_->library_available())return P4M_UNSUPPORTED;
    reset_view();provider_.cancel_author();return_.active=false;screen_=Screen::Installed;
    // Existing transfer jobs survive navigation. Refresh starts after they end.
    installer_->refresh_library();selected_=anchor_=library_.count?1:0;changed();return P4M_OK;
}
P4M_Result Controller::open_installed_page(uint32_t id){
    const auto* record=installed_record(id);if(!record||!record->submission_id)return P4M_NOT_FOUND;
    const auto result=provider_.begin_detail(record->submission_id,generation_);if(result!=P4M_OK)return result;
    detail_={};detail_.generation=generation_;detail_.listing.id=record->submission_id;detail_.listing.source=record->source;
    for(uint32_t i=0;i<sizeof detail_.listing.source_key;++i)detail_.listing.source_key[i]=record->source_key[i];
    for(uint32_t i=0;i<sizeof detail_.listing.title;++i)detail_.listing.title[i]=record->title[i];
    detail_installed_=id;screen_=Screen::Detail;detail_consumed_=false;detail_state_=State::Loading;changed();return P4M_OK;
}
const install::Record* Controller::installed_record(uint32_t id) const {return id&&id<=library_.count&&library_.state==install::LibraryState::Ready?&library_.records[id-1]:nullptr;}
bool Controller::file_installed(uint32_t sub,uint32_t file) const {
    for(uint32_t i=0;i<library_.count;++i)if(library_.records[i].submission_id==sub&&library_.records[i].file_id==file)return true;return false;
}
bool Controller::source_installed(uint32_t sub) const {
    for(uint32_t i=0;i<library_.count;++i)if(library_.records[i].submission_id==sub)return true;return false;
}
bool Controller::file_current(const File& f) const {
    for(uint32_t i=0;i<library_.count;++i){const auto& r=library_.records[i];if(r.submission_id!=f.submission_id||r.file_id!=f.id)continue;
        if(f.digest[0]&&r.digest[0]&&!same_text(f.digest,r.digest))return false;
        if(f.updated&&r.updated&&f.updated>r.updated)return false;
        return (f.digest[0]&&r.digest[0])||(f.updated&&r.updated);
    }return false;
}
const char* Controller::update_status() const {
    if(!source_installed(detail_.listing.id))return "";
    if(detail_state_==State::Loading)return "Checking published files...";
    if(detail_state_!=State::Ready)return "Update check failed. Retry to check again.";
    bool newer=false,other=false,current=false,unknown=false;
    for(uint32_t i=0;i<detail_.file_count;++i){const auto& f=detail_.files[i];if(file_current(f)){current=true;continue;}other=true;
        for(uint32_t j=0;j<library_.count;++j){const auto& r=library_.records[j];if(r.submission_id!=f.submission_id)continue;if((r.updated&&f.updated>r.updated)||(f.id==r.file_id&&f.digest[0]&&r.digest[0]&&!same_text(f.digest,r.digest)))newer=true;
            if(f.id==r.file_id&&!(f.digest[0]&&r.digest[0])&&!(f.updated&&r.updated))unknown=true;}
    }
    if(newer)return "Newer files are available. Choose the version and variant to update.";
    if(unknown)return "The installed file has insufficient comparison metadata; update status is unknown.";
    if(other)return "Other versions or variants are available. Choose a file to compare.";
    if(detail_.files_truncated)return "The file list is incomplete; update status is unknown.";
    return current?"Your installed file is current in this published file list.":"The installed file is no longer listed. Update status is unknown.";
}
bool Controller::file_retired(uint32_t sub,uint32_t file) const {for(uint32_t i=0;i<retired_count_;++i)if(retired_[i].submission==sub&&retired_[i].file==file)return true;return false;}
bool Controller::cancel_uninstall(){const bool was=remove_candidate_!=0;remove_candidate_=0;changed();return was;}
P4M_Result Controller::uninstall_selected(uint32_t gen){
    if(screen_!=Screen::Installed||gen!=generation_)return P4M_NOT_READY;
    const auto*r=installed_record(selected_);if(!r)return P4M_NOT_READY;
    if(install_pending_||provider_.download().operation.state==State::Loading||(install_status_.state>=install::State::Queued&&install_status_.state<=install::State::Committing))return P4M_BUSY;
    if(remove_candidate_!=selected_||remove_revision_!=library_.revision){remove_candidate_=selected_;remove_revision_=library_.revision;changed();return P4M_OK;}
    const auto result=installer_->uninstall(r->submission_id,r->file_id);if(result==P4M_OK){remove_candidate_=0;changed();}return result;
}
P4M_Result Controller::request(uint32_t page,bool refresh){
    pending_page_=page;cached_=refresh?nullptr:cached_batch(query_,page);
    if(cached_){provider_.cancel_batch();consumed_=false;error_=Error::None;changed();deliver_cached();return P4M_OK;}
    const auto result=provider_.begin_batch(query_,page,generation_);
    consumed_=result!=P4M_OK;
    error_=result==P4M_OK?Error::None:Error::Api;changed();return result;
}
// insert() can decline while the page it would retire is still on screen; the
// cached page then waits here exactly as a completed request does.
void Controller::deliver_cached(){
    cached_->batch.generation=generation_;
    if(insert(cached_->batch)){consumed_=true;cached_=nullptr;changed();}
}
const Listing* Controller::listing(uint32_t index) const {
    if(screen_==Screen::Installed)return index<library_.count?&installed_listings_[index]:nullptr;
    for(uint32_t p=0;p<pages_;++p){if(index<pages_data_[p].count)return &pages_data_[p].listings[index];index-=pages_data_[p].count;}
    return nullptr;
}
const Listing* Controller::find(uint32_t id) const {
    if(screen_==Screen::Installed)return id&&id<=library_.count?&installed_listings_[id-1]:nullptr;
    for(uint32_t p=0;p<pages_;++p)for(uint32_t i=0;i<pages_data_[p].count;++i)if(pages_data_[p].listings[i].id==id)return &pages_data_[p].listings[i];
    return nullptr;
}
BrowseSnapshot Controller::browse() const {
    BrowseSnapshot v{};v.generation=generation_;v.revision=revision_;v.anchor_revision=anchor_revision_;
    v.selected_id=selected_;v.anchor_id=anchor_;v.row_offset=row_offset_;v.error=error_;
    if(screen_==Screen::Installed){v.count=library_.count;v.loading=library_.state==install::LibraryState::Queued||library_.state==install::LibraryState::Loading;
        v.error=library_.state==install::LibraryState::Failed?Error::Unavailable:Error::None;return v;}
    for(uint32_t p=0;p<pages_;++p){v.count+=pages_data_[p].count;v.failed_sources|=pages_data_[p].failed_sources;}
    if(pages_){v.first_page=pages_data_[0].page;v.last_page=pages_data_[pages_-1].page;v.has_before=v.first_page>1;v.has_after=pages_data_[pages_-1].has_more;}
    v.loading=!consumed_;return v;
}
bool Controller::select(uint32_t id,uint32_t gen){
    if(gen!=generation_||(screen_!=Screen::Browse&&screen_!=Screen::Installed)||!find(id))return false;
    if(selected_!=id){selected_=id;remove_candidate_=0;changed();}return true;
}
bool Controller::viewport(uint32_t first,uint32_t last,float offset,uint32_t gen){
    if(gen!=generation_||(screen_!=Screen::Browse&&screen_!=Screen::Installed)||!find(first)||!find(last)||!finite_offset(offset))return false;
    anchor_=first;visible_last_=last;row_offset_=offset;return true;
}
P4M_Result Controller::demand(bool before,uint32_t gen){
    if(gen!=generation_||screen_!=Screen::Browse)return P4M_NOT_READY;
    if(!consumed_)return P4M_BUSY;
    if(error_!=Error::None)return P4M_NOT_READY; // explicit retry, never one request/frame
    if(!pages_)return P4M_NOT_FOUND;
    if(before)return pages_data_[0].page>1?request(pages_data_[0].page-1):P4M_NOT_FOUND;
    const auto& end=pages_data_[pages_-1];
    return end.has_more&&end.page!=UINT32_MAX?request(end.page+1):P4M_NOT_FOUND;
}
P4M_Result Controller::retry(){
    if(!initialized_||screen_==Screen::Closed)return P4M_NOT_READY;
    if(screen_==Screen::Installed)return installer_?installer_->refresh_library():P4M_UNSUPPORTED;

    if(screen_==Screen::Detail){
        images_.retry(detail_.listing.id,ImageKind::Preview);
        images_.retry(detail_.listing.id,ImageKind::Thumbnail);
        images_.retry((1ull<<32)|detail_.listing.author_id,ImageKind::Thumbnail);
        return load_detail(true);
    }
    if(!consumed_)return P4M_BUSY;
    for(uint32_t i=0;i<browse().count;++i)if(const auto* item=listing(i)){images_.retry(item->id,ImageKind::Card);images_.retry(item->id,ImageKind::Thumbnail);}
    counts_failed_=false;
    // Avatars are requested under a distinct synthetic ID; retrying the
    // listing alone would leave a failed profile or avatar failed.
    const bool author_page=query_.author_id&&return_.active;
    if(author_page)images_.retry((1ull<<32)|author_.id,ImageKind::Thumbnail);
    const bool reload_author=author_page&&author_state_!=State::Ready;
    // A failed page is asked for again. With nothing failed this is a refresh:
    // the section is read from the provider again, from its first page.
    P4M_Result result;
    if(error_!=Error::None)result=request(pending_page_?pending_page_:1,true);
    else {forget_batches(query_);const Query q=query_;result=begin_browse(q);}
    // After the listing: a refresh starts a new generation, and the profile
    // answer has to belong to it.
    if(reload_author)load_author(true);
    return result;
}
P4M_Result Controller::load_detail(bool refresh){
    if(!refresh)if(const auto* hit=cached_detail(detail_.listing.id)){
        provider_.cancel_detail();detail_=hit->detail;detail_.generation=generation_;
        detail_consumed_=true;detail_state_=State::Ready;changed();return P4M_OK;
    }
    const auto r=provider_.begin_detail(detail_.listing.id,generation_);
    detail_consumed_=r!=P4M_OK;detail_state_=r==P4M_OK?State::Loading:State::Failed;changed();return r;
}
P4M_Result Controller::load_author(bool refresh){
    if(!refresh)if(const auto* hit=cached_author(author_.id)){
        provider_.cancel_author();author_=hit->author;author_state_=State::Ready;changed();return P4M_OK;
    }
    const auto r=provider_.begin_author(author_.id,generation_);
    author_state_=r==P4M_OK?State::Loading:State::Failed;changed();return r;
}
void Controller::restore_anchor(){
    if(!find(anchor_)){const auto* first=listing(0);anchor_=first?first->id:0;row_offset_=0;}
    if(!find(selected_))selected_=anchor_;
    if(!find(visible_last_))visible_last_=anchor_;
    ++anchor_revision_;changed();
}
bool Controller::insert(const Batch& incoming){
    // Identity and shape are validated again at the controller boundary.
    if(incoming.generation!=generation_||incoming.page!=pending_page_||incoming.count>batch_size+6){error_=Error::Schema;return true;}
    const bool before=pages_&&incoming.page<pages_data_[0].page;
    if(pages_&&incoming.page!=(before?pages_data_[0].page-1:pages_data_[pages_-1].page+1)){error_=Error::Schema;return true;}
    if(pages_==WINDOW_BATCHES){
        const auto& victim=pages_data_[before?pages_-1:0];
        // Native viewport/selection must advance before its GC-visible models
        // can be retired. Hold completed data while a protected end is visible.
        if(contains(victim,anchor_)||contains(victim,visible_last_)||contains(victim,selected_))return false;
    }
    uint32_t unique=0;
    for(uint32_t i=0;i<incoming.count;++i){
        const auto id=incoming.listings[i].id;if(!id){error_=Error::Schema;return true;}
        bool seen=find(id)!=nullptr;
        for(uint32_t j=0;j<i;++j)if(incoming.listings[j].id==id)seen=true;
        if(!seen)++unique;
    }
    if(!unique&&pages_){
        if(!before)pages_data_[pages_-1].has_more=false;
        else error_=Error::Schema; // changed provider ordering; preserve current window
        changed();return true;
    }
    if(pages_==WINDOW_BATCHES){
        if(!before)for(uint32_t i=1;i<pages_;++i)pages_data_[i-1]=pages_data_[i];
        --pages_;
    }
    // Dedupe against the retained window before mutating its ordinal mapping.
    // In-place output lives in the free end slot, never on the native stack.
    const uint32_t target=pages_;
    auto& out=pages_data_[target];out.generation=generation_;out.page=incoming.page;out.total=incoming.total;out.has_more=incoming.has_more;out.count=0;out.failed_sources=incoming.failed_sources;
    for(uint32_t i=0;i<incoming.count;++i){
        const auto& item=incoming.listings[i];bool seen=find(item.id)!=nullptr;
        for(uint32_t j=0;j<out.count;++j)if(out.listings[j].id==item.id)seen=true;
        if(!seen)out.listings[out.count++]=item;
    }
    if(!out.count)out.has_more=false;
    ++pages_;
    if(before){
        // Swap listing records with a small field-sized scratch is still large;
        // use the provider's immutable result as the final front-page source.
        // Move complete batches backwards, then reconstruct front duplicates.
        for(uint32_t i=pages_-1;i>0;--i)pages_data_[i]=pages_data_[i-1];
        auto& front=pages_data_[0];front.generation=generation_;front.page=incoming.page;front.total=incoming.total;front.has_more=incoming.has_more;front.count=0;front.failed_sources=incoming.failed_sources;
        for(uint32_t i=0;i<incoming.count;++i){
            const auto& item=incoming.listings[i];bool seen=false;
            for(uint32_t p=1;p<pages_;++p)if(contains(pages_data_[p],item.id))seen=true;
            for(uint32_t j=0;j<front.count;++j)if(front.listings[j].id==item.id)seen=true;
            if(!seen)front.listings[front.count++]=item;
        }
    }
    error_=Error::None;restore_anchor();return true;
}
void Controller::pump(){
    if(!initialized_)return;
    provider_.pump();
    if(installer_&&installer_->library_available()){
        const uint32_t previous=library_.revision;
        if(installer_->library(library_)&&library_.revision!=previous){
            remove_candidate_=0;
            if(library_.removed_file&&!file_retired(library_.removed_submission,library_.removed_file)&&retired_count_<install::max_installed)retired_[retired_count_++]={library_.removed_submission,library_.removed_file};
            if(library_.state==install::LibraryState::Ready){
                for(uint32_t i=0;i<library_.count;++i){auto&out=installed_listings_[i];const auto&r=library_.records[i];out={};out.id=i+1;
                    for(uint32_t k=0;k<sizeof(out.title);++k){out.title[k]=r.title[k];if(!r.title[k])break;}
                    for(uint32_t k=0;k<sizeof(r.thumbnail)&&k+1<sizeof(out.image.thumbnail);++k){out.image.thumbnail[k]=r.thumbnail[k];if(!r.thumbnail[k])break;}}
                if(screen_==Screen::Installed){++generation_;if(!generation_)++generation_;selected_=anchor_=library_.count?1:0;row_offset_=0;++anchor_revision_;}
            }changed();
        }
        if(library_.state==install::LibraryState::Idle)installer_->refresh_library();
    }
    if(install_started_&&installer_){
        install::Status value{};
        if(installer_->status(value)&&value.submission_id==install_request_.submission_id&&value.file_id==install_request_.file_id){install_status_=value;
            if(installer_->library_available()&&(value.state==install::State::Installed||value.error==install::Error::Exists)&&library_install_file_!=value.file_id&&installer_->refresh_library()==P4M_OK)library_install_file_=value.file_id;}
    }
    if(author_state_==State::Loading){
        const auto& op=provider_.author_operation();
        if(op.generation==generation_&&op.state==State::Ready&&provider_.author().id==author_.id){
            author_=provider_.author();author_state_=State::Ready;store_author(author_);changed();
        }else if(op.generation==generation_&&(op.state==State::Failed||op.state==State::Cancelled)){author_state_=State::Failed;changed();}
    }
    if(screen_!=Screen::Closed&&!consumed_){
        if(cached_)deliver_cached();
        else {
            const auto& op=provider_.batch_operation();
            if(op.generation==generation_){
                if(op.state==State::Ready){if(insert(provider_.batch())){consumed_=true;if(error_==Error::None)store_batch(provider_.batch());changed();}}
                else if(op.state==State::Failed||op.state==State::Cancelled){consumed_=true;error_=op.error==Error::None?Error::Transport:op.error;changed();}
            }
        }
    }
    // Download counts are not part of a listing. They are asked for one page
    // of listings at a time, after the listing itself, and filled in when they
    // arrive; a card shows its other figures meanwhile.
    if(screen_==Screen::Browse&&consumed_&&!counts_failed_){
        const auto& op=provider_.counts_operation();
        if(op.state==State::Ready&&op.generation==generation_){merge_counts(provider_.counts());provider_.finish_counts();}
        else if(op.state==State::Failed&&op.generation==generation_)counts_failed_=true;
        if(provider_.counts_operation().state!=State::Loading&&!counts_failed_)request_counts();
    }
    // The category list does not depend on the query, so it is asked for once,
    // and only after the first listing has arrived: the listing is what the
    // page is waiting for, and the category request then continues on the
    // connection the listing opened instead of opening a second one beside it.
    if(categories_due_&&screen_!=Screen::Closed&&consumed_){
        categories_due_=false;const auto state=provider_.categories_operation().state;
        if(state!=State::Ready&&state!=State::Loading)provider_.begin_categories();
    }
    if(screen_==Screen::Detail&&!detail_consumed_){
        const auto& op=provider_.detail_operation();
        if(op.generation==generation_&&op.state==State::Ready){
            const auto& result=provider_.detail();
            if(result.generation==generation_&&result.listing.id==detail_.listing.id){
                detail_=result;detail_consumed_=true;detail_state_=State::Ready;store_detail(detail_);changed();
            }
        }else if(op.generation==generation_&&(op.state==State::Failed||op.state==State::Cancelled)){detail_consumed_=true;detail_state_=State::Failed;changed();}
    }
}
P4M_Result Controller::open_detail(uint32_t id,uint32_t gen){
    if(gen!=generation_||screen_!=Screen::Browse)return P4M_NOT_READY;
    const auto* item=find(id);if(!item)return P4M_NOT_FOUND;
    const bool cached=cached_detail(id)!=nullptr;
    if(!cached){const auto result=provider_.begin_detail(id,generation_);if(result!=P4M_OK)return result;}
    // The listing fills the page while the full record is on its way.
    detail_installed_=0;selected_=id;detail_={};detail_.generation=generation_;detail_.listing=*item;
    screen_=Screen::Detail;detail_consumed_=false;detail_state_=State::Loading;changed();
    return cached?load_detail(false):P4M_OK;
}
void Controller::request_counts(){
    counts_asked_=0;
    for(uint32_t p=0;p<pages_&&counts_asked_<batch_size;++p)for(uint32_t i=0;i<pages_data_[p].count&&counts_asked_<batch_size;++i)
        if(pages_data_[p].listings[i].source==1&&!(pages_data_[p].listings[i].statistics&Downloads))counts_ids_[counts_asked_++]=pages_data_[p].listings[i].id;
    if(counts_asked_&&provider_.begin_counts(counts_ids_,counts_asked_,generation_)!=P4M_OK)counts_failed_=true;
}
void Controller::merge_counts(const Counts& counts){
    // Every listing that was asked about is settled, so nothing is asked twice:
    // one the provider did not answer for keeps a count of zero.
    auto apply=[&](Listing& item){
        if(item.statistics&Downloads)return;
        bool asked=false;for(uint32_t i=0;i<counts_asked_;++i)if(counts_ids_[i]==item.id)asked=true;
        if(!asked)return;
        for(uint32_t i=0;i<counts.count;++i)if(counts.records[i].id==item.id)item.downloads=counts.records[i].downloads;
        item.statistics|=Downloads;
    };
    for(uint32_t p=0;p<pages_;++p)for(uint32_t i=0;i<pages_data_[p].count;++i)apply(pages_data_[p].listings[i]);
    for(auto& entry:batch_cache_)if(entry.used)for(uint32_t i=0;i<entry.batch.count;++i)apply(entry.batch.listings[i]);
    counts_asked_=0;changed();
}
P4M_Result Controller::open_author(uint32_t id,uint32_t gen){
    if(!initialized_||gen!=generation_||screen_!=Screen::Detail)return P4M_NOT_READY;
    if(!id||id!=detail_.listing.author_id)return P4M_NOT_FOUND;
    // Author-filtered pages are validated by the provider. Reopening the same
    // author from one of their mods does not overwrite the original context.
    if(return_.active&&id!=author_.id)return P4M_UNSUPPORTED;
    if(!return_.active){
        return_.active=true;return_.from_detail=true;return_.query=query_;return_.pages=pages_;
        return_.pending=!consumed_;return_.pending_page=pending_page_;return_.selected=selected_;
        return_.anchor=anchor_;return_.visible_last=visible_last_;return_.row_offset=row_offset_;return_.error=error_;
        for(uint32_t i=0;i<pages_;++i)return_.batches[i]=pages_data_[i];return_.detail=detail_;
    }
    AuthorProfile seed{};seed.id=id;
    for(uint32_t i=0;i<sizeof seed.name;++i)seed.name[i]=detail_.listing.author[i];
    for(uint32_t i=0;i<sizeof seed.avatar_url;++i)seed.avatar_url[i]=detail_.listing.author_avatar_url[i];
    // One page: the listing of the author's mods, with the profile above it.
    // A failed profile request leaves the name and picture the mod supplied.
    Query q{};q.author_id=id;const auto listing=begin_browse(q);
    author_=seed;load_author(false);changed();
    return listing;
}
void Controller::restore_return(){
    reset_view();query_=return_.query;pages_=return_.pages;selected_=return_.selected;anchor_=return_.anchor;
    visible_last_=return_.visible_last;row_offset_=return_.row_offset;error_=return_.error;pending_page_=return_.pending_page;
    for(uint32_t i=0;i<pages_;++i){pages_data_[i]=return_.batches[i];pages_data_[i].generation=generation_;}
    screen_=Screen::Browse;restore_anchor();
    if(return_.pending||!pages_)request(return_.pending_page?return_.pending_page:1);
    if(return_.from_detail){detail_=return_.detail;detail_.generation=generation_;screen_=Screen::Detail;load_detail(false);}
    return_.active=false;provider_.cancel_author();author_={};author_state_=State::Idle;
}
void Controller::back(){
    if(screen_==Screen::Detail){provider_.cancel_detail();detail_consumed_=true;detail_state_=State::Idle;screen_=detail_installed_?Screen::Installed:Screen::Browse;if(detail_installed_){selected_=anchor_=detail_installed_;detail_installed_=0;}restore_anchor();}
    else if(screen_==Screen::Browse){
        if(query_.author_id&&return_.active)restore_return();
        else close();
    }
}
P4M_Result Controller::download(uint32_t id){
    if(screen_!=Screen::Detail||!detail_consumed_||detail_state_!=State::Ready)return P4M_NOT_READY;
    if(provider_.download().operation.state==State::Loading)return P4M_BUSY;
    if(install_pending_||install_status_.state==install::State::Queued||install_status_.state==install::State::Validating||install_status_.state==install::State::Extracting||install_status_.state==install::State::Committing)return P4M_BUSY;
    for(uint32_t i=0;i<detail_.file_count;++i){const auto& file=detail_.files[i];if(file.id!=id)continue;
        if(file_retired(file.submission_id,file.id))return P4M_BUSY;
        if(file_current(file))return P4M_OK;
        // Stable IDs plus per-session attempt avoid overwriting prior complete
        // downloads. The native storage layer still rejects an existing key.
        char key[P4M_HTTPS_PATH_BYTES]="downloads/gb-";uint32_t n=13;
        decimal(key,n,file.submission_id);key[n++]='-';decimal(key,n,file.id);key[n++]='-';
        if(download_session_){
            char digits[20];uint32_t count=0;uint64_t value=download_session_;
            do{digits[count++]=char('0'+value%10);value/=10;}while(value);
            while(count)key[n++]=digits[--count];key[n++]='-';
        }
        ++download_serial_;if(!download_serial_)++download_serial_;decimal(key,n,download_serial_);
        const char* suffix=extension(file.name);for(uint32_t k=0;suffix[k];++k)key[n++]=suffix[k];key[n]=0;
        const auto* installed=detail_installed_?installed_record(detail_installed_):nullptr;
        const auto r=provider_.begin_download(file,key,P4M_HTTPS_MAX_FILE,installed?installed->mod_id:nullptr);
        if(r!=P4M_OK){install_started_=false;install_status_={};install_status_.submission_id=file.submission_id;install_status_.file_id=file.id;install_status_.state=install::State::Failed;install_status_.error=r==P4M_INVALID?install::Error::Unsupported:install::Error::Unavailable;changed();return r;}
        install_request_={};install_status_={};install_started_=true;
        install_request_.submission_id=install_status_.submission_id=file.submission_id;
        install_request_.file_id=install_status_.file_id=file.id;
        const auto copy=[](char*out,uint32_t cap,const char*text){uint32_t k=0;while(text[k]&&k+1<cap){out[k]=text[k];++k;}out[k]=0;};
        copy(install_request_.title,sizeof(install_request_.title),detail_.listing.title);copy(install_request_.filename,sizeof(install_request_.filename),file.name);
        copy(install_request_.thumbnail,sizeof(install_request_.thumbnail),detail_.listing.image.thumbnail);
        for(uint32_t k=0;k<=n;++k)install_request_.cache_key[k]=key[k];
        install_pending_=false;
        return r;
    }
    return P4M_NOT_FOUND;
}
void Controller::cancel_download(){
    provider_.cancel_download();
    if(install_pending_){install_pending_=false;install_status_.state=install::State::Cancelled;install_status_.error=install::Error::Cancelled;}
    if(install_started_&&installer_)installer_->cancel();
}
}}
