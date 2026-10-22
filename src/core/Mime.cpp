#include "mail/Mime.h"
#include "mail/Compose.h"
#include "mail/Clipboard.h"

#include <iconv.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mail {
namespace {
constexpr std::size_t kMaxMessage = 32 * 1024 * 1024;
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}
std::string lower(std::string s)
{
    for (char& c : s) if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(c));
    return s;
}
int hex(char c)
{
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
std::string quotedPrintable(std::string_view in, bool header = false)
{
    std::string out;
    for (std::size_t i=0; i<in.size(); ++i) {
        char c=in[i];
        if (header && c=='_') out+=' ';
        else if (c=='=' && i+1<in.size() && in[i+1]=='\n') ++i;
        else if (c=='=' && i+2<in.size() && in[i+1]=='\r' && in[i+2]=='\n') i+=2;
        else if (c=='=' && i+2<in.size() && hex(in[i+1])>=0 && hex(in[i+2])>=0) {
            out+=static_cast<char>(hex(in[i+1])*16+hex(in[i+2])); i+=2;
        } else out+=c;
    }
    return out;
}
std::string utf8(std::string_view text, std::string charset)
{
    if (charset.empty()) charset="UTF-8";
    iconv_t conv=iconv_open("UTF-8",charset.c_str());
    if (conv==reinterpret_cast<iconv_t>(-1)) throw std::runtime_error("This message uses an unsupported text encoding: " + charset);
    std::string result;
    char* input=const_cast<char*>(text.data());
    std::size_t left=text.size();
    while (left) {
        char buffer[4096]; char* output=buffer; std::size_t available=sizeof(buffer);
        auto rc=iconv(conv,&input,&left,&output,&available);
        result.append(buffer,static_cast<std::size_t>(output-buffer));
        if (rc==static_cast<std::size_t>(-1) && errno!=E2BIG) {
            if (errno!=EILSEQ && errno!=EINVAL) { iconv_close(conv); throw std::runtime_error("Could not decode message text."); }
            result+="\xef\xbf\xbd"; ++input; --left;
            iconv(conv,nullptr,nullptr,nullptr,nullptr);
        }
    }
    iconv_close(conv);
    std::replace(result.begin(),result.end(),'\0',' ');
    return result;
}
std::string decodeWords(std::string_view text)
{
    std::string out; bool encoded=false;
    for (std::size_t at=0; at<text.size();) {
        auto start=text.find("=?",at);
        if (start==std::string_view::npos) { out+=text.substr(at); break; }
        auto charsetEnd=text.find('?',start+2);
        auto end=charsetEnd==std::string_view::npos ? std::string_view::npos : text.find("?=",charsetEnd+3);
        if (end==std::string_view::npos || charsetEnd+2>=text.size() || text[charsetEnd+2]!='?') {
            out+=text.substr(at,start+2-at); at=start+2; encoded=false; continue;
        }
        auto gap=text.substr(at,start-at);
        if (!encoded || !trim(gap).empty()) out+=gap;
        try {
            char encoding=text[charsetEnd+1];
            auto bytes=text.substr(charsetEnd+3,end-charsetEnd-3);
            std::string decoded;
            if (encoding=='B' || encoding=='b') decoded=base64Decode(bytes);
            else if (encoding=='Q' || encoding=='q') decoded=quotedPrintable(bytes,true);
            else throw std::runtime_error("Unknown encoded word");
            out+=utf8(decoded,std::string(text.substr(start+2,charsetEnd-start-2)));
            encoded=true;
        } catch (const std::runtime_error&) { out+=text.substr(start,end+2-start); encoded=false; }
        at=end+2;
    }
    return out;
}
struct Part {
    std::map<std::string,std::string> headers;
    std::set<std::string> duplicates;
    std::string_view body;
};
Part splitPart(std::string_view input)
{
    Part p;
    std::string last;
    std::size_t at=0;
    while (at<input.size()) {
        auto end=input.find('\n',at);
        if (end==std::string_view::npos) end=input.size();
        if (end>65536) throw std::runtime_error("The message headers are too large.");
        auto line=input.substr(at,end-at);
        if (!line.empty() && line.back()=='\r') line.remove_suffix(1);
        at=end<input.size()?end+1:end;
        if (line.empty()) { p.body=input.substr(at); return p; }
        if (line.front()==' ' || line.front()=='\t') {
            if (!last.empty()) p.headers[last]+=' '+trim(line);
            continue;
        }
        auto colon=line.find(':');
        if (colon==std::string_view::npos || colon==0) throw std::runtime_error("This file does not contain valid mail headers.");
        last=lower(std::string(line.substr(0,colon)));
        // Keep the first occurrence: duplicate security-sensitive headers are not joined.
        if (!p.headers.count(last)) p.headers[last]=trim(line.substr(colon+1));
        else { p.duplicates.insert(last); last.clear(); }
    }
    return p;
}
struct ContentType { std::string value; std::map<std::string,std::string> params; };
ContentType contentType(std::string_view text)
{
    ContentType type;
    auto semicolon=text.find(';');
    type.value=lower(trim(text.substr(0,semicolon)));
    if (semicolon==std::string_view::npos) return type;
    std::size_t at=semicolon+1;
    while (at<text.size()) {
        while (at<text.size() && (text[at]==';' || std::isspace(static_cast<unsigned char>(text[at])))) ++at;
        auto equal=text.find('=',at);
        if (equal==std::string_view::npos) break;
        std::string key=lower(trim(text.substr(at,equal-at)));
        at=equal+1;
        while (at<text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
        std::string value;
        if (at<text.size() && text[at]=='"') {
            ++at;
            while (at<text.size() && text[at]!='"') {
                if (text[at]=='\\' && at+1<text.size()) ++at;
                value+=text[at++];
            }
            if (at<text.size()) ++at;
        } else {
            auto end=text.find(';',at);
            if (end==std::string_view::npos) end=text.size();
            value=trim(text.substr(at,end-at)); at=end;
        }
        type.params.emplace(std::move(key),std::move(value));
    }
    return type;
}
std::string parameter(const ContentType& type,const std::string& key)
{
    if (type.params.count(key+"*0*") || type.params.count(key+"*0")) {
        std::string value,charset="utf-8";
        for (int n=0;n<100;++n) {
            auto prefix=key+"*"+std::to_string(n);
            auto part=type.params.find(prefix+"*"); bool encoded=part!=type.params.end();
            if (!encoded) part=type.params.find(prefix);
            if (part==type.params.end()) break;
            auto bytes=part->second;
            if (encoded && n==0) {
                auto first=bytes.find('\''),second=first==std::string::npos?first:bytes.find('\'',first+1);
                if (second!=std::string::npos) { charset=bytes.substr(0,first); bytes=bytes.substr(second+1); }
            }
            for (std::size_t i=0;i<bytes.size();++i) {
                if (encoded && bytes[i]=='%' && i+2<bytes.size() && hex(bytes[i+1])>=0 && hex(bytes[i+2])>=0) {
                    value+=static_cast<char>(hex(bytes[i+1])*16+hex(bytes[i+2])); i+=2;
                } else value+=bytes[i];
            }
        }
        return utf8(value,charset);
    }
    auto extended=type.params.find(key+"*");
    if (extended!=type.params.end()) {
        auto first=extended->second.find('\'');
        auto second=first==std::string::npos?first:extended->second.find('\'',first+1);
        if (second!=std::string::npos) {
            std::string value;
            for (std::size_t i=second+1;i<extended->second.size();++i) {
                const auto& s=extended->second;
                if (s[i]=='%' && i+2<s.size() && hex(s[i+1])>=0 && hex(s[i+2])>=0) {
                    value+=static_cast<char>(hex(s[i+1])*16+hex(s[i+2])); i+=2;
                } else value+=s[i];
            }
            return utf8(value,extended->second.substr(0,first));
        }
    }
    auto found=type.params.find(key);
    return found==type.params.end()?std::string():decodeWords(found->second);
}
std::vector<std::string> identifiers(std::string_view text)
{
    std::vector<std::string> out;
    std::size_t at=0;
    while ((at=text.find('<',at))!=std::string_view::npos) {
        auto end=text.find('>',at+1);
        if (end==std::string_view::npos) break;
        auto value=text.substr(at,end-at+1);
        if (value.size()<=1000 && value.find_first_of("\r\n\t ")==std::string_view::npos) out.emplace_back(value);
        at=end+1;
    }
    return out;
}
Timestamp parseDate(std::string date)
{
    auto comma=date.find(','); if (comma!=std::string::npos) date=date.substr(comma+1);
    std::istringstream in(date); in.imbue(std::locale::classic());
    int day=0,year=0,hour=0,minute=0,second=0; char colon;
    std::string month,zone;
    if (!(in>>day>>month>>year>>hour>>colon>>minute) || colon!=':') return 0;
    if (in.peek()==':') { in.get(); if (!(in>>second)) return 0; }
    if (!(in>>zone)) return 0;
    static const std::string months="janfebmaraprmayjunjulaugsepoctnovdec";
    auto m=months.find(lower(month));
    if (month.size()!=3 || m==std::string::npos || m%3) return 0;
    int mon=static_cast<int>(m/3)+1;
    if (year<50) year+=2000; else if (year<100) year+=1900;
    if (year<1601 || year>9999 || day<1 || day>31 || hour<0 || hour>23 || minute<0 || minute>59 || second<0 || second>60) return 0;
    int offset=0;
    if (zone.size()==5 && (zone[0]=='+' || zone[0]=='-') && std::all_of(zone.begin()+1,zone.end(),[](unsigned char c){return std::isdigit(c);})) {
        int hours=std::stoi(zone.substr(1,2)),minutes=std::stoi(zone.substr(3,2));
        if (hours>23 || minutes>59) return 0;
        offset=(hours*60+minutes)*(zone[0]=='-'?-1:1);
    } else {
        const std::map<std::string,int> zones={{"ut",0},{"gmt",0},{"est",-300},{"edt",-240},{"cst",-360},{"cdt",-300},{"mst",-420},{"mdt",-360},{"pst",-480},{"pdt",-420}};
        auto z=zones.find(lower(zone)); if (z==zones.end()) return 0; offset=z->second;
    }
    // Gregorian civil date to days since 1970-01-01, independent of local TZ.
    year-=mon<=2;
    int era=year/400, yoe=year-era*400;
    int doy=(153*(mon+(mon>2?-3:9))+2)/5+day-1;
    int days=era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
    return static_cast<Timestamp>(days)*86400+hour*3600+minute*60+second-offset*60;
}
void parseBody(const Part& p,Message& m,int depth,int& count)
{
    if (depth>20 || ++count>512) throw std::runtime_error("This message contains too many nested parts.");
    auto header=[&](const char* name) { auto it=p.headers.find(name); return it==p.headers.end()?std::string():it->second; };
    auto type=contentType(header("content-type"));
    auto disposition=contentType(header("content-disposition"));
    if (type.value.empty()) type.value="text/plain";
    if (type.value.compare(0,10,"multipart/")==0) {
        auto boundary=parameter(type,"boundary");
        if (boundary.empty() || boundary.size()>200 || boundary.find_first_of("\r\n")!=std::string::npos)
            throw std::runtime_error("This message has an invalid MIME boundary.");
        const auto delimiter="--"+boundary;
        std::size_t at=0,start=std::string_view::npos; bool closed=false;
        while (at<p.body.size()) {
            auto end=p.body.find('\n',at); if (end==std::string_view::npos) end=p.body.size();
            auto line=p.body.substr(at,end-at);
            while (!line.empty() && (line.back()=='\r' || line.back()==' ' || line.back()=='\t')) line.remove_suffix(1);
            bool final=line==delimiter+"--";
            if (line==delimiter || final) {
                if (start!=std::string_view::npos) {
                    auto part=p.body.substr(start,at-start);
                    if (!part.empty() && part.back()=='\n') part.remove_suffix(1);
                    if (!part.empty() && part.back()=='\r') part.remove_suffix(1);
                    parseBody(splitPart(part),m,depth+1,count);
                }
                start=end<p.body.size()?end+1:end;
                if (final) { closed=true; break; }
            }
            at=end<p.body.size()?end+1:end;
        }
        if (!closed) throw std::runtime_error("This multipart message is incomplete.");
        return;
    }
    std::string data;
    auto encoding=lower(trim(header("content-transfer-encoding")));
    if (encoding=="base64") data=base64Decode(p.body);
    else if (encoding=="quoted-printable") data=quotedPrintable(p.body);
    else if (encoding.empty() || encoding=="7bit" || encoding=="8bit" || encoding=="binary") data=p.body;
    else throw std::runtime_error("This message uses an unsupported transfer encoding.");
    std::string name=parameter(disposition,"filename");
    if (name.empty()) name=parameter(type,"name");
    bool attachment=disposition.value=="attachment" || !name.empty()
        || (type.value!="text/plain" && type.value!="text/html");
    if (attachment) {
        // Use only a leaf filename when offering Save; never interpret sender paths.
        auto slash=name.find_last_of("/\\"); if (slash!=std::string::npos) name=name.substr(slash+1);
        for (char& c : name) if (static_cast<unsigned char>(c)<32 || c==127) c='_';
        if (name.empty() || name=="." || name=="..") name="attachment";
        auto cid=identifiers(header("content-id"));
        std::string contentId=cid.empty()?std::string():cid.front().substr(1,cid.front().size()-2);
        m.attachments.push_back({name,type.value,contentId,std::move(data),disposition.value=="inline" && type.value.compare(0,6,"image/")==0});
    } else {
        data=utf8(data,parameter(type,"charset"));
        auto& target=type.value=="text/html"?m.html:m.text;
        if (!target.empty()) target+='\n';
        target+=data;
    }
}
std::string codepoint(unsigned long cp)
{
    if (cp==0 || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return "\xef\xbf\xbd";
    std::string out;
    if (cp<0x80) out+=static_cast<char>(cp);
    else if (cp<0x800) { out+=static_cast<char>(0xc0|(cp>>6)); out+=static_cast<char>(0x80|(cp&63)); }
    else if (cp<0x10000) { out+=static_cast<char>(0xe0|(cp>>12)); out+=static_cast<char>(0x80|((cp>>6)&63)); out+=static_cast<char>(0x80|(cp&63)); }
    else { out+=static_cast<char>(0xf0|(cp>>18)); out+=static_cast<char>(0x80|((cp>>12)&63)); out+=static_cast<char>(0x80|((cp>>6)&63)); out+=static_cast<char>(0x80|(cp&63)); }
    return out;
}
}

std::string base64Encode(std::string_view bytes)
{
    std::string out;
    for (std::size_t i=0;i<bytes.size();i+=3) {
        unsigned a=static_cast<unsigned char>(bytes[i]);
        unsigned b=i+1<bytes.size()?static_cast<unsigned char>(bytes[i+1]):0;
        unsigned c=i+2<bytes.size()?static_cast<unsigned char>(bytes[i+2]):0;
        out+=alphabet[a>>2]; out+=alphabet[((a&3)<<4)|(b>>4)];
        out+=i+1<bytes.size()?alphabet[((b&15)<<2)|(c>>6)]:'=';
        out+=i+2<bytes.size()?alphabet[c&63]:'=';
    }
    return out;
}
std::string base64Decode(std::string_view text)
{
    std::string out;
    unsigned value=0; int bits=0,padding=0,symbols=0;
    for (unsigned char c : text) {
        if (std::isspace(c)) continue;
        ++symbols;
        if (c=='=') { ++padding; if (padding>2) throw std::runtime_error("Invalid base64 padding."); continue; }
        if (padding) throw std::runtime_error("Invalid base64 data after padding.");
        auto p=std::string_view(alphabet,64).find(static_cast<char>(c));
        if (p==std::string_view::npos) throw std::runtime_error("Invalid base64 character.");
        value=(value<<6)|static_cast<unsigned>(p); bits+=6;
        if (bits>=8) { bits-=8; out+=static_cast<char>((value>>bits)&255); }
    }
    if (symbols%4!=0 || (padding==1 && bits!=2) || (padding==2 && bits!=4) || (padding==0 && bits!=0)
        || (bits && (value&((1u<<bits)-1))!=0)) throw std::runtime_error("Incomplete base64 data.");
    return out;
}
std::string escapeHtml(std::string_view text)
{
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out+="&amp;"; break; case '<': out+="&lt;"; break;
            case '>': out+="&gt;"; break; case '"': out+="&quot;"; break;
            case '\'': out+="&#39;"; break; default: out+=c;
        }
    }
    return out;
}
std::string plainFromHtml(std::string_view html)
{
    std::string out;
    const std::string lowered=lower(std::string(html));
    const std::map<std::string,std::string> entities={{"amp","&"},{"lt","<"},{"gt",">"},{"quot","\""},{"apos","'"},{"nbsp"," "}};
    for (std::size_t i=0;i<html.size();) {
        if (html[i]=='<') {
            if (html.substr(i,4)=="<!--") { auto end=html.find("-->",i+4); if (end==std::string_view::npos) break; i=end+3; continue; }
            auto end=html.find('>',i+1); if (end==std::string_view::npos) break;
            auto tag=lower(trim(html.substr(i+1,end-i-1)));
            auto space=tag.find_first_of(" \t\r\n/");
            auto name=tag.substr(0,space);
            if (name=="script" || name=="style" || name=="head") {
                auto close=lowered.find("</"+name,end+1);
                if (close==std::string::npos) break;
                auto finish=html.find('>',close); if (finish==std::string_view::npos) break;
                i=finish+1; continue;
            }
            if (tag=="/p" || tag=="/div" || tag=="/li" || name=="br" || tag=="/tr") out+='\n';
            i=end+1;
        } else if (html[i]=='&') {
            auto end=html.find(';',i+1);
            if (end!=std::string_view::npos && end-i<=12) {
                std::string entity(html.substr(i+1,end-i-1)); auto found=entities.find(entity);
                if (found!=entities.end()) { out+=found->second; i=end+1; continue; }
                if (!entity.empty() && entity[0]=='#') {
                    try {
                        bool hexadecimal=entity.size()>1 && (entity[1]=='x' || entity[1]=='X');
                        std::string digits=entity.substr(hexadecimal?2:1); std::size_t used=0;
                        auto cp=std::stoul(digits,&used,hexadecimal?16:10);
                        if (used==digits.size()) { out+=codepoint(cp); i=end+1; continue; }
                    } catch (const std::exception&) { }
                }
            }
            out+=html[i++];
        } else {
            char c=html[i++];
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!out.empty() && !std::isspace(static_cast<unsigned char>(out.back()))) out+=' ';
            } else out+=c;
        }
    }
    return trim(out);
}
Message parseMessage(std::string_view raw)
{
    if (raw.size()>kMaxMessage) throw std::runtime_error("This message exceeds the current 32 MiB import limit.");
    auto part=splitPart(raw);
    if (part.headers.empty()) throw std::runtime_error("This file does not contain an email message.");
    Message m;
    m.raw=raw;
    m.sender=decodeWords(part.headers["from"]); m.replyTo=decodeWords(part.headers["reply-to"]);
    m.to=decodeWords(part.headers["to"]); m.cc=decodeWords(part.headers["cc"]);
    m.subject=decodeWords(part.headers["subject"]);
    auto ids=identifiers(part.headers["message-id"]); if (!ids.empty()) m.messageId=ids.front();
    m.references=identifiers(part.headers["references"]);
    auto reply=identifiers(part.headers["in-reply-to"]);
    for (const auto& id : reply) if (std::find(m.references.begin(),m.references.end(),id)==m.references.end()) m.references.push_back(id);
    m.received=parseDate(part.headers["date"]);
    int parts=0; parseBody(part,m,0,parts);
    if (m.text.empty() && !m.html.empty()) m.text=plainFromHtml(m.html);
    return m;
}
static void decodeAddressNames(std::vector<Mailbox>& addresses)
{
    for(auto& address:addresses) {
        address.name=decodeWords(address.name);
        if(address.name.size()>512) throw std::invalid_argument("The recipient name is too long.");
        for(unsigned char c:address.name) if(c<32 || c==127) throw std::invalid_argument("The recipient name contains invalid characters.");
    }
}
std::vector<Mailbox> parseMessageAddresses(std::string_view raw,const std::string& header)
{
    if(header!="to" && header!="cc" && header!="bcc") throw std::invalid_argument("Unsupported recipient header.");
    if(raw.size()>kMaxMessage) throw std::invalid_argument("The message exceeds 32 MiB.");
    auto part=splitPart(raw);
    if(part.duplicates.count(header)) throw std::invalid_argument("The message contains a duplicate recipient header.");
    const auto& source=part.headers[header];
    if(lower(trim(source))=="undisclosed-recipients:;") return {};
    auto addresses=parseAddresses(source,8192); decodeAddressNames(addresses); return addresses;
}
Draft parseDraftMessage(std::string_view raw,Id account,const std::string& providerThread)
{
    auto message=parseMessage(raw); auto part=splitPart(raw);
    for(const auto* name:{"to","cc","bcc","from","reply-to","subject","message-id","references","in-reply-to","content-type","content-transfer-encoding"})
        if(part.duplicates.count(name)) throw std::invalid_argument("The draft contains duplicate message headers. Its original content must be reviewed before editing.");
    auto recipients=[&](const char* name) {
        const auto& source=part.headers[name];
        if(lower(trim(source))=="undisclosed-recipients:;") return std::string();
        std::vector<Mailbox> addresses;
        try { addresses=parseAddresses(source,8192); }
        catch(const std::invalid_argument&) { return source; } // Preserve unfinished input without reinterpreting encoded punctuation.
        decodeAddressNames(addresses);
        std::string result;
        for(const auto& address:addresses) {
            if(!result.empty()) result+=", ";
            if(address.name.empty()) { result+=address.address; continue; }
            // Decode names after parsing address boundaries. An encoded comma
            // belongs to its display name, not to a second recipient.
            result+='"';
            for(char c:address.name) { if(c=='"' || c=='\\') result+='\\'; result+=c; }
            result+="\" <"+address.address+">";
        }
        return result;
    };
    auto draft=importDraftHtml(message.html,message.attachments);
    if(message.html.empty()) {
        for(std::size_t i=0;i<message.text.size();++i) {
            if(message.text[i]=='\r') { draft.text+='\n'; if(i+1<message.text.size() && message.text[i+1]=='\n') ++i; }
            else draft.text+=message.text[i];
        }
    }
    draft.account=account; draft.providerThread=providerThread;
    draft.messageId=message.messageId.empty()?newMessageId():message.messageId;
    draft.to=recipients("to"); draft.cc=recipients("cc"); draft.bcc=recipients("bcc");
    draft.subject=message.subject; draft.references=std::move(message.references); draft.modified=message.received;
    validateDraft(draft); return draft;
}
}
