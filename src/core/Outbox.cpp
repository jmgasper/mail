#include "mail/Outbox.h"
#include "mail/Sync.h"
#include <algorithm>
#include <ctime>
namespace mail {
DeliveryReport deliverQueued(Store& store,const std::function<std::string(const Account&)>& password,const std::atomic<bool>& cancelled,
    const std::function<std::unique_ptr<GmailApi>(const Account&)>& gmail)
{
    DeliveryReport report;
    while (!cancelled) {
        auto o=store.claimOutgoing(); if (!o.id) break;
        bool delivered=false;
        try {
            auto accounts=store.accounts(); auto a=std::find_if(accounts.begin(),accounts.end(),[&](const Account& a){return a.id==o.account;});
            if (a==accounts.end()) throw std::runtime_error("The sending account no longer exists.");
            GmailSent receipt;
            if (a->provider==Provider::Gmail) {
                if (!gmail) throw NetworkError(NetworkFailure::Authentication,"Sign in to the Gmail account before sending.");
                auto api=gmail(*a);
                if (!api) throw NetworkError(NetworkFailure::Authentication,"Google authorization is unavailable. Sign in again.");
                verifyGmailIdentity(*a,api->profile());
                receipt=o.gmailDraft.empty()?api->send(o.raw,o.recipients,o.providerThread)
                    :api->sendDraft(o.gmailDraft,o.raw,o.recipients,o.providerThread);
            } else sendSmtp(*a,password(*a),o.from,o.recipients,o.raw,cancelled);
            delivered=true;
            store.finishOutgoing(o.id,"sent","",std::time(nullptr),receipt); ++report.sent;
        } catch (const NetworkError& error) {
            bool uncertain=error.reason==NetworkFailure::DeliveryUnknown;
            store.finishOutgoing(o.id,uncertain?"uncertain":"failed",error.what(),std::time(nullptr));
            if (uncertain) ++report.uncertain; else ++report.failed;
        } catch (const std::exception& error) {
            // Once SMTP succeeded, a local cache failure must never authorize an
            // automatic retry. If even this write fails, startup recovery handles it.
            store.finishOutgoing(o.id,delivered?"uncertain":"failed",error.what(),std::time(nullptr));
            if (delivered) ++report.uncertain; else ++report.failed;
        }
    }
    return report;
}
}
