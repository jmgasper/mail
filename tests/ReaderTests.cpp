#include "mail/Preview.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace mail;
namespace {
int checks=0;
void check(bool value,const char* why) {++checks;if(!value) throw std::runtime_error(why);}
template<class F> void rejects(F fn,const char* why) {bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}check(rejected,why);}
}
int main()
{
    try {
        PreviewSnapshot snapshot; snapshot.key="received-fixture"; snapshot.received.emplace();auto& m=*snapshot.received;
        m.id=7;m.from="Sender café 🎉 <sender@example.test>";m.to="reader@example.test";m.cc="copy@example.test";m.subject="A received message";m.date="2026-10-21";
        m.text="Plain <text> café 🎉";m.thread={{6,"Earlier sender","2026-10-20"},{7,m.from,m.date}};
        m.files={{"inline.png","image/png","image@test",std::string("x\0y",3),false},{"data.txt","text/plain","text@test","plain bytes",false}};
        m.html="<html><head><style>.notice{color:#123456}</style></head><body><table cellpadding=4><tr><td class=notice style='font-weight:bold'>Hello café 🎉</td></tr></table><p>Before <img src='CID:image%40test' alt='A &amp; B'> after.</p><a href='https://example.test/?a=1&amp;b=2'>Link</a></body></html>";
        auto encoded=encodePreview(snapshot);auto decoded=decodePreview(encoded);
        check(decoded.received && decoded.key==snapshot.key && decoded.received->id==7,"received snapshot identity round trips");
        check(decoded.received->html==m.html && decoded.received->files[0].data==m.files[0].data,"received HTML and binary bytes round trip independently of composer validation");
        check(decoded.received->thread.size()==2 && decoded.received->thread[0].id==6,"conversation choices survive the transfer");
        auto render=[&]{return receivedHtml(m,"mail-preview://reader/");};auto html=render();
        check(html.find("<table cellpadding=\"4\">")!=std::string::npos && html.find("font-weight:bold")!=std::string::npos && html.find(".notice{color:#123456}")!=std::string::npos,"email tables, classes and inline and embedded styles retained");
        check(html.find("Hello café 🎉")!=std::string::npos,"Unicode remains intact");
        check(html.find("src=\"mail-preview://reader/image/0\"")!=std::string::npos,"CID scheme and percent escapes resolve to attached image even without inline disposition");
        check(html.find("Before")<html.find("<img") && html.find("<img")<html.find(" after."),"received inline image retains its body position");
        check(html.find("href=")==std::string::npos && html.find("title=\"https://example.test/?a=1&amp;b=2\"")!=std::string::npos,"links retain their destination as inert text tooltips");
        check(html.find("alt=\"A &amp; B\"")!=std::string::npos && html.find(kPreviewPolicy)!=std::string::npos,"attribute text escaped and resource policy present");
        auto plain=receivedHtml(m,"mail-preview://reader/",true);
        check(plain.find("Plain &lt;text&gt; café 🎉")!=std::string::npos && plain.find("<table")==std::string::npos && plain.find("<img")==std::string::npos,"plain-text toggle uses the original alternative without HTML or images");
        m.html="<script>window.injected=1</script><base href='https://example.test/'><meta http-equiv=refresh content='0;url=https://example.test/'><link rel=preconnect href='https://example.test/'><iframe srcdoc='<script>bad()</script>'></iframe><object data='file:///boot/home/secret'></object><svg><a href='javascript:bad()'>svg</a></svg><math><mtext>math</mtext></math><form action='https://example.test/'><input autofocus onfocus='bad()'><button formaction='https://example.test/'>Button text</button></form><p onclick='bad()' contenteditable=true>Hello</p>";
        html=render();
        for(const auto* fragment:{"<script","<base","http-equiv=refresh","<link","<iframe","<object","<svg","<math","<form","<input","<button","onclick=","contenteditable="})
            check(html.find(fragment)==std::string::npos,"active elements and event/edit attributes removed");
        check(html.find("Button text")!=std::string::npos && html.find("Hello")!=std::string::npos,"readable text survives discarded active wrappers");
        m.html="<img src='https://example.test/pixel' alt='Tracking image'><img src='file:///boot/home/secret'><img src='data:image/svg+xml,bad'><img srcset='https://example.test/a 1x' src='cid:missing'><img src='cid:text@test'><img src='cid:image%00@test'><img src='cid:image%zztest'>";
        html=render();check(html.find("<img")==std::string::npos && html.find("Tracking image")!=std::string::npos,"external, local-file, data, missing and non-image resources become placeholders");
        m.html="<img src='cid:image@test'>";m.files.push_back(m.files[0]);
        check(render().find("<img")==std::string::npos,"duplicate Content-IDs are not guessed");m.files.pop_back();
        m.html="<!doctype html SYSTEM 'https://example.test/evil.dtd'><p>&xxe;</p><p title='&quot; onmouseover=&quot;bad()'>Safe &lt;script&gt;</p>";
        html=render();check(html.find("evil.dtd")==std::string::npos && html.find("&lt;script&gt;")!=std::string::npos && html.find("title=\"&quot; onmouseover=&quot;bad()\"")!=std::string::npos,"DTD discarded and entity-decoded attributes re-escaped");
        m.html="<style>p::after{content:'</StYle><img src=x onerror=bad()>'}</style><p>After malformed style</p>";
        html=render();check(html.find("onerror=")==std::string::npos && html.find("After malformed style")!=std::string::npos,"malformed raw style cannot preserve active markup");
        m.html.clear();check(render().find("Plain &lt;text&gt;")!=std::string::npos,"plain-only received messages render as text");
        rejects([&]{receivedHtml(m,"https://example.test/");},"received resource root cannot be external");
        auto invalid=nlohmann::json::parse(encoded);invalid["files"][0][3]="%%%";rejects([&]{decodePreview(invalid.dump());},"damaged received bytes rejected");
        invalid=nlohmann::json::parse(encoded);invalid["thread"][0][0]=7;rejects([&]{decodePreview(invalid.dump());},"duplicate conversation identities rejected");
        invalid=nlohmann::json::parse(encoded);invalid["id"]=999;rejects([&]{decodePreview(invalid.dump());},"selected message must belong to transferred conversation");
        m.text.assign(8*1024*1024+1,'x');rejects([&]{encodePreview(snapshot);},"oversized received body is bounded");m.text="text";
        m.html=std::string(200,' ');for(int i=0;i<140;++i)m.html+="<div>";m.html+="nested";for(int i=0;i<140;++i)m.html+="</div>";
        rejects([&]{render();},"excessive HTML depth uses the reader fallback");
        const std::string seed="<table><tr><td style='color:red'>café &amp; text</td></tr></table><img src='cid:image@test'><svg><script>bad()</script></svg>";
        for(std::size_t i=0;i<500;++i) {m.html=seed;auto pos=(i*17)%seed.size();m.html[pos]=static_cast<char>((i*43)%256);try{render();}catch(const std::invalid_argument&){}}
        std::cout<<checks<<" received reader checks passed; 500 mutated HTML inputs parsed under bounded limits\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL reader: "<<error.what()<<'\n';return 1;}
}
