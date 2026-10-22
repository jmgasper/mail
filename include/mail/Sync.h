#pragma once
#include "Store.h"
#include <functional>

namespace mail {
struct SyncReport {
    std::size_t downloaded = 0;
    std::size_t applied = 0;
    std::vector<std::string> errors;
};
// What a mail check is doing. step/steps count folders (or Gmail list pages);
// done/total count messages to download in the current step, total 0 when unknown.
struct SyncProgress {
    std::string text;
    std::size_t step = 0, steps = 0;
    std::size_t done = 0, total = 0;
};
using SyncProgressFunction = std::function<void(const SyncProgress&)>;
// Progress callbacks run on the worker and must marshal UI work to its looper.
SyncReport syncAccount(Store&,const Account&,const std::string& password,const std::atomic<bool>& cancelled,
    const SyncProgressFunction& progress = {});
SyncReport syncGmailAccount(Store&,const Account&,GmailApi&,const std::atomic<bool>& cancelled,
    const SyncProgressFunction& progress = {});
SyncReport syncGmailDrafts(Store&,const Account&,GmailApi&,const std::atomic<bool>& cancelled);
void verifyGmailIdentity(const Account&,const GmailProfile&);
}
