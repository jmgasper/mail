#include "mail/Preview.h"
#include "mail/Mime.h"
#include <libxml/HTMLparser.h>
#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <cstring>

namespace mail {
namespace {
constexpr std::size_t bodyLimit=8*1024*1024,totalLimit=32*1024*1024;
void require(bool ok,const char* why) { if(!ok) throw std::invalid_argument(why); }
std::string lower(std::string value) { for(auto& c:value) if(c>='A' && c<='Z') c+='a'-'A'; return value; }
std::string attribute(xmlNode* node,const char* name)
{
    auto* raw=xmlGetProp(node,reinterpret_cast<const xmlChar*>(name));
    std::string result=raw?reinterpret_cast<const char*>(raw):""; xmlFree(raw); return result;
}
std::string cid(std::string value)
{
    if(lower(value.substr(0,4))!="cid:") return {};
    value.erase(0,4); std::string result;
    auto hex=[](char c)->int { if(c>='0' && c<='9') return c-'0'; if(c>='a' && c<='f') return c-'a'+10; if(c>='A' && c<='F') return c-'A'+10; return -1; };
    for(std::size_t i=0;i<value.size();++i) {
        unsigned char c=value[i];
        if(c=='%') { if(i+2>=value.size() || hex(value[i+1])<0 || hex(value[i+2])<0) return {}; c=hex(value[i+1])*16+hex(value[i+2]); i+=2; }
        if(c<=32 || c==127) return {};
        result+=c;
    }
    return result;
}
bool hasPrefix(const std::string& text,const char* prefix) { return lower(text.substr(0,std::string_view(prefix).size()))==prefix; }
// Plain text with web and mail addresses turned into links.
std::string linkedText(const std::string& text)
{
    std::string out; auto lowered=lower(text);
    for(std::size_t at=0;at<text.size();) {
        std::size_t next=std::string::npos; for(const char* scheme:{"https://","http://","mailto:"}) next=std::min(next,lowered.find(scheme,at));
        if(next==std::string::npos) { out+=escapeHtml(std::string_view(text).substr(at)); break; }
        auto end=next; while(end<text.size() && static_cast<unsigned char>(text[end])>32 && !std::strchr("<>\"'",text[end])) ++end;
        while(end>next && std::strchr(".,;:!?)]",text[end-1])) --end;
        auto url=text.substr(next,end-next);
        out+=escapeHtml(std::string_view(text).substr(at,next-at));
        if(openableLink(url)) out+="<a href=\""+escapeHtml(url)+"\" target=\"_blank\" rel=\"noreferrer\" title=\""+escapeHtml(url)+"\">"+escapeHtml(url)+"</a>";
        else out+=escapeHtml(url);
        at=std::max(end,next+1);
    }
    return out;
}
// Plain text with links, and quoted lines ("> ...") set apart.
std::string plainHtml(const std::string& text)
{
    std::string out="<div class=\"mail-plain\">";
    for(std::size_t at=0;at<=text.size();) {
        auto end=text.find('\n',at); if(end==std::string::npos) end=text.size();
        auto line=text.substr(at,end-at); if(!line.empty() && line.back()=='\r') line.pop_back();
        bool quoted=!line.empty() && line[0]=='>';
        out+=quoted?"<span class=\"mail-quote\">"+linkedText(line)+"</span>":linkedText(line);
        if(end<text.size()) out+='\n';
        at=end+1;
    }
    return out+"</div>";
}
class HtmlReader {
public:
    // With links, http(s) and mailto anchors open a new page that the reader
    // hands to the system instead of navigating the document.
    HtmlReader(const ReceivedPreview& message,const std::string& root,bool links=false):fMessage(message),fRoot(root),fLinks(links) {}
    std::string Read()
    {
        // HTML parsing never requests a DTD or enables XML entity substitution.
        // NONET is also specified for older libxml2 installations. Parse only
        // the bounded in-memory body, forcing the MIME parser's decoded UTF-8.
        std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> doc(htmlReadMemory(fMessage.html.data(),fMessage.html.size(),nullptr,"UTF-8",
            HTML_PARSE_NONET|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING|HTML_PARSE_NODEFDTD),xmlFreeDoc);
        require(bool(doc),"This message's HTML could not be read. Use the plain-text view.");
        Walk(xmlDocGetRootElement(doc.get()),0); return std::move(fOutput);
    }
private:
    std::string Image(const std::string& source)
    {
        auto id=cid(source); if(id.empty()) return {};
        std::string result;
        for(std::size_t i=0;i<fMessage.files.size();++i) {
            const auto& file=fMessage.files[i];
            if(file.contentId!=id) continue;
            // Duplicate Content-IDs are ambiguous even when one part is not an image.
            if(!result.empty() || !previewImageType(file.mediaType)) return {};
            result=fRoot+"image/"+std::to_string(i);
        }
        return result;
    }
    void Walk(xmlNode* node,unsigned depth)
    {
        require(depth<=128,"This message's HTML is nested too deeply. Use the plain-text view.");
        static const std::set<std::string> tags={"a","abbr","address","article","b","bdi","bdo","big","blockquote","br","caption","center","cite","code","col","colgroup","dd","del","details","dfn","div","dl","dt","em","figcaption","figure","font","footer","h1","h2","h3","h4","h5","h6","header","hr","i","img","ins","kbd","li","main","mark","ol","p","pre","q","s","samp","section","small","span","strike","strong","sub","summary","sup","table","tbody","td","tfoot","th","thead","tr","tt","u","ul","var","wbr"};
        static const std::set<std::string> drop={"script","iframe","frame","frameset","object","embed","svg","math","template","noscript","noembed","noframes","xmp","plaintext","textarea","select","title"};
        static const std::set<std::string> attrs={"align","valign","bgcolor","border","cellpadding","cellspacing","class","color","colspan","dir","face","height","id","lang","reversed","rowspan","size","span","start","style","title","type","width"};
        static const std::set<std::string> empty={"br","col","hr","img","wbr"};
        for(;node;node=node->next) {
            require(++fNodes<=100000,"This message's HTML is too complex. Use the plain-text view.");
            if(node->type==XML_TEXT_NODE || node->type==XML_CDATA_SECTION_NODE) {
                if(node->content) fOutput+=escapeHtml(reinterpret_cast<const char*>(node->content));
            } else if(node->type==XML_ELEMENT_NODE) {
                auto tag=lower(reinterpret_cast<const char*>(node->name));
                if(drop.count(tag)) continue;
                if(tag=="style") {
                    auto* raw=xmlNodeGetContent(node); std::string css=raw?reinterpret_cast<const char*>(raw):""; xmlFree(raw);
                    // Keep stylesheet layout, but never emit a literal HTML end
                    // tag from raw-text content (HTML parsers differ on errors).
                    std::string safe; for(char c:css) { if(c=='<') safe+="\\3c "; else safe+=c; }
                    fOutput+="<style>"+safe+"</style>";
                } else {
                    bool allowed=tags.count(tag) || tag=="body";
                    if(tag=="body") tag="div";
                    if(allowed) {
                        auto image=tag=="img"?Image(attribute(node,"src")):std::string();
                        if(tag=="img" && image.empty()) {
                            auto alt=attribute(node,"alt");
                            fOutput+="<span class=\"mail-unavailable-image\">["+escapeHtml(alt.empty()?"Image not loaded":alt)+"]</span>";
                            continue;
                        }
                        fOutput+='<'+tag;
                        for(auto* attr=node->properties;attr;attr=attr->next) {
                            auto name=lower(reinterpret_cast<const char*>(attr->name));
                            if(!attr->ns && attrs.count(name)) fOutput+=' '+name+"=\""+escapeHtml(attribute(node,name.c_str()))+'"';
                        }
                        if(tag=="img") fOutput+=" src=\""+image+"\" alt=\""+escapeHtml(attribute(node,"alt"))+"\"";
                        if(tag=="a" && attribute(node,"title").empty()) fOutput+=" title=\""+escapeHtml(attribute(node,"href"))+"\"";
                        if(tag=="a" && fLinks && openableLink(attribute(node,"href"))) fOutput+=" href=\""+escapeHtml(attribute(node,"href"))+"\" target=\"_blank\" rel=\"noreferrer\"";
                        auto background=Image(attribute(node,"background"));
                        if(!background.empty()) fOutput+=" background=\""+background+"\"";
                        fOutput+='>';
                    }
                    Walk(node->children,depth+1);
                    if(allowed && !empty.count(tag)) fOutput+="</"+tag+'>';
                }
            }
            require(fOutput.size()<=32*1024*1024,"The formatted message is too large. Use the plain-text view.");
        }
    }
    const ReceivedPreview& fMessage;
    const std::string& fRoot;
    bool fLinks;
    std::string fOutput;
    std::size_t fNodes=0;
};
}
bool previewImageType(const std::string& type)
{
    static const std::set<std::string> types={"image/png","image/jpeg","image/gif","image/webp","image/bmp","image/x-icon","image/vnd.microsoft.icon","image/svg+xml"};
    return types.count(type);
}
void validateReceivedPreview(const ReceivedPreview& m)
{
    require(m.id>0 && m.thread.size()<=10000,"Invalid received message identity.");
    for(const auto* header:{&m.from,&m.to,&m.cc,&m.subject,&m.date})
        require(header->size()<=8192 && header->find('\0')==std::string::npos,"A received message header is too large or invalid.");
    require(m.text.size()<=bodyLimit && m.html.size()<=bodyLimit,"The formatted reader supports message bodies up to 8 MiB. Use the plain-text reader.");
    require(m.files.size()<=1000,"The received message has too many attachments.");
    std::size_t total=m.text.size()+m.html.size();
    for(const auto& file:m.files) {
        require(file.name.size()<=4096 && file.contentId.size()<=1024 && file.mediaType.size()<=200,"Invalid received attachment metadata.");
        total+=file.data.size(); require(total<=totalLimit,"The received message snapshot exceeds 32 MiB.");
    }
    std::set<Id> ids;
    for(const auto& item:m.thread) require(item.id>0 && ids.insert(item.id).second && item.sender.size()<=8192 && item.date.size()<=8192,"Invalid conversation summary.");
    require(ids.empty() || ids.count(m.id),"This message is not in the previewed conversation.");
}
bool openableLink(const std::string& url)
{
    if(url.size()>8192 || url.find_first_of(std::string("\0\r\n\t \"<>",8))!=std::string::npos) return false;
    return (hasPrefix(url,"https://") || hasPrefix(url,"http://")) ? url.size()>(hasPrefix(url,"https://")?8:7) : (hasPrefix(url,"mailto:") && url.size()>7);
}
std::string conversationHtml(const std::vector<ReceivedPreview>& messages,const std::string& root,bool plain,const std::set<Id>& expanded)
{
    previewDocument({},root);
    std::string body="<div class=\"mail-conversation\">";
    for(std::size_t k=0;k<messages.size();++k) {
        const auto& m=messages[k]; validateReceivedPreview(m);
        auto messageRoot=root+"m"+std::to_string(k)+"/";
        std::string content;
        try { content=plain || m.html.empty()?plainHtml(m.text):HtmlReader(m,messageRoot,true).Read(); }
        catch(const std::invalid_argument&) { content=plainHtml(m.text); }
        auto snippet=m.text.substr(0,240); for(auto& c:snippet) if(c=='\r' || c=='\n' || c=='\t') c=' ';
        std::string files;
        for(const auto& f:m.files) if(!f.inlineImage) files+="<span class=\"mail-file\">"+escapeHtml(f.name)+"</span>";
        body+=std::string("<details class=\"mail-message\"")+(expanded.count(m.id) || k+1==messages.size()?" open":"")+"><summary class=\"mail-header\">"
            "<span class=\"mail-date\">"+escapeHtml(m.date)+"</span><span class=\"mail-from\">"+escapeHtml(m.from)+"</span>"
            "<span class=\"mail-snippet\">"+escapeHtml(snippet)+"</span>"
            "<span class=\"mail-recipients\">To: "+escapeHtml(m.to)+(m.cc.empty()?"":"<br>Cc: "+escapeHtml(m.cc))+"</span></summary>"
            +(files.empty()?"":"<div class=\"mail-files\">"+files+"</div>")+"<div class=\"mail-body\">"+content+"</div></details>";
        if(body.size()>totalLimit) throw std::invalid_argument("This conversation is too large to show formatted.");
    }
    body+="</div>";
    auto document=previewDocument(body,root);
    // The reader's own chrome. Sender styles come later in the body and may
    // restyle message content, but these rules keep the headers readable.
    static const char style[]="<style>"
        "body{margin:0!important;padding:0!important;background:#fff!important;color:#222}"
        ".mail-conversation{padding:8px 12px 24px}"
        ".mail-message{border:1px solid #d8d8d8!important;border-radius:4px;margin:0 0 10px!important;background:#fff!important;overflow:hidden}"
        ".mail-header{display:block!important;list-style:none;cursor:default;padding:8px 12px!important;background:#f3f3f3!important;color:#222!important;font:13px sans-serif!important;line-height:1.4!important}"
        ".mail-header::-webkit-details-marker{display:none}"
        ".mail-from{font-weight:bold!important;display:block}"
        ".mail-date{float:right;color:#666!important;margin-left:12px}"
        ".mail-snippet{display:block;color:#666!important;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
        ".mail-recipients{display:none;color:#555!important;font-size:12px!important}"
        "details[open]>.mail-header .mail-snippet{display:none}"
        "details[open]>.mail-header .mail-recipients{display:block}"
        ".mail-body{padding:12px 14px;overflow-x:auto;overflow-wrap:anywhere}"
        ".mail-plain{white-space:pre-wrap;font:13px/1.45 MailEmoji,sans-serif}"
        ".mail-quote{color:#5a6b7d;display:inline-block;border-left:3px solid #c8d3df;padding-left:8px}"
        ".mail-files{padding:6px 12px 0}"
        ".mail-file{display:inline-block;margin:0 6px 4px 0;padding:2px 8px;border:1px solid #ccc;border-radius:10px;font:12px sans-serif;color:#333;background:#fafafa}"
        ".mail-unavailable-image{color:#888;font-style:italic}"
        "</style>";
    auto at=document.find("</head>"); document.insert(at,style);
    return document;
}
std::string receivedHtml(const ReceivedPreview& message,const std::string& root,bool plain)
{
    validateReceivedPreview(message);
    // Validate the private resource root before interpolating any URL.
    previewDocument({},root);
    auto body=plain || message.html.empty()?"<pre style=\"white-space:pre-wrap;overflow-wrap:anywhere\">"+escapeHtml(message.text)+"</pre>":HtmlReader(message,root).Read();
    return previewDocument(body,root);
}
}
