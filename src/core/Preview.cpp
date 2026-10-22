#include "mail/Preview.h"
#include "mail/Mime.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace mail {
namespace {
using Json=nlohmann::json;
void require(bool ok,const char* why) { if(!ok) throw std::invalid_argument(why); }
void validate(const PreviewSnapshot& s)
{
    require(!s.key.empty() && s.key.size()<=250,"Invalid preview identity.");
    if(s.received) { validateReceivedPreview(*s.received); return; }
    validateDraft(s.draft);
    auto headers=s.draft; headers.subject=s.from; validateDraft(headers);
}
void replace(std::string& text,const std::string& from,const std::string& to)
{
    for(std::size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size()) text.replace(at,from.size(),to);
}
std::string documentHead(const std::string& root)
{
    require(root.rfind("mail-preview://",0)==0 && root.size()<=200 && root.back()=='/'
        && root.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:./-")==std::string::npos,"Invalid preview resource location.");
    return "<head><meta charset=\"utf-8\"><meta http-equiv=\"Content-Security-Policy\" content=\""+std::string(kPreviewPolicy)+"\">"
        "<style>@font-face{font-family:MailEmoji;src:url('"+root+"emoji.ttf');unicode-range:U+A9,U+AE,U+2000-2BFF,U+3030,U+303D,U+3297,U+3299,U+FE0E-FE0F,U+1F000-1FAFF,U+E0020-E007F}"
        "body{font-family:MailEmoji,sans-serif!important;margin:16px}a{color:#1255a2;text-decoration:underline}img{max-width:100%;height:auto}</style></head>";
}
}
std::string encodePreview(const PreviewSnapshot& s)
{
    validate(s); const auto& d=s.draft;
    if(s.received) {
        const auto& m=*s.received;
        Json j={{"version",2},{"key",s.key},{"received",true},{"id",m.id},{"from",m.from},{"to",m.to},{"cc",m.cc},
            {"subject",m.subject},{"date",m.date},{"text",m.text},{"html",m.html},{"files",Json::array()},{"thread",Json::array()}};
        for(const auto& f:m.files) j["files"].push_back({f.name,f.mediaType,f.contentId,base64Encode(f.data),f.inlineImage});
        for(const auto& item:m.thread) j["thread"].push_back({item.id,item.sender,item.date});
        auto bytes=j.dump(); require(bytes.size()<=kPreviewSnapshotLimit,"The received message preview is too large."); return bytes;
    }
    Json j={{"version",1},{"key",s.key},{"from",s.from},{"to",d.to},{"cc",d.cc},{"bcc",d.bcc},{"subject",d.subject},{"text",d.text},
        {"runs",Json::array()},{"images",Json::array()},{"files",Json::array()}};
    for(const auto& r:d.runs) j["runs"].push_back({r.offset,r.bold,r.italic,r.underline,r.link});
    for(const auto& i:d.images) j["images"].push_back({i.offset,i.length,i.contentId});
    for(const auto& f:d.attachments) j["files"].push_back({f.name,f.mediaType,f.contentId,base64Encode(f.data),f.inlineImage});
    auto bytes=j.dump(); require(bytes.size()<=kPreviewSnapshotLimit,"The preview snapshot is too large."); return bytes;
}
PreviewSnapshot decodePreview(const std::string& bytes)
{
    require(bytes.size()<=kPreviewSnapshotLimit,"The preview snapshot is too large.");
    auto j=Json::parse(bytes);
    if(j.at("version")==2) {
        require(j.at("received")==true,"Invalid received preview format.");
        PreviewSnapshot s; s.key=j.at("key").get<std::string>(); s.received.emplace(); auto& m=*s.received;
        m.id=j.at("id").get<Id>(); m.from=j.at("from").get<std::string>(); m.to=j.at("to").get<std::string>(); m.cc=j.at("cc").get<std::string>();
        m.subject=j.at("subject").get<std::string>(); m.date=j.at("date").get<std::string>(); m.text=j.at("text").get<std::string>(); m.html=j.at("html").get<std::string>();
        require(j.at("files").is_array() && j["files"].size()<=1000 && j.at("thread").is_array() && j["thread"].size()<=10000,"The received preview contains too many parts.");
        std::size_t total=m.text.size()+m.html.size(); require(total<=16*1024*1024,"The received message body is too large.");
        for(const auto& f:j["files"]) {
            auto encoded=f.at(3).get<std::string>();
            require(encoded.size()<=(32*1024*1024-total+2)/3*4,"The received attachments are too large.");
            auto data=base64Decode(encoded); require(base64Encode(data)==encoded,"An attachment in the received preview is incomplete.");
            total+=data.size(); require(total<=32*1024*1024,"The received attachments are too large.");
            m.files.push_back({f.at(0).get<std::string>(),f.at(1).get<std::string>(),f.at(2).get<std::string>(),std::move(data),f.at(4).get<bool>()});
        }
        for(const auto& item:j["thread"]) m.thread.push_back({item.at(0).get<Id>(),item.at(1).get<std::string>(),item.at(2).get<std::string>()});
        validate(s); return s;
    }
    require(j.at("version")==1,"This preview needs a newer version of Mail.");
    PreviewSnapshot s; auto& d=s.draft; d.account=1;
    s.key=j.at("key").get<std::string>(); s.from=j.at("from").get<std::string>();
    d.to=j.at("to").get<std::string>(); d.cc=j.at("cc").get<std::string>(); d.bcc=j.at("bcc").get<std::string>();
    d.subject=j.at("subject").get<std::string>(); d.text=j.at("text").get<std::string>();
    require(j.at("runs").is_array() && j["runs"].size()<=10000 && j.at("images").is_array() && j["images"].size()<=500
        && j.at("files").is_array() && j["files"].size()<=100,"The preview contains too many parts.");
    for(const auto& r:j["runs"]) d.runs.push_back({r.at(0).get<std::size_t>(),r.at(1).get<bool>(),r.at(2).get<bool>(),r.at(3).get<bool>(),r.at(4).get<std::string>()});
    for(const auto& i:j["images"]) d.images.push_back({i.at(0).get<std::size_t>(),i.at(1).get<std::size_t>(),i.at(2).get<std::string>()});
    std::size_t total=d.text.size(); require(total<=2*1024*1024,"The preview body is too large.");
    for(const auto& f:j["files"]) {
        auto encoded=f.at(3).get<std::string>();
        require(encoded.size()<=(22*1024*1024-total+2)/3*4,"The preview attachments are too large.");
        auto data=base64Decode(encoded); require(base64Encode(data)==encoded,"An image in the preview is incomplete.");
        total+=data.size(); require(total<=22*1024*1024,"The preview attachments are too large.");
        d.attachments.push_back({f.at(0).get<std::string>(),f.at(1).get<std::string>(),f.at(2).get<std::string>(),std::move(data),f.at(4).get<bool>()});
    }
    validate(s); return s;
}
std::string previewHtml(const Draft& d,const std::string& root)
{
    auto head=documentHead(root);
    auto html=draftHtml(d);
    replace(html,"<a href=\"","<a title=\"");
    for(std::size_t i=0;i<d.attachments.size();++i) if(d.attachments[i].inlineImage)
        replace(html,"src=\"cid:"+escapeHtml(d.attachments[i].contentId)+"\"","src=\""+root+"image/"+std::to_string(i)+"\"");
    // draftHtml emits a complete fixed document. Put the policy before its body.
    replace(html,"<html>","<html>"+head); return html;
}
std::string previewDocument(const std::string& body,const std::string& root)
{
    return "<!doctype html><html>"+documentHead(root)+"<body>"+body+"</body></html>";
}
}
