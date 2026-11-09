#pragma once
#include "models.h"
namespace tsuchi::browser {
class GitHub {
public:
 void initialize(const P4M_HttpsApi* api,const char* owner){api_=api;owner_=owner;}
 P4M_Result search(const Query&,uint32_t page,uint32_t generation);
 P4M_Result detail(uint32_t id,uint32_t generation);
 P4M_Result author(uint32_t id,uint32_t generation);
 void cancel_search();void cancel_detail();void cancel_author();void pump();
 const Operation& search_operation()const{return search_op_;}
 const Operation& detail_operation()const{return detail_op_;}
 const Operation& author_operation()const{return author_op_;}
 const Batch& batch()const{return batch_;}
 const Detail& mod()const{return detail_;}
 const AuthorProfile& profile()const{return author_;}
private:
 const P4M_HttpsApi* api_=nullptr;const char* owner_=nullptr;
 P4M_Transfer search_handle_=0,detail_handle_=0,author_handle_=0;
 Operation search_op_{},detail_op_{},author_op_{};
 Batch batch_{};Detail detail_{};AuthorProfile author_{};
 bool release_stage_=false;uint32_t author_filter_=0;
 char scratch_[1u<<20]{};
 P4M_Result start(P4M_Transfer&,Operation&,const char*,uint32_t);
 void cancel(P4M_Transfer&,Operation&);
 bool response(P4M_Transfer&,Operation&,uint32_t&);
};
}
