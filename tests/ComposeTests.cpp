#include "mail/Compose.h"
#include "mail/Mime.h"
#include "mail/Store.h"
#include <fstream>
#include <iostream>
#include <atomic>
#include <limits>
#include <thread>
#include <unistd.h>
using namespace mail;
namespace {
int count=0;
void expect(bool value,const char* why) { ++count; if (!value) throw std::runtime_error(why); }
template<class F> void rejects(F fn,const char* why) { bool caught=false; try { fn(); } catch(const std::exception&) { caught=true; } expect(caught,why); }
Account account() { Account a; a.id=1; a.name="Composer test"; a.email="sender@example.test"; a.provider=Provider::Imap; a.incoming={"example.test",993,Security::TLS,"sender",{}}; a.outgoing={"example.test",465,Security::TLS,"sender",{}}; return a; }
Draft fixture()
{
    Draft d; d.account=1; d.messageId=newMessageId(); d.to="\"Doe, Jane\" <jane@example.test>, Café 🌿 <other@example.test>";
    d.cc="jane@example.test"; d.bcc="hidden@example.test"; d.subject=std::string(90,'x')+" Café 🎉";
    d.text="Hello café 🎉\nBold & <safe>\n.leading dot\n"; d.runs={{0,true,false,false,"https://example.test/?a=1&b=2"},{5,false,true,true,{}}};
    d.references={"<parent@example.test>"};
    d.attachments={{"report café 🎉 and a very long filename.bin","application/octet-stream",{},std::string("a\0b",3),false},
        {"picture.png","image/png","inline@test","PNG fixture bytes",true}};
    return d;
}
Draft positionedFixture()
{
    auto d=fixture(); d.subject="Positioned images"; d.runs.clear();
    d.attachments.push_back({"diagram & \"café\".png","image/png","second@test",std::string("\0second\xff",8),true});
    d.attachments.push_back({"legacy.png","image/png","legacy@test","legacy",true});
    d.text="Before café 🎉 ";
    auto place=[&](std::size_t index) {
        const auto& a=d.attachments[index]; auto marker=imagePlaceholder(a);
        d.images.push_back({d.text.size(),marker.size(),a.contentId}); d.text+=marker;
    };
    place(1); d.text+=" middle "; place(2); place(1); d.text+=" after <safe>";
    d.runs={{0,true,false,false,{}},{d.images[0].offset,false,true,false,"https://example.test/?a=1&b=2"},
        {d.images[0].offset+d.images[0].length,false,false,false,{}},
        {d.images.back().offset+d.images.back().length,false,false,true,{}}};
    return d;
}
void positionedImages(const std::string& path)
{
    auto d=positionedFixture(); auto html=draftHtml(d);
    auto first=html.find("cid:inline@test"),second=html.find("cid:second@test"),repeat=html.find("cid:inline@test",first+1);
    expect(html.find("Before café 🎉")<first && first<second && second<repeat && repeat<html.find("after &lt;safe&gt;"),"images render between surrounding text in document order, including reused CID");
    expect(html.find("[Image:")==std::string::npos && html.find("alt=\"diagram &amp; &quot;café&quot;.png\"")!=std::string::npos,"positioned placeholders become images with escaped alternative text");
    expect(html.find("cid:legacy@test")>html.find("</div>") && html.find("cid:inline@test",repeat+1)==std::string::npos,"legacy inline files append once and positioned files are not appended again");
    expect(html.find("<em><img")!=std::string::npos && html.find("</em></a> middle")!=std::string::npos,"image placement respects style and link boundaries");
    auto plain=d; plain.runs.clear(); expect(draftHtml(plain).find(" middle <img")!=std::string::npos,"placement also works with no style runs");
    auto invalid=[&](auto change,const char* why) { auto bad=d; change(bad); rejects([&]{validateDraft(bad);},why); };
    invalid([](Draft& bad){bad.images[0].length=0;},"zero-length image placement rejected");
    invalid([](Draft& bad){bad.images[0].offset=std::numeric_limits<std::size_t>::max();},"overflowing image offset rejected");
    invalid([](Draft& bad){bad.images[0].length=std::numeric_limits<std::size_t>::max();},"overflowing image length rejected");
    invalid([](Draft& bad){bad.images[1].offset=bad.images[0].offset;},"overlapping image placements rejected");
    invalid([](Draft& bad){std::swap(bad.images[0],bad.images[1]);},"unsorted image placements rejected");
    invalid([](Draft& bad){bad.images[0].offset=std::string("Before caf").size()+1;},"image position cannot split UTF-8");
    invalid([](Draft& bad){bad.images[0].contentId="missing@test";},"missing image attachment rejected");
    invalid([](Draft& bad){bad.attachments[1].inlineImage=false;},"regular attachment cannot back an image placement");
    invalid([](Draft& bad){bad.text[bad.images[0].offset]='!';},"damaged placeholder rejected");
    invalid([](Draft& bad){bad.runs[1].offset=bad.images[0].offset+1;},"styles cannot split an image placeholder");
    invalid([](Draft& bad){bad.images.resize(501,bad.images[0]);},"image placement count is bounded");
    auto a=account(); auto wire=composeMessage(d,a,1792542600); auto parsed=parseMessage(wire.raw);
    expect(parsed.text==d.text && parsed.attachments.size()==4 && parsed.html==html,"MIME preserves plain placeholders and references one binary part per image file");
    Id id=0,queued=0;
    {
        Store store(path); store.saveAccount(a); id=store.saveDraft(d); d.id=id;
        auto bad=d; bad.images[0].contentId="missing@test";
        rejects([&]{store.saveDraft(bad);},"invalid placements cannot replace a valid saved draft");
        queued=store.enqueueDraft(id,1792542600);
        expect(parseMessage(store.outbox()[0].raw).html==html,"queued MIME retains positioned images");
    }
    {
        Store store(path); auto saved=store.draft(id);
        expect(saved.images.size()==3 && saved.images[1].offset==d.images[1].offset && saved.images[1].length==d.images[1].length
            && saved.images[1].contentId==d.images[1].contentId && draftHtml(saved)==html,"image ranges and CID survive database reopen");
        expect(saved.attachments[2].data==d.attachments[2].data,"positioned binary image survives database reopen");
        expect(store.returnToDraft(queued)==id && draftHtml(store.draft(id))==html,"return to draft preserves image layout for delivery recovery");
        saved.images.clear(); saved.runs.clear(); saved.text="Replaced body"; store.saveDraft(saved);
        expect(store.draft(id).images.empty(),"replacing draft removes old image placement rows");
        store.removeDraft(id);
        expect(store.drafts().empty(),"draft removal cascades image placement data");
    }
}
}
int main(int argc,char** argv)
{
    char dir[]="mail-compose-XXXXXX"; if (!mkdtemp(dir)) return 2;
    auto path=std::string(dir)+"/mail.db";
    auto cleanup=[&] {
        for (const char* name:{"mail.db-wal","mail.db-shm","mail.db"}) unlink((std::string(dir)+"/"+name).c_str());
        rmdir(dir);
    };
    try {
        positionedImages(path);
        auto a=account(); auto d=fixture(); auto wire=composeMessage(d,a,1792542600); auto parsed=parseMessage(wire.raw);
        expect(wire.recipients.size()==3 && wire.raw.find("hidden@example.test")==std::string::npos,"BCC only belongs to the envelope and recipients are deduplicated");
        expect(parsed.subject==d.subject && parsed.text=="Hello café 🎉\r\nBold & <safe>\r\n.leading dot\r\n","UTF-8 subject/body round-trip with CRLF");
        expect(parsed.attachments.size()==2 && parsed.attachments[1].name==d.attachments[0].name && parsed.attachments[1].data==std::string("a\0b",3),"Unicode continued filenames and binary attachments round-trip");
        expect(parsed.html.find("<strong>")!=std::string::npos && parsed.html.find("&lt;safe&gt;")!=std::string::npos && parsed.html.find("cid:inline@test")!=std::string::npos,"rich styles are escaped and images use CID");
        expect(parsed.references==d.references && parsed.received==1792542600,"reply references and date round-trip");
        auto addresses=parseAddresses(d.to); expect(addresses.size()==2 && addresses[0].name=="Doe, Jane","quoted display-name commas are not recipient separators");
        for (const char* bad:{"victim@example.test\r\nBcc: injected@example.test","a@example.test,","\"broken <a@example.test>","bad@@example.test","a@example.test garbage"})
            rejects([&]{parseAddresses(bad);},"invalid or injected recipients are rejected");
        auto invalid=d; invalid.runs[1].offset=10; rejects([&]{composeMessage(invalid,a,0);},"style boundaries cannot split UTF-8");
        invalid=d; invalid.runs[0].link="javascript:alert(1)"; rejects([&]{composeMessage(invalid,a,0);},"unsafe links are rejected");
        invalid=d; invalid.subject="subject\nBcc: victim@example.test"; rejects([&]{composeMessage(invalid,a,0);},"subject injection is rejected");
        Message source=parseMessage("From: Person <person@example.test>\nReply-To: replies@example.test\nTo: sender@example.test, team@example.test\nCc: team@example.test\nMessage-ID: <parent@example.test>\nSubject: Hello\n\nOriginal body");
        auto reply=replyDraft(source,a,true); expect(reply.to=="replies@example.test, team@example.test" && reply.cc.empty(),"reply-all honors Reply-To, excludes self, deduplicates recipients");
        expect(reply.references==std::vector<std::string>{"<parent@example.test>"} && reply.subject=="Re: Hello","replies preserve ancestry");
        auto forward=forwardDraft(parsed,a); expect(forward.attachments.size()==2 && forward.to.empty() && forward.references.empty(),"forward preserves attachments without joining the original thread");
        Id draftId,outgoingId;
        {
            Store store(path); store.saveAccount(a); d.modified=123; draftId=store.saveDraft(d); d.id=draftId;
            auto saved=store.draft(draftId); expect(saved.bcc==d.bcc && saved.runs[0].link==d.runs[0].link && saved.attachments[0].data==d.attachments[0].data,"draft persists BCC, rich text, links and attachment bytes");
            outgoingId=store.enqueueDraft(draftId,1792542600);
            expect(store.drafts().empty() && store.outbox().size()==1,"queued draft leaves the editable list");
            auto summary=store.outboxSummary();
            expect(summary.total()==1 && summary.queued==1 && !summary.attention(),"queued count is separate from delivery problems");
            auto preview=store.outbox(false);
            expect(preview.size()==1 && preview[0].raw.empty() && preview[0].subject==d.subject && preview[0].recipients==wire.recipients,"outbox preview retains subject/envelope without loading MIME attachments");
            rejects([&]{store.saveDraft(d);},"queued MIME cannot be changed by draft editing");
            rejects([&]{store.removeDraft(draftId);},"queued draft cannot be deleted underneath delivery");
            auto claimed=store.claimOutgoing(); expect(claimed.id==outgoingId && claimed.state=="sending" && !store.claimOutgoing().id,"only one worker can claim a queued message");
            summary=store.outboxSummary(); expect(summary.sending==1 && !summary.queued && !summary.attention(),"claimed messages are counted as sending");
            rejects([&]{store.returnToDraft(outgoingId,true);},"confirmation cannot return an active send to Drafts");
            rejects([&]{store.retryOutgoing(outgoingId,true);},"confirmation cannot queue an active send twice");
            rejects([&]{store.removeAccount(a.id);},"account deletion is blocked during a send");
        }
        {
            Store store(path); store.recoverOutgoing(); auto items=store.outbox();
            expect(items.size()==1 && items[0].state=="uncertain" && !store.claimOutgoing().id,"interrupted sends survive restart without automatic resend");
            auto summary=store.outboxSummary(); expect(summary.total()==1 && summary.uncertain==1 && summary.attention()==1,"delivery attention survives reopening the database");
            rejects([&]{store.retryOutgoing(outgoingId);},"uncertain delivery requires an explicit retry decision");
            rejects([&]{store.returnToDraft(outgoingId);},"uncertain delivery requires an explicit decision before editing");
            store.retryOutgoing(outgoingId,true); auto sending=store.claimOutgoing();
            store.finishOutgoing(sending.id,"failed","Authentication failed",0);
            summary=store.outboxSummary(); expect(summary.failed==1 && summary.attention()==1 && !summary.uncertain,"failed attempts remain in delivery attention");
            auto restored=store.returnToDraft(outgoingId); expect(restored==draftId && store.outbox().empty(),"failed delivery can return to the original editable draft");
            d.subject="Edited after failure"; store.saveDraft(d); outgoingId=store.enqueueDraft(draftId,1792542600); store.claimOutgoing();
            Filter rule; rule.name="Never filter outgoing"; rule.subjectContains="Edited"; rule.moveTo="trash"; store.saveFilter(rule);
            store.finishOutgoing(outgoingId,"sent","",1792542600);
            Query sent; sent.folder="sent"; auto rows=store.conversations(sent,2000000000);
            expect(rows.size()==1 && store.drafts().empty() && store.outbox().empty(),"successful delivery atomically creates Sent and removes the draft");
            expect(!store.outboxSummary().total(),"delivered history is excluded from the persistent indicator");
            expect(store.thread(rows[0].id)[0].subject==d.subject && !store.thread(rows[0].id)[0].unread,"Sent copy reflects the submitted draft and bypasses incoming filters");
            rejects([&]{store.finishOutgoing(outgoingId,"sent","",0);},"late completion cannot create a second Sent copy");
            rejects([&]{store.saveDraft(d);},"stale editor cannot resurrect a sent draft");
        }
        {
            Store sender(path),editor(path);
            for (int round=0;round<24;++round) {
                auto candidate=fixture(); auto draft=sender.saveDraft(candidate); auto id=sender.enqueueDraft(draft,1792542600);
                std::atomic<int> ready{0}; std::atomic<bool> start{false};
                auto wait=[&]{ ++ready; while (!start) std::this_thread::yield(); };
                Outgoing claimed; Id restored=0; std::string sendError,editError;
                std::thread sending([&]{ wait(); try { claimed=sender.claimOutgoing(); } catch (const std::exception& e) { sendError=e.what(); } });
                std::thread editing([&]{ wait(); try { restored=editor.returnToDraft(id); } catch (const std::exception& e) { editError=e.what(); } });
                while (ready!=2) std::this_thread::yield();
                start=true; sending.join(); editing.join();
                expect(sendError.empty(),"claim completes while a separate database connection returns a draft");
                expect(claimed.id ? (!restored && !editError.empty() && sender.outboxSummary().sending==1)
                    : (restored==draft && editError.empty() && !sender.outboxSummary().total()),"concurrent send and return-to-draft have exactly one winner");
                if (claimed.id) { sender.finishOutgoing(id,"failed","Fixture did not submit",0); editor.returnToDraft(id); }
                editor.removeDraft(draft);
            }
        }
        if (argc==2) {
            std::ofstream out(argv[1],std::ios::binary); out<<wire.raw;
            std::ofstream positioned(std::string(argv[1])+".positioned.eml",std::ios::binary);
            positioned<<composeMessage(positionedFixture(),a,1792542600).raw;
        }
        cleanup();
        std::cout<<count<<" composition/outbox assertions passed\n"; return 0;
    } catch (const std::exception& error) { cleanup(); std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
