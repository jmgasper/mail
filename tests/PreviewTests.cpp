#include "mail/Preview.h"
#include <iostream>
#include <nlohmann/json.hpp>
using namespace mail;
namespace {
int count=0;
void check(bool value,const char* why) { ++count; if(!value) throw std::runtime_error(why); }
template<class F> void rejects(F fn,const char* why) { bool rejected=false; try {fn();} catch(const std::exception&) {rejected=true;} check(rejected,why); }
}
int main()
{
    try {
        PreviewSnapshot s; s.key="preview-fixture"; s.from="Sender <sender@example.test>"; auto& d=s.draft;
        d.account=1; d.subject="Preview café 🎉"; d.to=""; d.bcc="hidden@example.test";
        d.attachments={{"garden & café.png","image/png","image@test",std::string("a\0b",3),true},{"note.txt","text/plain",{},"regular",false}};
        d.text="Before 🎉 "; auto at=d.text.size(); auto marker=imagePlaceholder(d.attachments[0]); d.text+=marker+" after <script>alert(1)</script>";
        d.images={{at,marker.size(),"image@test"}}; d.runs={{0,true,true,true,"https://example.test/?a=1&b=2"}};
        auto encoded=encodePreview(s); auto decoded=decodePreview(encoded);
        check(decoded.key==s.key && decoded.from==s.from && decoded.draft.to.empty() && decoded.draft.bcc==d.bcc,"preview permits unfinished recipients and preserves private review headers");
        check(draftHtml(decoded.draft)==draftHtml(d) && decoded.draft.attachments[0].data==d.attachments[0].data,"snapshot round-trips style, positions and binary image data");
        auto html=previewHtml(d,"mail-preview://fixture/");
        check(html.find("src=\"mail-preview://fixture/image/0\"")!=std::string::npos && html.find("cid:")==std::string::npos,"CID images use exact private resource locations");
        check(html.find("Before")<html.find("<img") && html.find("<img")<html.find(" after "),"preview keeps delivery image order");
        check(html.find("href=")==std::string::npos && html.find("title=\"https://example.test/?a=1&amp;b=2\"")!=std::string::npos,"links are inert and keep an escaped tooltip");
        check(html.find("<script>")==std::string::npos && html.find("&lt;script&gt;")!=std::string::npos,"body markup remains text");
        check(html.find(kPreviewPolicy)!=std::string::npos && html.find("emoji.ttf")!=std::string::npos,"preview carries its resource policy and local font");
        check(html.find("note.txt")==std::string::npos,"regular attachment is not inserted into the body");
        auto old=d; old.images.clear(); old.runs.clear(); old.text="Old body";
        check(previewHtml(old,"mail-preview://legacy/").find("src=\"mail-preview://legacy/image/0\"")!=std::string::npos,"legacy inline image is rendered too");
        rejects([&]{previewHtml(d,"https://example.test/");},"external resource root rejected");
        rejects([&]{previewHtml(d,"mail-preview://fixture/\"onload=x/");},"injected resource root rejected");
        for(const char* part:{"version","images","files","text"}) {
            auto broken=nlohmann::json::parse(encoded); broken.erase(part);
            rejects([&]{decodePreview(broken.dump());},"incomplete snapshot rejected");
        }
        auto broken=nlohmann::json::parse(encoded); broken["version"]=2;
        rejects([&]{decodePreview(broken.dump());},"unknown snapshot version rejected");
        broken=nlohmann::json::parse(encoded); broken["files"][0][3]="%%%";
        rejects([&]{decodePreview(broken.dump());},"damaged image encoding rejected");
        broken=nlohmann::json::parse(encoded); broken["images"][0][0]=-1;
        rejects([&]{decodePreview(broken.dump());},"negative image offset rejected");
        broken=nlohmann::json::parse(encoded); broken["runs"][0][4]="javascript:alert(1)";
        rejects([&]{decodePreview(broken.dump());},"unsafe links rejected during restore");
        s.from="Sender\nInjected"; rejects([&]{encodePreview(s);},"injected sender review header rejected");
        std::cout<<count<<" preview snapshot/HTML checks passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
