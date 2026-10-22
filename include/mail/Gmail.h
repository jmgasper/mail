#pragma once
#include "GoogleOAuth.h"

namespace mail {
struct GmailProfile { std::string email; std::string history; };
struct GmailLabel { std::string id; std::string name; bool system = false; };
struct GmailMessage {
    std::string id;
    std::string thread;
    std::string history;
    std::vector<std::string> labels;
    Timestamp received = 0;
    std::string raw; // Populated only by message(..., true).
};
struct GmailMessagePage { std::vector<std::string> ids; std::string next; };
struct GmailHistoryPage {
    std::vector<std::string> changed; // Fetch latest state; a subsequent 404 means deleted.
    std::string history;
    std::string next;
};
struct GmailSent { std::string id; std::string thread; };
struct GmailDraft { std::string id; GmailMessage message; }; // Draft ID survives replacement of message.id.
struct GmailDraftPage { std::vector<GmailDraft> drafts; std::string next; };
// Confined to a worker. The token callback supplies a valid access token and
// persists any refresh in the system key store before returning. force=true is
// used once following an explicit 401, never after an ambiguous send failure.
class GmailApi {
public:
    using AccessToken = std::function<std::string(bool force)>;
    GmailApi(AccessToken, const std::atomic<bool>& cancelled, GoogleEndpoints = {});
    GmailProfile profile();
    std::vector<GmailLabel> labels();
    GmailMessagePage messages(const std::string& page = {});
    GmailMessage message(const std::string& id, bool raw);
    GmailHistoryPage history(const std::string& since, const std::string& page = {});
    void modify(const std::string& id, const std::vector<std::string>& add, const std::vector<std::string>& remove);
    void trash(const std::string& id, bool trash);
    GmailLabel createLabel(const std::string& name);
    GmailLabel renameLabel(const std::string& id,const std::string& name);
    void deleteLabel(const std::string& id);
    GmailSent send(const std::string& raw, const std::vector<std::string>& recipients, const std::string& thread = {});
    GmailDraftPage drafts(const std::string& page={},const std::string& query={});
    GmailDraft draft(const std::string& id,bool raw=true);
    GmailDraft createDraft(const std::string& raw,const std::string& thread={});
    GmailDraft updateDraft(const std::string& id,const std::string& raw,const std::string& thread={});
    void deleteDraft(const std::string& id);
    // Always submit the immutable outbox content and envelope, even if the
    // server draft changed. Never send mutable server content by ID alone.
    GmailSent sendDraft(const std::string& id,const std::string& raw,const std::vector<std::string>& recipients,const std::string& thread={});
private:
    enum class Mutation { None,Delivery,Draft };
    std::string request(const std::string& path, const std::string& method = "GET", const std::string& body = {}, Mutation = Mutation::None);
    AccessToken access_;
    const std::atomic<bool>& cancelled_;
    GoogleEndpoints endpoints_;
};
}
