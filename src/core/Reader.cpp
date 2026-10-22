#include "mail/Preview.h"
#include "mail/Mime.h"
#include <libxml/HTMLparser.h>
#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>

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
class HtmlReader {
public:
    HtmlReader(const ReceivedPreview& message,const std::string& root):fMessage(message),fRoot(root) {}
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
std::string receivedHtml(const ReceivedPreview& message,const std::string& root,bool plain)
{
    validateReceivedPreview(message);
    // Validate the private resource root before interpolating any URL.
    previewDocument({},root);
    auto body=plain || message.html.empty()?"<pre style=\"white-space:pre-wrap;overflow-wrap:anywhere\">"+escapeHtml(message.text)+"</pre>":HtmlReader(message,root).Read();
    return previewDocument(body,root);
}
}
