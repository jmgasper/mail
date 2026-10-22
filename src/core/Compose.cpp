#include "mail/Compose.h"
#include "mail/Mime.h"
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace mail {
namespace {
void require(bool ok,const char* text) { if (!ok) throw std::invalid_argument(text); }
bool controls(const std::string& s) { for (unsigned char c:s) if (c<32 || c==127) return true; return false; }
std::string trim(std::string s)
{
    auto start=s.find_first_not_of(" \t\r\n"),end=s.find_last_not_of(" \t\r\n");
    return start==std::string::npos?"":s.substr(start,end-start+1);
}
std::string lower(std::string s) { for (auto& c:s) if (c>='A' && c<='Z') c+='a'-'A'; return s; }
bool boundary(const std::string& s,std::size_t n) { return n==s.size() || (n<s.size() && (static_cast<unsigned char>(s[n])&0xc0)!=0x80); }
bool utf8(const std::string& text)
{
    for (std::size_t i=0;i<text.size();) {
        auto c=static_cast<unsigned char>(text[i++]); if (c<128) { if (!c) return false; continue; }
        int more; unsigned value,minimum;
        if ((c&0xe0)==0xc0) { more=1; value=c&31; minimum=0x80; }
        else if ((c&0xf0)==0xe0) { more=2; value=c&15; minimum=0x800; }
        else if ((c&0xf8)==0xf0) { more=3; value=c&7; minimum=0x10000; }
        else return false;
        while (more--) { if (i==text.size()) return false; auto b=static_cast<unsigned char>(text[i++]); if ((b&0xc0)!=0x80) return false; value=(value<<6)|(b&63); }
        if (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
    }
    return true;
}
void address(const std::string& s)
{
    auto at=s.find('@');
    require(at!=std::string::npos && at>0 && at+1<s.size() && at==s.rfind('@') && at<=64 && s.size()<=254,
        "Enter an email address such as name@example.com.");
    for (unsigned char c:s) require(c>32 && c<127 && std::string("<>(),;:\\\"[]").find(c)==std::string::npos,
        "Use an ASCII email address without spaces or quoted local parts.");
    require(s.front()!='.' && s[at-1]!='.' && s.find("..") == std::string::npos,"An email address contains an invalid dot.");
}
std::string encoded(const std::string& s)
{
    require(!controls(s) && utf8(s),"A header contains an invalid character.");
    std::string out;
    for (std::size_t at=0;at<s.size();) {
        auto end=std::min(s.size(),at+42); while (!boundary(s,end)) --end;
        if (!out.empty()) out+="\r\n ";
        out+="=?UTF-8?B?"+base64Encode(std::string_view(s).substr(at,end-at))+"?="; at=end;
    }
    return out;
}
std::string mailboxHeader(const std::vector<Mailbox>& list)
{
    std::string out;
    for (const auto& m:list) {
        if (!out.empty()) out+=",\r\n ";
        if (!m.name.empty()) out+=encoded(m.name)+"\r\n <"+m.address+">";
        else out+=m.address;
    }
    return out;
}
std::string base64Part(const std::string& s)
{
    std::string out,encoded=base64Encode(s);
    for (std::size_t i=0;i<encoded.size();i+=76) out+=encoded.substr(i,76)+"\r\n";
    return out.empty()?"\r\n":out;
}
std::string normalized(const std::string& s)
{
    std::string out;
    for (std::size_t i=0;i<s.size();++i) {
        if (s[i]=='\r') { if (i+1<s.size() && s[i+1]=='\n') ++i; out+="\r\n"; }
        else if (s[i]=='\n') out+="\r\n"; else out+=s[i];
    }
    return out;
}
std::string textPart(const char* type,const std::string& body)
{ return std::string("Content-Type: ")+type+"; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n"+base64Part(normalized(body)); }
std::string percent(const std::string& s)
{
    constexpr char hex[]="0123456789ABCDEF"; std::string out;
    for (unsigned char c:s) { out+='%'; out+=hex[c>>4]; out+=hex[c&15]; } return out;
}
std::string attachmentPart(const Attachment& a)
{
    std::string out="Content-Type: "+a.mediaType+"\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: "+(a.inlineImage?"inline":"attachment");
    // RFC2231 continuations keep long/Unicode filenames within wire line limits.
    auto value=percent(a.name);
    if (value.size()<=54) out+=";\r\n filename*=utf-8''"+value;
    else for (std::size_t i=0,n=0;i<value.size();i+=54,++n)
        out+=";\r\n filename*"+std::to_string(n)+"*="+(n==0?"utf-8''":"")+value.substr(i,54);
    if (!a.contentId.empty()) out+="\r\nContent-ID: <"+a.contentId+">";
    return out+"\r\n\r\n"+base64Part(a.data);
}
std::string multipart(const char* type,const std::vector<std::string>& parts)
{
    std::string delimiter="mail_"+newMessageId(); delimiter.erase(std::remove_if(delimiter.begin(),delimiter.end(),[](char c){return c=='<' || c=='>' || c=='@';}),delimiter.end());
    std::string out=std::string("Content-Type: multipart/")+type+"; boundary=\""+delimiter+"\"";
    if (std::string(type)=="related") out+="; type=\"text/html\"";
    out+="\r\n\r\n";
    for (const auto& part:parts) out+="--"+delimiter+"\r\n"+part;
    return out+"--"+delimiter+"--\r\n";
}
bool messageId(const std::string& s)
{ return s.size()>=5 && s.size()<250 && s.front()=='<' && s.back()=='>' && !controls(s) && s.find_first_of(" \t",0)==std::string::npos && s.find('@')!=std::string::npos && s.find('<',1)==std::string::npos && s.find('>')==s.size()-1; }
std::string quotedBody(const Message& m)
{
    auto time=static_cast<std::time_t>(m.received); std::tm tm{}; char date[80]="an earlier date";
    if (gmtime_r(&time,&tm)) std::strftime(date,sizeof(date),"%d %b %Y at %H:%M UTC",&tm);
    std::string out="\n\nOn "+std::string(date)+", "+m.sender+" wrote:\n> ";
    for (char c:m.text) { out+=c; if (c=='\n') out+="> "; } return out;
}
}
std::string newMessageId()
{
    unsigned char bytes[16]; sqlite3_randomness(sizeof(bytes),bytes);
    constexpr char digits[]="0123456789abcdef"; std::string out="<";
    for (auto b:bytes) { out+=digits[b>>4]; out+=digits[b&15]; } return out+"@mail.airos>";
}
std::vector<Mailbox> parseAddresses(const std::string& input,std::size_t nameLimit)
{
    require(!controls(input) && utf8(input),"A recipient contains an invalid character.");
    if (trim(input).empty()) return {};
    std::vector<std::string> pieces; std::size_t start=0; bool quote=false,escape=false; int angle=0;
    for (std::size_t i=0;i<input.size();++i) {
        char c=input[i];
        if (escape) { escape=false; continue; }
        if (quote && c=='\\') { escape=true; continue; }
        if (c=='"') quote=!quote;
        if (!quote) {
            if (c=='<') { require(angle==0,"Invalid recipient brackets."); angle=1; }
            if (c=='>') { require(angle==1,"Invalid recipient brackets."); angle=0; }
            if (c==',' && !angle) { pieces.push_back(trim(input.substr(start,i-start))); start=i+1; }
        }
    }
    require(!quote && !angle && !escape,"Finish the quoted name or address brackets.");
    pieces.push_back(trim(input.substr(start)));
    require(pieces.size()<=100,"Send to at most 100 recipients at a time.");
    std::vector<Mailbox> out;
    for (auto part:pieces) {
        Mailbox m; auto open=part.rfind('<');
        if (open==std::string::npos) m.address=part;
        else {
            require(part.back()=='>',"Unexpected text after an email address.");
            m.name=trim(part.substr(0,open)); m.address=trim(part.substr(open+1,part.size()-open-2));
            if (!m.name.empty() && m.name.front()=='"') {
                require(m.name.size()>1 && m.name.back()=='"',"Finish the quoted display name.");
                auto name=m.name.substr(1,m.name.size()-2); m.name.clear(); bool escaped=false;
                for (char c:name) { if (!escaped && c=='\\') escaped=true; else { m.name+=c; escaped=false; } }
            }
        }
        address(m.address); require(m.name.size()<=nameLimit,"The recipient name is too long."); out.push_back(std::move(m));
    }
    return out;
}
std::string imagePlaceholder(const Attachment& image) { return "[Image: "+image.name+"]"; }
void validateDraft(const Draft& d)
{
    require(d.account>0,"Choose a sending account.");
    require(d.text.size()<=2*1024*1024 && utf8(d.text),"The message body must be valid UTF-8 and at most 2 MiB.");
    for (const auto* h:{&d.to,&d.cc,&d.bcc,&d.subject}) require(h->size()<=8192 && !controls(*h) && utf8(*h),"A message header is invalid or too long.");
    require(d.messageId.empty() || messageId(d.messageId),"Invalid outgoing message identifier.");
    require(d.runs.size()<=10000 && d.references.size()<=100 && d.attachments.size()<=100 && d.images.size()<=500,"This draft has too many parts.");
    std::size_t previous=0; bool first=true;
    for (const auto& run:d.runs) {
        require(run.offset<=d.text.size() && boundary(d.text,run.offset) && (first || run.offset>previous),"Text styles have invalid offsets.");
        require(run.link.size()<=2048 && !controls(run.link) && utf8(run.link) && (run.link.empty() || run.link.rfind("https://",0)==0 || run.link.rfind("http://",0)==0 || run.link.rfind("mailto:",0)==0),"Links must use https, http, or mailto.");
        previous=run.offset; first=false;
    }
    for (const auto& ref:d.references) require(messageId(ref),"Invalid reply reference.");
    std::size_t total=d.text.size(); std::set<std::string> contentIds;
    for (const auto& a:d.attachments) {
        require(!a.name.empty() && a.name.size()<=512 && !controls(a.name) && utf8(a.name),"An attachment has an invalid name.");
        require(!a.mediaType.empty() && a.mediaType.size()<128 && a.mediaType.find('/')!=std::string::npos
            && a.mediaType.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$&^_.+-/")==std::string::npos,"An attachment has an invalid media type.");
        require(!a.inlineImage || a.mediaType.rfind("image/",0)==0,"Only images can be included inline.");
        require(a.contentId.size()<=200 && a.contentId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._-")==std::string::npos,"An attachment has an invalid content identifier.");
        require(!a.inlineImage || !a.contentId.empty(),"An inline image needs a content identifier.");
        if (!a.contentId.empty()) require(contentIds.insert(a.contentId).second,"Attachment content identifiers must be unique.");
        total+=a.data.size(); require(total<=22*1024*1024,"The message and attachments exceed the 22 MiB composition limit.");
    }
    std::size_t end=0;
    for(const auto& image:d.images) {
        require(image.offset>=end && image.offset<=d.text.size() && image.length<=d.text.size()-image.offset && image.length>0
            && boundary(d.text,image.offset) && boundary(d.text,image.offset+image.length),"An inline image has an invalid text position.");
        auto file=std::find_if(d.attachments.begin(),d.attachments.end(),[&](const Attachment& a){return a.contentId==image.contentId;});
        require(file!=d.attachments.end() && file->inlineImage && d.text.substr(image.offset,image.length)==imagePlaceholder(*file),"An inline image is missing its file or placeholder.");
        end=image.offset+image.length;
        for(const auto& run:d.runs) require(run.offset<=image.offset || run.offset>=end,"Text styles cannot split an inline image.");
    }
}
std::string draftHtml(const Draft& d)
{
    validateDraft(d);
    std::string out="<!doctype html><html><body style=\"font-family:sans-serif\"><div style=\"white-space:pre-wrap\">";
    auto append=[&](std::size_t begin,std::size_t end,const TextRun& run) {
        if (begin==end) return;
        if (!run.link.empty()) out+="<a href=\""+escapeHtml(run.link)+"\">";
        if (run.bold) out+="<strong>";
        if (run.italic) out+="<em>";
        if (run.underline) out+="<u>";
        auto at=begin;
        for(const auto& image:d.images) {
            if(image.offset<begin || image.offset>=end) continue;
            out+=escapeHtml(std::string_view(d.text).substr(at,image.offset-at));
            auto file=std::find_if(d.attachments.begin(),d.attachments.end(),[&](const Attachment& a){return a.contentId==image.contentId;});
            out+="<img src=\"cid:"+escapeHtml(image.contentId)+"\" alt=\""+escapeHtml(file->name)+"\" style=\"max-width:100%;height:auto\">";
            at=image.offset+image.length;
        }
        out+=escapeHtml(std::string_view(d.text).substr(at,end-at));
        if (run.underline) out+="</u>";
        if (run.italic) out+="</em>";
        if (run.bold) out+="</strong>";
        if (!run.link.empty()) out+="</a>";
    };
    if (d.runs.empty()) append(0,d.text.size(),{});
    else {
        append(0,d.runs.front().offset,{});
        for (std::size_t i=0;i<d.runs.size();++i) append(d.runs[i].offset,i+1<d.runs.size()?d.runs[i+1].offset:d.text.size(),d.runs[i]);
    }
    out+="</div>";
    for (const auto& a:d.attachments) if (a.inlineImage && std::none_of(d.images.begin(),d.images.end(),[&](const InlineImage& image){return image.contentId==a.contentId;}))
        out+="<p><img src=\"cid:"+escapeHtml(a.contentId)+"\" alt=\""+escapeHtml(a.name)+"\" style=\"max-width:100%;height:auto\"></p>";
    return out+"</body></html>";
}
static ComposedMessage compose(const Draft& d,const Account& a,Timestamp time,bool draft)
{
    validateDraft(d); address(a.email); require(a.id==d.account && a.provider!=Provider::Local,"Choose a configured sending account.");
    require(messageId(d.messageId),"Save the draft before sending.");
    auto to=parseAddresses(d.to),cc=parseAddresses(d.cc),bcc=parseAddresses(d.bcc);
    ComposedMessage out; out.from=a.email; std::set<std::string> unique;
    for (const auto* list:{&to,&cc,&bcc}) for (const auto& m:*list) if (unique.insert(lower(m.address)).second) out.recipients.push_back(m.address);
    require((draft || !out.recipients.empty()) && out.recipients.size()<=100,"Add between 1 and 100 recipients.");
    std::time_t t=static_cast<std::time_t>(time); std::tm tm{}; require(gmtime_r(&t,&tm),"Invalid outgoing date.");
    const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}; const char* months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char date[80]; std::snprintf(date,sizeof(date),"%s, %02d %s %04d %02d:%02d:%02d +0000",days[tm.tm_wday],tm.tm_mday,months[tm.tm_mon],tm.tm_year+1900,tm.tm_hour,tm.tm_min,tm.tm_sec);
    out.raw="Date: "+std::string(date)+"\r\nFrom: "+a.email+"\r\nMessage-ID: "+d.messageId+"\r\n";
    if (!to.empty()) out.raw+="To: "+mailboxHeader(to)+"\r\n";
    else if (cc.empty() && !draft) out.raw+="To: undisclosed-recipients:;\r\n";
    if (!cc.empty()) out.raw+="Cc: "+mailboxHeader(cc)+"\r\n";
    if (draft && !bcc.empty()) out.raw+="Bcc: "+mailboxHeader(bcc)+"\r\n";
    out.raw+="Subject: "+encoded(d.subject)+"\r\n";
    if (!d.references.empty()) {
        out.raw+="In-Reply-To: "+d.references.back()+"\r\nReferences:";
        for (const auto& ref:d.references) out.raw+="\r\n "+ref;
        out.raw+="\r\n";
    }
    out.raw+="MIME-Version: 1.0\r\n";
    auto html=textPart("text/html",draftHtml(d)); std::vector<std::string> related{html};
    for (const auto& attachment:d.attachments) if (attachment.inlineImage) related.push_back(attachmentPart(attachment));
    if (related.size()>1) html=multipart("related",related);
    auto body=multipart("alternative",{textPart("text/plain",d.text),html}); std::vector<std::string> mixed{body};
    for (const auto& attachment:d.attachments) if (!attachment.inlineImage) mixed.push_back(attachmentPart(attachment));
    out.raw+=mixed.size()>1?multipart("mixed",mixed):body;
    require(out.raw.size()<=32*1024*1024,"The encoded message exceeds 32 MiB.");
    return out;
}
ComposedMessage composeMessage(const Draft& d,const Account& a,Timestamp time)
{ return compose(d,a,time,false); }
std::string composeDraftMessage(const Draft& d,const Account& a,Timestamp time)
{ return compose(d,a,time,true).raw; }
Draft replyDraft(const Message& m,const Account& a,bool all)
{
    Draft d; d.account=a.id; d.messageId=newMessageId(); d.providerThread=m.providerThread;
    d.subject=lower(m.subject).rfind("re:",0)==0?m.subject:"Re: "+m.subject;
    d.references=m.references; if (messageId(m.messageId)) d.references.push_back(m.messageId);
    while (d.references.size()>100) d.references.erase(d.references.begin()+1);
    d.text=quotedBody(m); std::set<std::string> seen{lower(a.email)};
    auto add=[&](const std::string& source,std::string& target) {
        for (const auto& address:parseAddresses(source)) if (seen.insert(lower(address.address)).second) {
            if (!target.empty()) target+=", ";
            target+=address.address;
        }
    };
    // Reply-To is decoded with the same header parser as other display fields.
    auto raw=parseMessage(m.raw.empty()?"From: "+m.sender+"\n\n":m.raw);
    add(raw.replyTo.empty()?m.sender:raw.replyTo,d.to);
    if (all) { add(m.to,d.to); add(m.cc,d.cc); }
    if (d.to.empty()) add(m.to,d.to);
    return d;
}
Draft forwardDraft(const Message& m,const Account& a)
{
    Draft d; d.account=a.id; d.messageId=newMessageId();
    d.subject=lower(m.subject).rfind("fwd:",0)==0?m.subject:"Fwd: "+m.subject;
    d.text="\n\n---------- Forwarded message ----------\nFrom: "+m.sender+"\nTo: "+m.to+"\nSubject: "+m.subject+"\n\n"+m.text;
    d.attachments=m.attachments; for (auto& attachment:d.attachments) attachment.inlineImage=false;
    return d;
}
}
