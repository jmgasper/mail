#include "MoveWindow.h"
#include "Emoji.h"
#include <algorithm>
#include <Alert.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <ScrollView.h>
#include <TextView.h>

namespace mail::ui {
namespace {
enum:uint32 { kRefresh='mvrf',kSelected='mvsl',kCheck='mvsc',kRetry='mvrt',kKeep='mvkp' };
std::string status(const PendingMove& move)
{
    if (move.active) return "Checking move";
    if (!move.error.empty()) return "Move needs attention";
    if (move.remote.stage=="queued" || move.remote.stage=="ready") return "Waiting to move";
    if (move.remote.stage=="retry") return "Waiting to retry copy";
    if (move.remote.stage=="copied" || move.remote.stage=="moved") return "Finishing move";
    return "Server copy unconfirmed";
}
bool canRetry(const PendingMove& move)
{
    return !move.active && move.remote.stage=="started" && move.remote.destination.locator.empty();
}
std::string rowLabel(const PendingMove& move)
{
    auto text=status(move)+" · "+move.accountName+" · "+(move.subject.empty()?"(No subject)":move.subject);
    std::replace(text.begin(),text.end(),'\n',' '); std::replace(text.begin(),text.end(),'\r',' '); return text;
}
bool confirm(const char* title,const char* text,const char* action)
{
    auto* alert=new BAlert(title,text,"Cancel",action,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT);
    alert->SetShortcut(0,B_ESCAPE); alert->SetDefaultButton(alert->ButtonAt(0)); return alert->Go()==1;
}
}
MoveWindow::MoveWindow(BWindow* owner,std::shared_ptr<Store> store)
    :BWindow(BRect(165,120,915,590),"Folder moves",B_TITLED_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS),fStore(std::move(store)),fOwner(owner)
{
    fList=new BListView("move list"); fList->SetSelectionMessage(new BMessage(kSelected));
    auto* scroll=new BScrollView("move list scroll",fList,0,false,true); scroll->SetExplicitMinSize(BSize(560,140));
    fDetails=new EmojiTextView("move details"); fDetails->MakeEditable(false); fDetails->SetWordWrap(true); fDetails->SetInsets(8,8,8,8);
    auto* details=new BScrollView("move details scroll",fDetails,0,false,true); details->SetExplicitMinSize(BSize(560,190));
    fCheck=new BButton("check move","Check server",new BMessage(kCheck));
    fRetry=new BButton("retry move","Retry copy…",new BMessage(kRetry));
    fKeep=new BButton("keep move","Keep local copy…",new BMessage(kKeep));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS).Add(scroll,1).Add(details)
        .AddGroup(B_HORIZONTAL).Add(fCheck).Add(fRetry).Add(fKeep).AddGlue()
            .Add(new BButton("Close",new BMessage(B_QUIT_REQUESTED))).End();
    BMessage tick(kRefresh); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,1000000); Refresh();
}
MoveWindow::~MoveWindow() { while (auto* item=fList->RemoveItem(int32(0))) delete item; }
void MoveWindow::Refresh()
{
    auto index=fList->CurrentSelection();
    Id selected=index>=0 && static_cast<std::size_t>(index)<fItems.size()?fItems[index].change.id:0;
    auto items=fStore->moveReviews(); bool changed=items.size()!=fItems.size();
    for (std::size_t i=0;!changed && i<items.size();++i) changed=items[i].change.id!=fItems[i].change.id || rowLabel(items[i])!=rowLabel(fItems[i]);
    fItems=std::move(items);
    if (changed) {
        while (auto* item=fList->RemoveItem(int32(0))) delete item;
        for (std::size_t i=0;i<fItems.size();++i) {
            fList->AddItem(new BStringItem(rowLabel(fItems[i]).c_str()));
            if (fItems[i].change.id==selected) fList->Select(i);
        }
        if (fList->CurrentSelection()<0 && fList->CountItems()) fList->Select(0);
    }
    UpdateSelection();
}
void MoveWindow::UpdateSelection()
{
    auto index=fList->CurrentSelection();
    const auto* move=index>=0 && static_cast<std::size_t>(index)<fItems.size()?&fItems[index]:nullptr;
    fCheck->SetEnabled(move && !move->active); fRetry->SetEnabled(move && canRetry(*move)); fKeep->SetEnabled(move && !move->active);
    fKeep->SetLabel(move && move->remote.stage=="queued"?"Cancel move":"Keep local copy…");
    std::string text="No folder moves waiting. Mail has finished the saved move requests.";
    if (move) {
        auto target=move->remote.destination.folder.empty()?move->change.value:move->remote.destination.folder;
        text="Account: "+move->accountName+"\nSubject: "+(move->subject.empty()?"(No subject)":move->subject)
            +"\nFrom: "+move->remote.source.folder+"\nTo: "+target+"\nStatus: "+status(*move);
        if (!move->error.empty()) text+="\n"+move->error;
        if (move->active) text+="\nWait for this check to finish before changing the move.";
        else if (move->remote.stage=="queued") text+="\nCheck server starts the queued move. Cancel move restores its server folder in Mail.";
        else {
            if (canRetry(*move)) text+="\nCheck server looks for the previous copy without copying again. Retry copy may create a duplicate; inspect the destination folder first.";
            else if (move->remote.stage=="copied" || move->remote.stage=="moved") text+="\nThe destination copy is recorded. Check server finishes the saved progress without copying again.";
            else text+="\nCheck server continues this saved move request.";
            text+="\nKeep local copy stops this move and keeps the message and its current local edits. Server messages can appear separately on the next check.";
        }
    }
    if (text!=fDetails->Text()) fDetails->SetText(text.c_str());
}
void MoveWindow::MessageReceived(BMessage* message)
{
    try {
        if (message->what==kRefresh || message->what==kFolderMoves) { Refresh(); if (message->what==kFolderMoves) Activate(); return; }
        if (message->what==kSelected) { UpdateSelection(); return; }
        if (message->what!=kCheck && message->what!=kRetry && message->what!=kKeep) { BWindow::MessageReceived(message); return; }
        auto index=fList->CurrentSelection();
        if (index<0 || static_cast<std::size_t>(index)>=fItems.size()) return;
        // Hold the reviewed operation and revision across confirmation. Never
        // apply a choice to a replacement at this row, or to a running worker.
        const auto selected=fItems[index]; const auto current=fStore->moveReviews();
        auto found=std::find_if(current.begin(),current.end(),[&](const PendingMove& m){return m.change.id==selected.change.id && m.revision==selected.revision;});
        if (found==current.end() || found->active || found->remote.stage!=selected.remote.stage) { Refresh(); return; }
        if (message->what==kCheck) fOwner.SendMessage(kCheckMoves);
        else if (message->what==kRetry) {
            if (!canRetry(selected)) return;
            if (!confirm("Folder copy unconfirmed","The destination folder may already contain this message. Check it before retrying. Another copy can create a duplicate. This action does not send mail to recipients.","Retry copy")) return;
            fStore->retryMove(selected,true); fOwner.SendMessage(kCheckMoves);
        } else {
            if (selected.remote.stage=="queued") fStore->cancelQueuedMove(selected.change);
            else {
                if (!confirm("Keep local copy","Stop this move and keep the message with its current local edits? No server messages will be removed. Mail will download server messages separately on a later check. Changes to this local copy will stay on this computer.","Keep local copy")) return;
                fStore->keepMoveLocal(selected);
            }
        }
        fOwner.SendMessage(kMovesChanged); Refresh();
    } catch (const std::exception& error) { (new BAlert("Folder moves",error.what(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go(); Refresh(); }
}
}
