#pragma once
#include "Store.h"
#include <functional>

namespace mail {
struct SyncReport {
    std::size_t downloaded = 0;
    std::size_t applied = 0;
    std::vector<std::string> errors;
};
// Progress callbacks run on the worker and must marshal UI work to its looper.
SyncReport syncAccount(Store&,const Account&,const std::string& password,const std::atomic<bool>& cancelled,
    const std::function<void(const std::string&)>& progress = {});
SyncReport syncGmailAccount(Store&,const Account&,GmailApi&,const std::atomic<bool>& cancelled,
    const std::function<void(const std::string&)>& progress = {});
SyncReport syncGmailDrafts(Store&,const Account&,GmailApi&,const std::atomic<bool>& cancelled);
void verifyGmailIdentity(const Account&,const GmailProfile&);
}
