#include "mail/Gmail.h"
#include "mail/Compose.h"
#include "mail/Mime.h"
#include "GoogleJson.h"
#include <algorithm>
#include <set>
#include <limits>

namespace mail {
namespace {
using google::Json;
void identifier(const std::string& value)
{
    if (value.empty() || value.size() > 256 || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        throw NetworkError(NetworkFailure::Protocol, "Gmail returned an invalid identifier.");
}
std::string idField(const Json& value, const char* key)
{ auto result = google::string(value, key, 256); identifier(result); return result; }
std::string historyField(const Json& value, bool required = true)
{
    auto result = google::string(value, "historyId", 32, required);
    if ((!result.empty() && result.find_first_not_of("0123456789") != std::string::npos) || (required && result.empty()))
        throw NetworkError(NetworkFailure::Protocol, "Gmail returned an invalid history identifier.");
    return result;
}
const Json& array(const Json& value, const char* key, std::size_t limit)
{
    static const Json empty = Json::array(); auto found = value.find(key);
    if (found == value.end()) return empty;
    if (!found->is_array() || found->size() > limit)
        throw NetworkError(NetworkFailure::Protocol, "Gmail returned an invalid list.");
    return *found;
}
GmailLabel label(const Json& value)
{
    auto type = google::string(value, "type", 32);
    if (type != "system" && type != "user") throw NetworkError(NetworkFailure::Protocol, "Gmail returned an invalid label type.");
    return {idField(value, "id"), google::string(value, "name", 1024), type == "system"};
}
GmailMessage messageValue(const Json& value,bool raw)
{
    GmailMessage result; result.id = idField(value, "id"); result.thread = idField(value, "threadId");
    result.history = historyField(value, false);
    for (const auto& entry : array(value, "labelIds", 10000)) {
        if (!entry.is_string()) throw NetworkError(NetworkFailure::Protocol, "Gmail returned an invalid message label.");
        auto name = entry.get<std::string>(); identifier(name); result.labels.push_back(std::move(name));
    }
    auto date = google::string(value, "internalDate", 20, false);
    if (!date.empty()) {
        std::uint64_t milliseconds = 0;
        for (char c : date) {
            if (c < '0' || c > '9' || milliseconds > (std::numeric_limits<std::uint64_t>::max() - (c - '0')) / 10)
                throw NetworkError(NetworkFailure::Protocol, "Gmail returned an invalid message date.");
            milliseconds = milliseconds * 10 + c - '0';
        }
        result.received = static_cast<Timestamp>(milliseconds / 1000);
    }
    if (raw) result.raw = base64UrlDecode(google::string(value, "raw", 48 * 1024 * 1024));
    return result;
}
GmailDraft draftValue(const Json& value,bool raw)
{
    auto message=value.find("message");
    if(message==value.end() || !message->is_object()) throw NetworkError(NetworkFailure::Protocol,"Gmail returned an invalid draft message.");
    return {idField(value,"id"),messageValue(*message,raw)};
}
Json draftBody(const std::string& raw,const std::string& thread)
{
    auto end=raw.find("\r\n\r\n");
    if(raw.size()>32*1024*1024 || end==std::string::npos || end==0 || end>65536)
        throw NetworkError(NetworkFailure::Protocol,"The Gmail draft has invalid or oversized MIME content.");
    if(!thread.empty()) identifier(thread);
    Json message{{"raw",base64UrlEncode(raw)}};
    if(!thread.empty()) message["threadId"]=thread;
    return Json{{"message",std::move(message)}};
}
std::string lower(std::string value)
{ std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value; }
std::string submission(const std::string& raw, const std::vector<std::string>& recipients)
{
    if (raw.size() > 32 * 1024 * 1024 || recipients.empty() || recipients.size() > 100)
        throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail message exceeds Mail's limits.");
    auto end = raw.find("\r\n\r\n");
    if (end == std::string::npos || end > 65536)
        throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail message has invalid headers.");
    // The immutable outbox copy has no BCC header. Only the API submission
    // receives a temporary BCC header, because Gmail uses headers as its envelope.
    std::set<std::string> recipientHeaders;
    bool haveHeader = false;
    for (std::size_t start = 0; start < end;) {
        auto line = raw.find("\r\n", start);
        if (raw[start] == ' ' || raw[start] == '\t') {
            if (!haveHeader) throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail message has an invalid folded header.");
        } else {
            auto colon = raw.find(':', start);
            if (colon >= line || colon == start) throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail message has an invalid header.");
            auto name = lower(raw.substr(start, colon - start));
            if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
                throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail message has an invalid header name.");
            if (name == "bcc") throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail cache contains an unexpected BCC header.");
            if ((name == "to" || name == "cc") && !recipientHeaders.insert(name).second)
                throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail message has duplicate recipient headers.");
            haveHeader = true;
        }
        for (auto i = start; i < line; ++i) {
            auto c = static_cast<unsigned char>(raw[i]);
            if ((c < 32 && c != '\t') || c == 127) throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail message has invalid header characters.");
        }
        start = line + 2;
    }
    std::set<std::string> visible, envelope;
    for (const auto* field : {"to","cc"})
        for (const auto& mailbox : parseMessageAddresses(raw,field)) visible.insert(lower(mailbox.address));
    std::string hidden;
    for (const auto& recipient : recipients) {
        auto parsed = parseAddresses(recipient);
        if (parsed.size() != 1 || parsed[0].address != recipient)
            throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail envelope contains an invalid address.");
        if (!envelope.insert(lower(recipient)).second) continue;
        if (!visible.count(lower(recipient))) {
            if (!hidden.empty()) hidden += ",\r\n ";
            hidden += recipient;
        }
    }
    for (const auto& recipient : visible) if (!envelope.count(recipient))
        throw NetworkError(NetworkFailure::Protocol, "The outgoing Gmail headers do not match the saved recipients.");
    return hidden.empty() ? raw : "Bcc: " + hidden + "\r\n" + raw;
}
}
GmailApi::GmailApi(AccessToken access, const std::atomic<bool>& cancelled, GoogleEndpoints endpoints)
    : access_(std::move(access)), cancelled_(cancelled), endpoints_(std::move(endpoints)) {}
std::string GmailApi::request(const std::string& path, const std::string& method, const std::string& body, Mutation mutation)
{
    const bool delivery=mutation!=Mutation::None;
    try {
        HttpRequest request; request.url = endpoints_.gmail + path; request.method = method;
        request.body = body; request.certificate = endpoints_.certificate; request.delivery = delivery;
        if (method != "GET") request.contentType = "application/json";
        request.bearer = access_(false); google::opaque(request.bearer, "Google access token");
        auto response = requestHttps(request, cancelled_);
        if (response.status == 401) {
            request.bearer = access_(true); google::opaque(request.bearer, "Google access token");
            response = requestHttps(request, cancelled_);
        }
        requireHttpSuccess(response, delivery); return response.body;
    } catch (const NetworkError& error) {
        if (mutation==Mutation::Draft && error.reason==NetworkFailure::DeliveryUnknown)
            throw NetworkError(NetworkFailure::CopyUnknown,"Gmail may have saved this draft. Check Drafts before saving it again.");
        throw;
    }
}
GmailProfile GmailApi::profile()
{
    auto value = google::parse(request("/profile"));
    auto email = google::string(value, "emailAddress", 320);
    auto parsed = parseAddresses(email);
    if (parsed.size() != 1 || parsed[0].address != email)
        throw NetworkError(NetworkFailure::Protocol, "Google returned an invalid account address.");
    return {email, historyField(value)};
}
std::vector<GmailLabel> GmailApi::labels()
{
    auto value = google::parse(request("/labels")); std::vector<GmailLabel> result;
    for (const auto& entry : array(value, "labels", 10000)) result.push_back(label(entry));
    return result;
}
GmailMessagePage GmailApi::messages(const std::string& page)
{
    if (page.size() > 8192) throw NetworkError(NetworkFailure::Protocol, "Invalid Gmail page token.");
    auto path = "/messages?includeSpamTrash=true&maxResults=500";
    auto value = google::parse(request(std::string(path) + (page.empty() ? "" : "&pageToken=" + urlEncode(page))));
    GmailMessagePage result; result.next = google::string(value, "nextPageToken", 8192, false);
    for (const auto& entry : array(value, "messages", 500)) result.ids.push_back(idField(entry, "id"));
    return result;
}
GmailMessage GmailApi::message(const std::string& id, bool raw)
{
    identifier(id);
    auto value = google::parse(request("/messages/" + urlEncode(id) + "?format=" + (raw ? "raw" : "minimal")));
    auto result=messageValue(value,raw);
    if (result.id != id) throw NetworkError(NetworkFailure::Protocol, "Gmail returned a different message than requested.");
    return result;
}
GmailHistoryPage GmailApi::history(const std::string& since, const std::string& page)
{
    if (since.empty() || since.size() > 32 || since.find_first_not_of("0123456789") != std::string::npos || page.size() > 8192)
        throw NetworkError(NetworkFailure::Protocol, "Invalid Gmail history cursor.");
    auto value = google::parse(request("/history?startHistoryId=" + urlEncode(since) + "&maxResults=500"
        + (page.empty() ? "" : "&pageToken=" + urlEncode(page))));
    GmailHistoryPage result; result.history = historyField(value); result.next = google::string(value, "nextPageToken", 8192, false);
    std::set<std::string> ids;
    for (const auto& entry : array(value, "history", 500)) {
        for (const auto& message : array(entry, "messages", 100000)) ids.insert(idField(message, "id"));
        for (const auto* kind : {"messagesAdded", "messagesDeleted", "labelsAdded", "labelsRemoved"}) {
            for (const auto& change : array(entry, kind, 100000)) {
                auto message = change.find("message");
                if (message == change.end() || !message->is_object()) throw NetworkError(NetworkFailure::Protocol, "Gmail returned an invalid history entry.");
                ids.insert(idField(*message, "id"));
            }
        }
    }
    result.changed.assign(ids.begin(), ids.end()); return result;
}
void GmailApi::modify(const std::string& id, const std::vector<std::string>& add, const std::vector<std::string>& remove)
{
    identifier(id);
    if (add.size() > 100 || remove.size() > 100) throw NetworkError(NetworkFailure::Protocol, "Too many Gmail label changes in one request.");
    for (const auto& labels : {add, remove}) for (const auto& label : labels) identifier(label);
    request("/messages/" + urlEncode(id) + "/modify", "POST", Json{{"addLabelIds", add}, {"removeLabelIds", remove}}.dump());
}
void GmailApi::trash(const std::string& id, bool trash)
{ identifier(id); request("/messages/" + urlEncode(id) + (trash ? "/trash" : "/untrash"), "POST", "{}"); }
GmailLabel GmailApi::createLabel(const std::string& name)
{
    if (name.empty() || name.size() > 256 || name.find_first_of("\r\n") != std::string::npos || name.find('\0') != std::string::npos)
        throw NetworkError(NetworkFailure::Protocol, "The Gmail label name is invalid.");
    return label(google::parse(request("/labels", "POST", Json{{"name", name}, {"labelListVisibility", "labelShow"}, {"messageListVisibility", "show"}}.dump())));
}
GmailLabel GmailApi::renameLabel(const std::string& id,const std::string& name)
{
    identifier(id);
    if (name.empty() || name.size()>256 || name.find_first_of("\r\n")!=std::string::npos || name.find('\0')!=std::string::npos)
        throw NetworkError(NetworkFailure::Protocol,"The Gmail label name is invalid.");
    auto result=label(google::parse(request("/labels/"+urlEncode(id),"PATCH",Json{{"name",name}}.dump())));
    if (result.id!=id || result.name!=name || result.system) throw NetworkError(NetworkFailure::Protocol,"Gmail returned a different label than the requested rename.");
    return result;
}
void GmailApi::deleteLabel(const std::string& id)
{ identifier(id); request("/labels/"+urlEncode(id),"DELETE"); }
GmailSent GmailApi::send(const std::string& raw, const std::vector<std::string>& recipients, const std::string& thread)
{
    if (!thread.empty()) identifier(thread);
    Json body{{"raw", base64UrlEncode(submission(raw, recipients))}};
    if (!thread.empty()) body["threadId"] = thread;
    auto response = request("/messages/send", "POST", body.dump(), Mutation::Delivery);
    try {
        auto value = google::parse(response); return {idField(value, "id"), idField(value, "threadId")};
    } catch (const std::exception&) {
        // A successful HTTP response with an invalid result can still represent
        // accepted mail. Preserve uncertainty instead of inviting a duplicate.
        throw NetworkError(NetworkFailure::DeliveryUnknown, "Gmail accepted the request but did not return a usable receipt. Check Sent before retrying.");
    }
}
GmailDraftPage GmailApi::drafts(const std::string& page,const std::string& query)
{
    if(page.size()>8192 || query.size()>8192) throw NetworkError(NetworkFailure::Protocol,"Invalid Gmail draft query or page token.");
    auto path=std::string("/drafts?maxResults=500&includeSpamTrash=true");
    if(!page.empty()) path+="&pageToken="+urlEncode(page);
    if(!query.empty()) path+="&q="+urlEncode(query);
    auto value=google::parse(request(path)); GmailDraftPage result;
    result.next=google::string(value,"nextPageToken",8192,false);
    for(const auto& item:array(value,"drafts",500)) result.drafts.push_back(draftValue(item,false));
    return result;
}
GmailDraft GmailApi::draft(const std::string& id,bool raw)
{
    identifier(id); auto result=draftValue(google::parse(request("/drafts/"+urlEncode(id)+"?format="+(raw?"raw":"minimal"))),raw);
    if(result.id!=id) throw NetworkError(NetworkFailure::Protocol,"Gmail returned a different draft than requested.");
    return result;
}
GmailDraft GmailApi::createDraft(const std::string& raw,const std::string& thread)
{
    auto response=request("/drafts","POST",draftBody(raw,thread).dump(),Mutation::Draft);
    try { return draftValue(google::parse(response),false); }
    catch(const std::exception&) {
        throw NetworkError(NetworkFailure::CopyUnknown,"Gmail accepted the draft request but did not return its identifier. Check Drafts before saving it again.");
    }
}
GmailDraft GmailApi::updateDraft(const std::string& id,const std::string& raw,const std::string& thread)
{
    identifier(id); auto body=draftBody(raw,thread); body["id"]=id;
    auto response=request("/drafts/"+urlEncode(id),"PUT",body.dump(),Mutation::Draft);
    try {
        auto result=draftValue(google::parse(response),false);
        if(result.id!=id) throw NetworkError(NetworkFailure::Protocol,"The returned draft identifier changed.");
        return result;
    } catch(const std::exception&) {
        throw NetworkError(NetworkFailure::CopyUnknown,"Gmail accepted the draft update but did not return a usable receipt. Check Drafts before saving it again.");
    }
}
void GmailApi::deleteDraft(const std::string& id)
{ identifier(id); request("/drafts/"+urlEncode(id),"DELETE"); }
GmailSent GmailApi::sendDraft(const std::string& id,const std::string& raw,const std::vector<std::string>& recipients,const std::string& thread)
{
    identifier(id); auto body=draftBody(submission(raw,recipients),thread); body["id"]=id;
    auto response=request("/drafts/send","POST",body.dump(),Mutation::Delivery);
    try {
        auto value=google::parse(response); return {idField(value,"id"),idField(value,"threadId")};
    } catch(const std::exception&) {
        throw NetworkError(NetworkFailure::DeliveryUnknown,"Gmail accepted the draft send but did not return a usable receipt. Check Sent before retrying.");
    }
}
}
