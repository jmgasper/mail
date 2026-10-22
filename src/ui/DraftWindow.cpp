#include "DraftWindow.h"
#include "Emoji.h"
#include <Alert.h>
#include <Button.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <algorithm>

namespace mail::ui {
namespace {
constexpr uint32 kRefresh='gdrf',kSelected='gdsl',kKeep='gdkp',kMatch='gdms',kUse='gdus',kRetry='gdrt';
bool confirm(const char* title,const char* text,const char* action)
{
    auto* alert=new BAlert(title,text,"Cancel",action,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT);
    alert->SetShortcut(0,B_ESCAPE); alert->SetDefaultButton(alert->ButtonAt(0)); return alert->Go()==1;
}
class DraftItem : public BStringItem {
public:
    explicit DraftItem(const std::string& text):BStringItem(text.c_str()) {}
    void DrawItem(BView* owner,BRect frame,bool) override {
        const auto background=ui_color(IsSelected()?B_LIST_SELECTED_BACKGROUND_COLOR:B_LIST_BACKGROUND_COLOR);
        owner->SetHighColor(background); owner->FillRect(frame); owner->SetLowColor(background);
        owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_ITEM_TEXT_COLOR:B_LIST_ITEM_TEXT_COLOR));
        BFont font; owner->GetFont(&font); DrawEmojiText(*owner,Text(),font,{frame.left+5,frame.top+BaselineOffset()},frame.Width()-10);
    }
};
std::string preview(const Draft& d)
{
    auto text=(d.subject.empty()?"(No subject)":d.subject)+"\nTo: "+d.to;
    if (!d.cc.empty()) text+="\nCc: "+d.cc;
    if (!d.bcc.empty()) text+="\nBcc: "+d.bcc;
    if (!d.attachments.empty()) text+="\nAttachments: "+std::to_string(d.attachments.size());
    auto end=std::min<std::size_t>(4000,d.text.size());
    while (end && end<d.text.size() && (static_cast<unsigned char>(d.text[end])&0xc0)==0x80) --end;
    return text+"\n"+d.text.substr(0,end)+(end<d.text.size()?"…":"");
}
}
DraftWindow::DraftWindow(BWindow* owner,std::shared_ptr<Store> store)
    :BWindow(BRect(165,90,945,710),"Draft synchronization",B_TITLED_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS),fStore(std::move(store)),fOwner(owner)
{
    fList=new BListView("draft sync list"); fList->SetSelectionMessage(new BMessage(kSelected));
    auto* list=new BScrollView("draft sync scroll",fList,0,false,true); list->SetExplicitMinSize(BSize(600,90));
    list->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,110));
    fMatchesList=new BListView("draft copies"); fMatchesList->SetSelectionMessage(new BMessage(kMatch));
    auto* copies=new BScrollView("draft copies scroll",fMatchesList,0,false,true);
    copies->SetExplicitMinSize(BSize(600,45)); copies->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,65));
    fMatchesPanel=new BGroupView("draft copies panel",B_VERTICAL);
    BLayoutBuilder::Group<>(fMatchesPanel).Add(new BStringView("draft copies label","Copies found on Gmail — select one to review")).Add(copies);
    fDetails=new EmojiTextView("draft sync details"); fDetails->MakeEditable(false); fDetails->SetWordWrap(true); fDetails->SetInsets(8,8,8,8);
    auto* details=new BScrollView("draft details scroll",fDetails,0,false,true); details->SetExplicitMinSize(BSize(600,220));
    fCheck=new BButton("check drafts","Check Gmail",new BMessage(kCheckDrafts));
    fKeep=new BButton("keep draft versions","Keep both versions",new BMessage(kKeep));
    fUse=new BButton("use gmail copy","Use Gmail copy…",new BMessage(kUse));
    fRetry=new BButton("retry draft save","Retry save…",new BMessage(kRetry));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS).Add(list).Add(fMatchesPanel).Add(details,1)
        .AddGroup(B_HORIZONTAL).Add(fCheck).Add(fKeep).Add(fUse).Add(fRetry).AddGlue().Add(new BButton("Close",new BMessage(B_QUIT_REQUESTED))).End();
    BMessage tick(kRefresh); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,1000000); Refresh(); CenterIn(owner->Frame());
}
DraftWindow::~DraftWindow() { for (auto* list:{fList,fMatchesList}) while (auto* item=list->RemoveItem(int32(0))) delete item; }
void DraftWindow::Refresh()
{
    auto index=fList->CurrentSelection(); Id selected=index>=0 && static_cast<std::size_t>(index)<fItems.size()?fItems[index].id:0;
    auto items=fStore->gmailDraftStates();
    bool changed=items.size()!=fItems.size();
    for (std::size_t i=0;!changed && i<items.size();++i) changed=items[i].id!=fItems[i].id || items[i].revision!=fItems[i].revision || items[i].draft!=fItems[i].draft;
    fItems=std::move(items);
    if (changed) {
        while (auto* item=fList->RemoveItem(int32(0))) delete item;
        const auto accounts=fStore->accounts();
        for (std::size_t i=0;i<fItems.size();++i) {
            const auto& state=fItems[i]; std::string text;
            for (const auto& account:accounts) if (account.id==state.account) text=account.name+" · ";
            if (state.draft) { const auto subject=fStore->draft(state.draft).subject; text+=subject.empty()?"(No subject)":subject; }
            else text+="Discarded draft";
            text+=state.active?" · Checking":state.conflict?" · Versions differ":!state.error.empty()?" · Needs attention":"";
            for (char& c:text) if (c=='\r' || c=='\n') c=' ';
            fList->AddItem(new DraftItem(text)); if (state.id==selected) fList->Select(i);
        }
        if (fList->CurrentSelection()<0 && fList->CountItems()) fList->Select(0);
    }
    Selection();
}
void DraftWindow::Selection()
{
    auto index=fList->CurrentSelection(); const auto* item=index>=0 && static_cast<std::size_t>(index)<fItems.size()?&fItems[index]:nullptr;
    const bool uncertain=item && item->attempt=="create";
    if (uncertain!=fShowingMatches) { if (uncertain) fMatchesPanel->Show(); else fMatchesPanel->Hide(); fShowingMatches=uncertain; }
    if (!item || item->id!=fMatchJournal || item->revision!=fMatchRevision) {
        auto matchIndex=fMatchesList->CurrentSelection();
        std::string selected=item && item->id==fMatchJournal && matchIndex>=0 && static_cast<std::size_t>(matchIndex)<fMatches.size()?fMatches[matchIndex].id:"";
        fMatchJournal=item?item->id:0; fMatchRevision=item?item->revision:0;
        fMatches=uncertain?fStore->gmailDraftMatches(item->id):std::vector<GmailDraftMatch>{};
        while (auto* entry=fMatchesList->RemoveItem(int32(0))) delete entry;
        for (std::size_t i=0;i<fMatches.size();++i) {
            auto label="Copy "+std::to_string(i+1)+" · "+(fMatches[i].subject.empty()?"(No subject)":fMatches[i].subject);
            for (char& c:label) if (c=='\r' || c=='\n') c=' ';
            fMatchesList->AddItem(new DraftItem(label)); if (fMatches[i].id==selected) fMatchesList->Select(i);
        }
        if (fMatchesList->CurrentSelection()<0 && fMatchesList->CountItems()) fMatchesList->Select(0);
    }
    fUse->SetEnabled(uncertain && !item->active && fMatchesList->CurrentSelection()>=0);
    fRetry->SetEnabled(uncertain && !item->active);
    fRetry->SetLabel(item && !item->draft?"Stop checking…":"Retry save…");
    fKeep->SetEnabled(item && !item->active && item->conflict && item->attempt!="create");
    fKeep->SetLabel(item && item->missing?"Keep local draft":item && !item->draft?"Keep Gmail draft":"Keep both versions");
    std::string text="Gmail drafts are saved in the background. Changes from other devices appear here and in Drafts after checking mail.";
    if (item) {
        auto state=fStore->gmailDraftState(item->id);
        text=state.draft?fStore->draftStatus(state.draft):"Discarded draft — checking its Gmail copy";
        if (!state.error.empty() && !state.conflict) text+="\n\n"+state.error;
        if (state.conflict) {
            text+="\n\nClose the composer and return queued mail to Drafts before choosing which versions to keep.";
            if (state.draft) text+="\n\nOn this computer:\n"+preview(fStore->draft(state.draft));
            if (!state.observedRaw.empty()) {
                auto remote=parseDraftMessage(state.observedRaw,state.account,state.observedThread);
                text+="\n\nOn Gmail:\n"+preview(remote);
            }
            if (state.missing) text+="\n\nGmail no longer has this draft. Keeping the local version allows Mail to save it as a new Gmail draft.";
            else text+="\n\nKeep both versions preserves the local text and attachments as a separate draft and retains the Gmail version.";
        } else if (!state.attempt.empty() && !uncertain) text+="\n\nCheck Gmail looks for the previous save. Mail will not repeat an unconfirmed upload automatically.";
        if (uncertain) {
            if (state.draft) text+="\n\nOn this computer:\n"+preview(fStore->draft(state.draft));
            else text+="\n\nThe local draft was discarded. Stop checking leaves any saved Gmail copies intact.";
            auto matchIndex=fMatchesList->CurrentSelection();
            if (matchIndex>=0 && static_cast<std::size_t>(matchIndex)<fMatches.size()) {
                auto match=fStore->gmailDraftMatch(state.id,fMatches[matchIndex].id);
                text+="\n\nSelected Gmail copy:\n"+preview(parseDraftMessage(match.message.raw,state.account,match.message.thread));
                text+="\n\nUse Gmail copy links this saved version. Different local content is preserved as a separate draft; other Gmail copies remain intact.";
            } else text+="\n\nNo copies are listed. Check Gmail to refresh the search. Retry save may create a duplicate if the earlier save appears later.";
            text+="\n\nClose the composer and return queued mail to Drafts before resolving this save.";
        }
    }
    if (text!=fDetails->Text()) fDetails->SetText(text.c_str());
}
void DraftWindow::MessageReceived(BMessage* message)
{
    try {
        if (message->what==kRefresh || message->what==kDraftReview) { Refresh(); if (message->what==kDraftReview) Activate(); }
        else if (message->what==kSelected || message->what==kMatch) Selection();
        else if (message->what==kCheckDrafts) fOwner.SendMessage(kCheckDrafts);
        else if (message->what==kKeep) {
            auto index=fList->CurrentSelection(); if (index<0 || static_cast<std::size_t>(index)>=fItems.size()) return;
            fStore->keepBothGmailDrafts(fItems[index]); fOwner.SendMessage(kDraftReviewChanged); Refresh();
        } else if (message->what==kUse || message->what==kRetry) {
            auto index=fList->CurrentSelection(); if (index<0 || static_cast<std::size_t>(index)>=fItems.size()) return;
            const auto selected=fItems[index]; auto current=fStore->gmailDraftState(selected.id,false);
            if (current.active || current.revision!=selected.revision || current.attempt!="create") { Refresh(); return; }
            if (message->what==kUse) {
                auto matchIndex=fMatchesList->CurrentSelection(); if (matchIndex<0 || static_cast<std::size_t>(matchIndex)>=fMatches.size()) return;
                const auto remoteId=fMatches[matchIndex].id;
                if (!confirm("Use Gmail copy","Resolve this interrupted save using the selected Gmail copy? If its content differs, Mail keeps your local work as a separate draft. Other Gmail copies remain intact. This action does not send mail to recipients.","Use Gmail copy")) return;
                fStore->useGmailDraftMatch(selected,remoteId);
            } else if (selected.draft) {
                if (!confirm("Gmail save unconfirmed","The earlier save may already be on Gmail. Check Gmail before retrying. Saving again can create a duplicate. Your local text and attachments are retained. This action does not send mail to recipients.","Retry save")) return;
                fStore->retryGmailDraftCreate(selected);
            } else {
                if (!confirm("Stop checking discarded draft","Stop looking for this discarded draft's earlier save? No Gmail copies will be deleted. Any remaining copies can appear in Drafts on a later check.","Stop checking")) return;
                fStore->forgetDiscardedGmailDraft(selected);
            }
            fOwner.SendMessage(kDraftReviewChanged); Refresh();
        } else BWindow::MessageReceived(message);
    } catch (const std::exception& error) {
        if (message->what==kRefresh || message->what==kSelected || message->what==kMatch) { fDetails->SetText("Drafts changed. Refreshing…"); return; }
        (new BAlert("Draft synchronization",error.what(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go();
    }
}
}
