#pragma once
#include "Model.h"
#include <string_view>

namespace mail {
struct TextRun {
    std::size_t offset=0; // UTF-8 byte offset; style extends to the next run.
    bool bold=false,italic=false,underline=false;
    std::string link;
};
struct InlineImage {
    std::size_t offset=0,length=0; // UTF-8 placeholder range in the editable text.
    std::string contentId;
};
struct Draft {
    Id id=0,account=0;
    std::string messageId,to,cc,bcc,subject,text;
    std::vector<TextRun> runs;
    std::vector<std::string> references;
    std::string providerThread;
    std::vector<Attachment> attachments;
    std::vector<InlineImage> images;
    Timestamp modified=0;
    Id revision=0; // Store version. Saving an existing draft requires this version.
};
struct Mailbox { std::string name,address; };
struct Outgoing {
    Id id=0,draft=0,account=0;
    std::string state,error,from,raw,subject;
    std::vector<std::string> recipients;
    Timestamp created=0;
    std::string gmailDraft,providerThread; // Captured while claiming immutable delivery.
};
struct OutboxSummary {
    Id queued=0,sending=0,failed=0,uncertain=0;
    Id total() const { return queued+sending+failed+uncertain; }
    Id attention() const { return failed+uncertain; }
};
struct ComposedMessage {
    std::string from,raw;
    std::vector<std::string> recipients;
};
std::string newMessageId();
std::vector<Mailbox> parseAddresses(const std::string&,std::size_t nameLimit=512);
std::vector<Mailbox> parseMessageAddresses(std::string_view raw,const std::string& header);
void validateDraft(const Draft&);
std::string imagePlaceholder(const Attachment&);
std::string draftHtml(const Draft&);
ComposedMessage composeMessage(const Draft&,const Account&,Timestamp);
// Server drafts include BCC and can have no recipients. Delivery continues to
// require a valid envelope and omits BCC from its immutable local MIME copy.
std::string composeDraftMessage(const Draft&,const Account&,Timestamp);
// Import supported rich text and owned CID images into the editable model.
// Callers retain the original raw message when tracking remote revisions.
Draft parseDraftMessage(std::string_view,Id account,const std::string& providerThread={});
Draft replyDraft(const Message&,const Account&,bool all);
Draft forwardDraft(const Message&,const Account&);
}
