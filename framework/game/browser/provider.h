#pragma once
#include "models.h"
#include "github.h"
namespace tsuchi { namespace browser {
/* Game-thread only. Owns copied descriptors/results; no callbacks run on workers.
 * Caller supplies json_bytes scratch lasting through shutdown. Results stay valid
 * until the next corresponding begin call. Internal page indices support refetch.
 * A fresh begin cancels/releases its old handle before starting a new generation.
 * Download cancellation keeps its handle until terminal acknowledgement; a
 * completed commit can win and remains Ready. shutdown releases pending work.
 * Installation delegates to Tsuchi's managed downloader and receipt publication. */
class Provider {
public:
    Provider();
    P4M_Result initialize(const P4M_HttpsApi*,const char* owner,void* scratch,uint32_t capacity);
    void shutdown();
    P4M_Result begin_batch(const Query&,uint32_t page,uint32_t generation);
    P4M_Result begin_detail(uint32_t submission_id,uint32_t generation);
    P4M_Result begin_author(uint32_t author_id,uint32_t generation);
    P4M_Result begin_categories();
    // Download counts for up to batch_size listings, in one request.
    P4M_Result begin_counts(const uint32_t* ids,uint32_t count,uint32_t generation);
    P4M_Result begin_download(const File&,const char* cache_key,uint64_t max_bytes=P4M_HTTPS_MAX_FILE,const char* replacing=nullptr);
    void pump();
    void cancel_batch(); void cancel_detail(); void cancel_author(); void cancel_categories(); void cancel_download(); void cancel_counts();
    const Batch& batch() const { return batch_; }
    const Detail& detail() const { return detail_; }
    const Categories& categories() const { return categories_; }
    const Counts& counts() const { return counts_; }
    const Operation& counts_operation() const { return counts_op_; }
    // The caller has taken the counts; the next answer is a new one.
    void finish_counts() { counts_op_.state=State::Idle; }
    const AuthorProfile& author() const { return author_; }
    const Operation& batch_operation() const { return merged_op_; }
    const Operation& detail_operation() const { return github_detail_?github_.detail_operation():detail_op_; }
    const Operation& author_operation() const { return github_author_?github_.author_operation():author_op_; }
    const Operation& categories_operation() const { return categories_op_; }
    const Download& download() const { return download_; }
private:
    GitHub github_;
    Operation merged_op_{};bool gamebanana_batch_=false,github_batch_=false,github_detail_=false,github_author_=false;
    Sort merged_sort_=Sort::Latest;
    void merge_sources();
    const P4M_HttpsApi* api_; char owner_[P4M_ID_BYTES]; char* scratch_; uint32_t scratch_capacity_;
    P4M_Transfer batch_handle_,detail_handle_,categories_handle_,download_handle_;
    Batch batch_; Detail detail_; Categories categories_; Download download_;
    Operation batch_op_,detail_op_,categories_op_;
    P4M_Transfer author_handle_; AuthorProfile author_; Operation author_op_;
    uint32_t batch_perpage_,batch_author_;
    P4M_Transfer counts_handle_; Counts counts_; Operation counts_op_;
    // A listing page is 24 records of several kilobytes each. Parsing it in one
    // call was a visible hitch, so pump() parses a few records per call. The
    // text stays in the scratch buffer meanwhile, and the other answers that
    // share that buffer wait.
    const char* parse_begin_; const char* parse_end_; uint32_t parse_pos_; bool parsing_;
    P4M_Result start(P4M_Transfer&,Operation&,const char*,const char*,uint64_t,uint32_t);
    void cancel(P4M_Transfer&,Operation&); void poll(P4M_Transfer&,Operation&,uint32_t);
};
/* Offline bounded schema entry points, also used by production pump. */
bool parse_batch(const char*,uint32_t,Batch&,uint32_t expected_perpage=batch_size,uint32_t expected_author=0);
bool parse_counts(const char*,uint32_t,Counts&);
bool counts_url(const uint32_t* ids,uint32_t count,char*,uint32_t);
bool parse_author(const char*,uint32_t,AuthorProfile&);
bool parse_detail(const char*,uint32_t,Detail&);
bool parse_categories(const char*,uint32_t,Categories&);
bool batch_url(const Query&,uint32_t,char*,uint32_t);
} }
