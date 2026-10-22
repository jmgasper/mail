#include "mail/Clipboard.h"
#include "mail/Mime.h"
#include "mail/Preview.h"
#include <libxml/HTMLparser.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace mail {
namespace {
void require(bool ok,const char* why) {if(!ok)throw std::invalid_argument(why);}
bool space(unsigned char c){return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f';}
std::string lower(std::string s){for(auto& c:s)if(c>='A' && c<='Z')c+='a'-'A';return s;}
std::string trim(std::string s){auto a=s.find_first_not_of(" \t\r\n\f"),b=s.find_last_not_of(" \t\r\n\f");return a==std::string::npos?std::string():s.substr(a,b-a+1);}
std::string attribute(xmlNode* node,const char* name){auto* p=xmlGetProp(node,reinterpret_cast<const xmlChar*>(name));std::string s=p?reinterpret_cast<const char*>(p):"";xmlFree(p);return s;}
bool same(const TextRun& a,const TextRun& b){return a.bold==b.bold && a.italic==b.italic && a.underline==b.underline && a.link==b.link;}
std::string safeLink(std::string link)
{
    link=trim(std::move(link));if(link.size()>2048)return {};
    for(unsigned char c:link)if(c<32 || c==127)return {};
    auto colon=link.find(':');if(colon==std::string::npos)return {};
    link.replace(0,colon,lower(link.substr(0,colon)));
    if(link.rfind("https://",0) && link.rfind("http://",0) && link.rfind("mailto:",0))return {};
    return link;
}
std::map<std::string,std::string> declarations(std::string css)
{
    // Only simple, non-resource properties are interpreted. Quoted strings,
    // CSS escapes and arbitrary stylesheet rules are never evaluated.
    for(std::size_t at=0;(at=css.find("/*",at))!=std::string::npos;) {
        auto end=css.find("*/",at+2);if(end==std::string::npos){css.erase(at);break;}css.erase(at,end+2-at);
    }
    std::map<std::string,std::string> result;std::set<std::string> important;
    for(std::size_t at=0;at<css.size();) {
        auto end=css.find(';',at);if(end==std::string::npos)end=css.size();auto part=css.substr(at,end-at);at=end+1;
        auto colon=part.find(':');if(colon==std::string::npos)continue;
        auto key=lower(trim(part.substr(0,colon))),value=lower(trim(part.substr(colon+1)));
        bool priority=false;auto bang=value.find('!');if(bang!=std::string::npos){priority=trim(value.substr(bang+1))=="important";value=trim(value.substr(0,bang));}
        if(!important.count(key) || priority){result[key]=value;if(priority)important.insert(key);}
    }
    return result;
}
struct Style {TextRun run;bool pre=false;};
class Importer {
public:
    explicit Importer(const std::vector<Attachment>& files={})
    {
        require(files.size()<=100,"The draft contains too many attachments.");
        fDraft.attachments=files; fDataBegin=files.size();
        std::map<std::string,std::size_t> counts;
        for(const auto& file:files) if(!file.contentId.empty()) ++counts[file.contentId];
        for(std::size_t i=0;i<files.size();++i) {
            const auto& original=files[i]; auto& file=fDraft.attachments[i]; file.inlineImage=false;
            fBytes+=file.data.size(); require(fBytes<=22*1024*1024,"Draft attachments exceed the message limit.");
            if(original.contentId.empty()) continue;
            if(counts[original.contentId]==1) fCid[original.contentId]=i;
            if(counts[original.contentId]!=1 || original.contentId.size()>200
                || original.contentId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._-")!=std::string::npos) file.contentId=NewCid();
        }
    }
    Draft Read(std::string_view html)
    {
        require(html.size()<=kClipboardHtmlLimit,"Clipboard HTML exceeds 8 MiB.");
        while(!html.empty() && html.back()=='\0')html.remove_suffix(1);
        require(html.find('\0')==std::string_view::npos,"Clipboard HTML contains invalid text.");
        fDraft.account=1;if(html.empty()){validateDraft(fDraft);return std::move(fDraft);}
        std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> doc(htmlReadMemory(html.data(),html.size(),nullptr,"UTF-8",HTML_PARSE_NONET|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING|HTML_PARSE_NODEFDTD),xmlFreeDoc);
        require(bool(doc),"The clipboard HTML could not be read.");Walk(xmlDocGetRootElement(doc.get()),{},0);validateDraft(fDraft);return std::move(fDraft);
    }
private:
    static std::string NewCid() { auto id=newMessageId(); return id.substr(1,id.size()-2); }
    void PlaceImage(std::size_t index,const Style& style)
    {
        auto& file=fDraft.attachments[index]; file.inlineImage=true;
        if(file.contentId.empty()) file.contentId=NewCid();
        Prefix(style.run); auto marker=imagePlaceholder(file); auto offset=fDraft.text.size(); Append(marker,style.run);
        fDraft.images.push_back({offset,marker.size(),file.contentId}); require(fDraft.images.size()<=500,"HTML contains too many image placements.");
    }
    void Append(const std::string& text,const TextRun& style)
    {
        if(text.empty())return;
        require(text.size()<=2*1024*1024-fDraft.text.size(),"Pasted text exceeds 2 MiB.");
        if(fDraft.runs.empty() || !same(fDraft.runs.back(),style)){auto run=style;run.offset=fDraft.text.size();fDraft.runs.push_back(std::move(run));}
        require(fDraft.runs.size()<=10000,"Clipboard HTML has too many formatting changes.");fDraft.text+=text;
    }
    void Prefix(const TextRun& style)
    {
        if(fBreak && !fDraft.text.empty() && fDraft.text.back()!='\n')Append("\n",{});
        else if(fSpace && !fDraft.text.empty() && fDraft.text.back()!='\n')Append(" ",style);
        fBreak=false;fSpace=false;
    }
    void Text(const std::string& text,const Style& style)
    {
        if(style.pre){Prefix(style.run);std::string normalized;for(std::size_t i=0;i<text.size();++i){if(text[i]=='\r'){normalized+='\n';if(i+1<text.size() && text[i+1]=='\n')++i;}else normalized+=text[i];}Append(normalized,style.run);return;}
        for(std::size_t at=0;at<text.size();) {
            if(space(text[at])){fSpace=true;++at;continue;}
            auto end=at+1;while(end<text.size() && !space(text[end]))++end;
            Prefix(style.run);Append(text.substr(at,end-at),style.run);at=end;
        }
    }
    void Image(xmlNode* node,const Style& style)
    {
        auto source=trim(attribute(node,"src"));auto comma=source.find(',');std::string type;
        if(lower(source.substr(0,4))=="cid:") {
            std::string cid;
            auto hex=[](char c)->int { if(c>='0' && c<='9')return c-'0'; if(c>='a' && c<='f')return c-'a'+10; if(c>='A' && c<='F')return c-'A'+10; return -1; };
            bool valid=true;
            for(std::size_t at=4;at<source.size();++at) {
                if(source[at]!='%')cid+=source[at];
                else if(at+2<source.size() && hex(source[at+1])>=0 && hex(source[at+2])>=0) { cid+=static_cast<char>(hex(source[at+1])*16+hex(source[at+2])); at+=2; }
                else { valid=false; break; }
            }
            auto found=fCid.find(cid);
            if(valid && found!=fCid.end() && fDraft.attachments[found->second].mediaType.rfind("image/",0)==0) { PlaceImage(found->second,style); return; }
        }
        if(comma!=std::string::npos && lower(source.substr(0,5))=="data:") {
            auto info=lower(source.substr(5,comma-5));auto semi=info.find(';');
            if(semi!=std::string::npos && info.substr(semi)==";base64" && previewImageType(info.substr(0,semi)))type=info.substr(0,semi);
        }
        if(type.empty()){auto alt=attribute(node,"alt");Text(alt.empty()?"[Image not included]":alt,style);return;}
        auto encoded=source.substr(comma+1);encoded.erase(std::remove_if(encoded.begin(),encoded.end(),[](unsigned char c){return space(c);}),encoded.end());
        std::string bytes;
        try {bytes=base64Decode(encoded);} catch(const std::runtime_error&) { /* An unusable image keeps its alternative text. */ }
        if(bytes.empty() || base64Encode(bytes)!=encoded){auto alt=attribute(node,"alt");Text(alt.empty()?"[Image not included]":alt,style);return;}
        std::size_t index=fDataBegin;while(index<fDraft.attachments.size() && !(fDraft.attachments[index].mediaType==type && fDraft.attachments[index].data==bytes))++index;
        if(index==fDraft.attachments.size()) {
            require(index<100,"Clipboard HTML contains too many images.");
            fBytes+=bytes.size();require(fBytes<=22*1024*1024,"Clipboard images exceed the message limit.");
            std::string extension=type=="image/jpeg"?"jpg":type=="image/svg+xml"?"svg":type=="image/x-icon" || type=="image/vnd.microsoft.icon"?"ico":type.substr(6);
            auto id=newMessageId();id=id.substr(1,id.size()-2);
            fDraft.attachments.push_back({"Pasted image "+std::to_string(index+1)+'.'+extension,type,id,std::move(bytes),true});
        }
        PlaceImage(index,style);
    }
    void Walk(xmlNode* node,Style inherited,unsigned depth)
    {
        require(depth<=128,"Clipboard HTML is nested too deeply.");
        static const std::set<std::string> skipped={"head","style","script","iframe","object","embed","svg","math","template","noscript","noembed","noframes","title","textarea","select"};
        static const std::set<std::string> blocks={"address","article","blockquote","div","dl","dt","dd","footer","h1","h2","h3","h4","h5","h6","header","hr","main","p","pre","section","table","tr"};
        for(;node;node=node->next) {
            require(++fNodes<=100000,"Clipboard HTML is too complex.");
            if(node->type==XML_TEXT_NODE || node->type==XML_CDATA_SECTION_NODE){if(node->content)Text(reinterpret_cast<const char*>(node->content),inherited);continue;}
            if(node->type!=XML_ELEMENT_NODE)continue;
            auto tag=lower(reinterpret_cast<const char*>(node->name));if(skipped.count(tag) || xmlHasProp(node,reinterpret_cast<const xmlChar*>("hidden")))continue;
            auto style=inherited;auto css=declarations(attribute(node,"style"));if(css["display"]=="none" || css["visibility"]=="hidden")continue;
            if(tag=="b" || tag=="strong" || (tag.size()==2 && tag[0]=='h' && tag[1]>='1' && tag[1]<='6'))style.run.bold=true;
            if(tag=="i" || tag=="em")style.run.italic=true;
            if(tag=="u" || tag=="ins")style.run.underline=true;
            if(tag=="a")style.run.link=safeLink(attribute(node,"href"));
            if(tag=="pre")style.pre=true;
            const auto& weight=css["font-weight"];
            if(weight=="bold" || weight=="bolder" || weight=="600" || weight=="700" || weight=="800" || weight=="900")style.run.bold=true;
            if(weight=="normal" || weight=="lighter" || weight=="100" || weight=="200" || weight=="300" || weight=="400" || weight=="500")style.run.bold=false;
            if(css["font-style"]=="italic" || css["font-style"]=="oblique")style.run.italic=true;else if(css["font-style"]=="normal")style.run.italic=false;
            auto decoration=css.count("text-decoration-line")?css["text-decoration-line"]:css["text-decoration"];
            if(decoration=="none")style.run.underline=false;else if(decoration.find("underline")!=std::string::npos)style.run.underline=true;
            auto whitespace=css["white-space"];
            if(whitespace=="pre" || whitespace=="pre-wrap" || whitespace=="break-spaces")style.pre=true;else if(whitespace=="normal" || whitespace=="nowrap")style.pre=false;
            if(tag=="br"){Prefix(style.run);Append("\n",style.run);continue;}
            if(tag=="img"){Image(node,style);continue;}
            bool block=blocks.count(tag);if(block){fBreak=true;fSpace=false;}
            if(tag=="ul" || tag=="ol") {
                int start=1;try{auto value=attribute(node,"start");if(!value.empty())start=std::clamp(std::stoi(value),-1000000,1000000);}catch(const std::exception&){}
                fLists.push_back({tag=="ol",start});fBreak=true;
            }
            if(tag=="li") {
                fBreak=true;Prefix(style.run);std::string prefix(fLists.empty()?0:(fLists.size()-1)*2,' ');
                prefix+=!fLists.empty() && fLists.back().first?std::to_string(fLists.back().second++)+". ":"• ";Append(prefix,style.run);
            }
            if(tag=="tr")fRows.push_back(false);
            if((tag=="td" || tag=="th") && !fRows.empty()){if(fRows.back()){fSpace=false;Append("\t",style.run);}fRows.back()=true;}
            Walk(node->children,style,depth+1);
            if(tag=="tr")fRows.pop_back();
            if(tag=="ul" || tag=="ol"){fLists.pop_back();fBreak=true;}
            if(block || tag=="li"){fBreak=true;fSpace=false;}
        }
    }
    Draft fDraft;std::size_t fNodes=0,fBytes=0,fDataBegin=0;bool fSpace=false,fBreak=false;
    std::map<std::string,std::size_t> fCid;
    std::vector<std::pair<bool,int>> fLists;std::vector<bool> fRows;
};
}
Draft importClipboardHtml(std::string_view html){return Importer().Read(html);}
Draft importDraftHtml(std::string_view html,const std::vector<Attachment>& files){return Importer(files).Read(html);}
std::string exportClipboardHtml(const Draft& fragment)
{
    auto html=draftHtml(fragment);
    for(const auto& file:fragment.attachments)if(file.inlineImage) {
        auto from="src=\"cid:"+escapeHtml(file.contentId)+'"';
        if(html.find(from)==std::string::npos)continue;
        require(file.data.size()<=(kClipboardHtmlLimit/4)*3,"Clipboard HTML images exceed 8 MiB.");
        auto to="src=\"data:"+file.mediaType+";base64,"+base64Encode(file.data)+'"';
        for(std::size_t at=0;(at=html.find(from,at))!=std::string::npos;at+=to.size()) {
            require(to.size()<=kClipboardHtmlLimit && html.size()-from.size()<=kClipboardHtmlLimit-to.size(),"Clipboard HTML exceeds 8 MiB.");html.replace(at,from.size(),to);
        }
    }
    require(html.size()<=kClipboardHtmlLimit,"Clipboard HTML exceeds 8 MiB.");return html;
}
}
