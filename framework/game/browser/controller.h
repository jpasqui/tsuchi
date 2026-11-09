#pragma once
#include "provider.h"
#include "images.h"
#include "install.h"
namespace tsuchi { namespace browser {
// Profile is no longer entered: an author's page is the Browse screen for
// that author's mods, with the profile shown above them. The value stays so
// the numbering of the others does not change.
enum class Screen : uint32_t { Closed, Browse, Detail, Profile, Installed };
struct BrowseSnapshot {
    uint32_t generation=0,revision=0,count=0,first_page=0,last_page=0;
    uint32_t selected_id=0,anchor_id=0,anchor_revision=0;
    float row_offset=0;
    bool loading=false,has_before=false,has_after=false;
    uint32_t failed_sources=0;
    Error error=Error::None;
};
// All entry points and borrowed records are game-thread-only. Revision changes
// invalidate borrowed records; adapter callbacks carry host/query generation.
//
// Listings, mod details, author profiles and images that have been loaded are
// kept for CACHE_MS, so going back to a section, a mod or an author shows it at
// once. retry() on a screen that has no error reloads it from the provider.
class Controller {
public:
    static constexpr uint32_t WINDOW_BATCHES=5, MAX_ITEMS=WINDOW_BATCHES*(batch_size+6);
    static constexpr uint32_t CACHED_BATCHES=24, CACHED_DETAILS=8, CACHED_AUTHORS=8;
    static constexpr uint64_t CACHE_MS=10ull*60*1000;
    // Wall-clock milliseconds, supplied every tick. Only differences are used.
    void set_time(uint64_t milliseconds){now_=milliseconds;}
    P4M_Result initialize(const P4M_HttpsApi*,const ImageBackend&);
    void attach_installer(install::Installer* installer){installer_=installer;}
    // Nonsecret per-attachment clock namespace; storage still rejects collisions.
    void set_download_session(uint64_t value){download_session_=value;}
    void shutdown();
    P4M_Result open(const Query& query=Query{});
    void close();
    P4M_Result change_query(const Query&);
    P4M_Result open_installed();
    P4M_Result open_installed_page(uint32_t local_id);
    const install::Library& installed() const {return library_;}
    const install::Record* installed_record(uint32_t local_id) const;
    bool file_installed(uint32_t submission_id,uint32_t file_id) const;
    bool file_current(const File&) const;
    bool source_installed(uint32_t submission_id) const;
    const char* update_status() const;
    bool file_retired(uint32_t submission_id,uint32_t file_id) const;
    // First activation arms an exact local package; the second confirms it.
    P4M_Result uninstall_selected(uint32_t generation);
    bool cancel_uninstall();
    bool uninstall_armed() const {return remove_candidate_!=0;}
    void pump();
    Screen screen() const {return screen_;}
    BrowseSnapshot browse() const;
    const Listing* listing(uint32_t index) const;
    const Listing* find(uint32_t id) const;
    const Query& query() const {return query_;}
    const Categories& categories() const {return provider_.categories();}
    const Operation& categories_operation() const {return provider_.categories_operation();}
    // Ignore stale native callbacks instead of selecting/rebinding new hosts.
    bool select(uint32_t id,uint32_t generation);
    bool viewport(uint32_t first_id,uint32_t last_id,float row_offset,uint32_t generation);
    P4M_Result demand(bool before,uint32_t generation);
    P4M_Result retry();
    P4M_Result open_detail(uint32_t id,uint32_t generation);
    // From a mod's page: the author's profile with their mods below it, as one
    // Browse screen whose query is that author. back() returns to the mod.
    P4M_Result open_author(uint32_t author_id,uint32_t generation);
    const AuthorProfile& author() const {return author_;}
    const Operation& author_operation() const {return provider_.author_operation();}
    // Loading until the profile is shown, whether it came from the provider or
    // from the cache. author_operation() describes the last request only.
    State author_state() const {return author_state_;}
    void back();
    const Detail& detail() const {return detail_;}
    const Operation& detail_operation() const {return provider_.detail_operation();}
    State detail_state() const {return detail_state_;}
    P4M_Result download(uint32_t file_id);
    void cancel_download();
    const Download& download_status() const {return provider_.download();}
    const install::Status& install_status() const {return install_status_;}
    bool install_pending() const {return install_pending_;}
    // Adapter calls begin_images(), then image() for visible cards / detail
    // gallery plus tested overscan, before pump_images(). Tokens are pinned by
    // the native backend; no Unreal pointer escapes to an HTTPS worker.
    void begin_images(){images_.begin_visible();}
    ImageView image(uint64_t id,const char* url,ImageKind kind){return images_.want(id,url,kind);}
    // What is already held for a URL, without asking for it.
    ImageView cached_image(const char* url,ImageKind kind) const {return images_.cached(url,kind);}
    // For images just off screen, so they are there when the view moves.
    ImageView prefetch_image(uint64_t id,const char* url,ImageKind kind){return images_.prefetch(id,url,kind);}
    // Frees every texture. The next request loads again.
    void release_images(){images_.reset(1);}
    void retry_image(uint64_t id,ImageKind kind){images_.retry(id,kind);}
    void pump_images(){images_.pump();}
    uint32_t image_bytes() const {return images_.resident_bytes();}
    uint32_t images_unsettled() const {return images_.unsettled();}
    uint32_t images_failed() const {return images_.failed();}
private:
    Provider provider_;
    Images images_;
    install::Installer* installer_=nullptr;
    install::Request install_request_{};
    install::Status install_status_{};
    bool install_pending_=false,install_started_=false;
    install::Library library_{};
    Listing installed_listings_[install::max_installed]{};
    uint32_t remove_candidate_=0,remove_revision_=0,library_install_file_=0;
    struct Retired {uint32_t submission,file;};
    Retired retired_[install::max_installed]{};uint32_t retired_count_=0;
    bool initialized_=false,consumed_=true,detail_consumed_=true,categories_due_=false,counts_failed_=false;
    uint32_t counts_ids_[batch_size]{},counts_asked_=0;
    void request_counts();
    void merge_counts(const Counts&);
    Screen screen_=Screen::Closed;
    uint32_t detail_installed_=0;
    Query query_{};
    uint32_t generation_=0,revision_=0,anchor_revision_=0;
    uint32_t selected_=0,anchor_=0,visible_last_=0;
    float row_offset_=0;
    uint32_t pages_=0,pending_page_=0,download_serial_=0;
    uint64_t download_session_=0;
    Error error_=Error::None;
    Batch pages_data_[WINDOW_BATCHES]{};
    Detail detail_{};
    AuthorProfile author_{};
    // One author excursion retains the original catalogue and exact anchor.
    // A detail within that author's catalogue returns to the profile's mods;
    // closing the profile restores the original detail under a fresh epoch.
    struct ReturnContext {
        bool active=false,from_detail=false,pending=false;
        Query query{};uint32_t pages=0,pending_page=0,selected=0,anchor=0,visible_last=0;
        float row_offset=0;Error error=Error::None;
        Batch batches[WINDOW_BATCHES]{};Detail detail{};
    } return_{};
    struct CachedBatch {bool used=false;uint64_t stored=0,touched=0;Query query{};Batch batch{};};
    struct CachedDetail {bool used=false;uint64_t stored=0,touched=0;Detail detail{};};
    struct CachedAuthor {bool used=false;uint64_t stored=0,touched=0;AuthorProfile author{};};
    CachedBatch batch_cache_[CACHED_BATCHES]{};
    CachedDetail detail_cache_[CACHED_DETAILS]{};
    CachedAuthor author_cache_[CACHED_AUTHORS]{};
    CachedBatch* cached_=nullptr; // a cached page waiting for insert() to accept it
    uint64_t now_=0,cache_clock_=0;
    State detail_state_=State::Idle,author_state_=State::Idle;
    bool fresh(uint64_t stored) const {return now_-stored<CACHE_MS;}
    CachedBatch* cached_batch(const Query&,uint32_t page);
    void store_batch(const Batch&);
    void forget_batches(const Query&);
    CachedDetail* cached_detail(uint32_t id);
    void store_detail(const Detail&);
    CachedAuthor* cached_author(uint32_t id);
    void store_author(const AuthorProfile&);
    void deliver_cached();
    P4M_Result load_detail(bool refresh);
    P4M_Result load_author(bool refresh);
    void reset_view();
    P4M_Result begin_browse(const Query&);
    void restore_return();
    alignas(16) char scratch_[json_bytes]{};
    P4M_Result request(uint32_t page,bool refresh=false);
    bool insert(const Batch&);
    void changed(){++revision_;if(!revision_)++revision_;}
    void restore_anchor();
};
Controller& controller();
}}
