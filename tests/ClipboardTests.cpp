#include "mail/Clipboard.h"
#include "mail/Mime.h"
#include <iostream>
using namespace mail;
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F fn,const char* why){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}check(rejected,why);}
TextRun at(const Draft& d,const std::string& word){auto offset=d.text.find(word);if(offset==std::string::npos)throw std::runtime_error("Expected pasted word missing: "+word);TextRun run;for(const auto& r:d.runs)if(r.offset<=offset)run=r;return run;}
}
int main()
{
    try {
        auto d=importClipboardHtml("<p> Hello <strong>café <em>🎉</em></strong> <u>world</u>.</p><p>Next &amp; last</p>");
        check(d.text=="Hello café 🎉 world.\nNext & last","HTML whitespace, entities, paragraphs and Unicode become editable text");
        check(at(d,"café").bold && !at(d,"café").italic && at(d,"🎉").bold && at(d,"🎉").italic,"nested semantic formatting keeps UTF-8 offsets");
        check(at(d,"world").underline && !at(d,"world").bold && !at(d,"Next").underline,"inline styles end at their element boundaries");
        d=importClipboardHtml("<b><span style='font-weight:normal;font-style:italic'>Plain italic</span> Bold</b><span style='font-weight:700!important;font-weight:normal;text-decoration-line:underline'> Underlined</span>");
        check(!at(d,"Plain").bold && at(d,"Plain").italic && at(d,"Bold").bold,"CSS can override an inherited semantic face");
        check(at(d,"Underlined").bold && at(d,"Underlined").underline,"supported CSS declarations honor important precedence");
        d=importClipboardHtml("<pre>  first\n\tsecond\r\nthird</pre><div>after<br><br>end</div>");
        check(d.text=="  first\n\tsecond\nthird\nafter\n\nend","preformatted spaces/tabs and explicit line breaks retained");
        d=importClipboardHtml("<ol start=3><li>One</li><li>Two<ul><li>Nested</li></ul></li></ol><table><tr><td>A</td><td>B</td></tr><tr><td>C</td><td>D</td></tr></table>");
        check(d.text=="3. One\n4. Two\n  • Nested\nA\tB\nC\tD","list numbering, nested bullets and table cells keep readable text structure");
        d=importClipboardHtml("<a href='HTTPS://example.test/?a=1&amp;b=2'>Safe</a> <a href='javascript:alert(1)'>Unsafe</a> <a href='mailto:person@example.test'>Mail</a><script>bad()</script><style>bad{}</style><iframe>hidden</iframe><svg>hidden</svg><span hidden>hidden</span><span style='display:none'>hidden</span>");
        check(at(d,"Safe").link=="https://example.test/?a=1&b=2" && at(d,"Unsafe").link.empty() && at(d,"Mail").link=="mailto:person@example.test","only supported link schemes survive, with entity-decoded destinations");
        check(d.text=="Safe Unsafe Mail","active content, stylesheet text and hidden elements are not pasted");
        auto png=base64Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5e8AAAAASUVORK5CYII=");
        auto source="data:image/png;base64,"+base64Encode(png);
        d=importClipboardHtml("Before <b><img src='"+source+"'></b> after <img src='"+source+"'> end");
        check(d.attachments.size()==1 && d.images.size()==2 && d.attachments[0].data==png,"repeated embedded image retains exact bytes with one owned file");
        check(d.text.substr(d.images[0].offset,d.images[0].length)==imagePlaceholder(d.attachments[0]) && d.images[0].offset<d.text.find("after"),"embedded image has a valid editable position");
        check(at(d,imagePlaceholder(d.attachments[0])).bold,"formatting around an embedded image is retained");
        auto html=exportClipboardHtml(d);
        check(html.find("src=\"data:image/png;base64,")!=std::string::npos && html.find("cid:")==std::string::npos,"HTML export is self-contained for other applications");
        auto restored=importClipboardHtml(html);
        check(restored.text==d.text && restored.attachments[0].data==png && restored.images.size()==2,"portable image HTML can be pasted back without binary loss");
        d=importClipboardHtml("<img src='https://example.test/photo' alt='Remote picture'><img src='file:///secret'><img src='cid:not-owned'><img src='data:text/html;base64,PHNjcmlwdD4='><img src='data:image/png;base64,%%%'>");
        check(d.attachments.empty() && d.text.find("Remote picture")==0 && d.text.find("[Image not included]")!=std::string::npos,"remote/file/CID/invalid data images never fetch or attach bytes");
        d=importClipboardHtml("<a href='https://example.test'>A &lt;tag&gt;</a><br>next");html=exportClipboardHtml(d);
        check(html.find("href=\"https://example.test\"")!=std::string::npos && html.find("&lt;tag&gt;")!=std::string::npos,"export preserves escaped text and active portable hyperlink metadata");
        restored=importClipboardHtml(html);check(restored.text==d.text && at(restored,"A").link==at(d,"A").link,"formatted text export round trips links and line breaks");
        d=importClipboardHtml(std::string("<b>Text</b>\0",12));check(d.text=="Text" && at(d,"Text").bold,"clipboard terminator accepted");
        rejects([&]{importClipboardHtml(std::string("a\0b",3));},"embedded NUL rejected");
        rejects([&]{importClipboardHtml(std::string(kClipboardHtmlLimit+1,'x'));},"clipboard input bound enforced");
        rejects([&]{importClipboardHtml("<pre>"+std::string(2*1024*1024+1,'x')+"</pre>");},"composed text limit enforced before paste");
        std::string nested;for(int i=0;i<140;++i)nested+="<div>";nested+='x';for(int i=0;i<140;++i)nested+="</div>";
        rejects([&]{importClipboardHtml(nested);},"clipboard nesting bound enforced");
        Draft large;large.account=1;large.attachments={{"large.png","image/png","large@test",std::string(7*1024*1024,'x'),true}};
        rejects([&]{exportClipboardHtml(large);},"oversized HTML export can fall back to native clipboard formats");
        const std::string seed="<p>A <b>café 🎉</b> <a href='https://example.test'>link</a><img src='data:image/png;base64,eA=='></p>";
        for(std::size_t i=0;i<500;++i){auto input=seed;input[(i*17)%input.size()]=static_cast<char>((i*43)%256);try{validateDraft(importClipboardHtml(input));}catch(const std::invalid_argument&){}}
        std::cout<<checks<<" HTML clipboard checks passed; 500 mutated inputs validated\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL clipboard: "<<e.what()<<'\n';return 1;}
}
