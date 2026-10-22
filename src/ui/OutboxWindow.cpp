#include "OutboxWindow.h"
#include "ComposerWindow.h"
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
enum:uint32 { kRefresh='orfh',kSelected='osel',kRetry='otry',kEdit='oedt' };
std::string stateLabel(const std::string& state)
{
    if (state=="queued") return "Waiting to send";
    if (state=="sending") return "Sending";
    if (state=="failed") return "Delivery failed";
    return "Delivery uncertain";
}
bool copyActive(const SentCopy& copy) { return copy.state=="checking" || copy.state=="uploading"; }
std::string copyLabel(const SentCopy& copy)
{
    if (copy.state=="uploading") return "Sent — saving server copy";
    if (copy.state=="checking") return "Sent — checking server copy";
    if (copy.state=="uncertain") return "Sent — server copy unconfirmed";
    if (copy.state=="saved") return "Sent — server copy accepted";
    if (copy.state=="failed") return "Sent — server copy needs attention";
    return "Sent — waiting for server copy";
}
std::string rowLabel(const SentCopy& copy)
{
    auto subject=copy.subject.empty()?"(No subject)":copy.subject;
    std::replace(subject.begin(),subject.end(),'\n',' '); std::replace(subject.begin(),subject.end(),'\r',' ');
    return copyLabel(copy)+" · "+subject;
}
std::string rowLabel(const Outgoing& item)
{
    std::string subject=item.subject.empty()?"(No subject)":item.subject;
    std::replace(subject.begin(),subject.end(),'\n',' '); std::replace(subject.begin(),subject.end(),'\r',' ');
    return stateLabel(item.state)+" · "+subject;
}
}
OutboxWindow::OutboxWindow(BWindow* owner,std::shared_ptr<Store> store)
    :BWindow(BRect(180,130,850,540),"Outbox",B_TITLED_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS),fStore(std::move(store)),fOwner(owner)
{
    fList=new BListView("outbox list"); fList->SetSelectionMessage(new BMessage(kSelected));
    auto* scroll=new BScrollView("outbox scroll",fList,0,false,true); scroll->SetExplicitMinSize(BSize(500,150));
    fDetails=new EmojiTextView("delivery details"); fDetails->MakeEditable(false); fDetails->SetWordWrap(true); fDetails->SetInsets(8,8,8,8);
    auto* details=new BScrollView("delivery details scroll",fDetails,0,false,true); details->SetExplicitMinSize(BSize(500,130));
    fRetry=new BButton("retry delivery","Retry delivery",new BMessage(kRetry));
    fEdit=new BButton("return to draft","Return to draft",new BMessage(kEdit));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS).Add(scroll,1).Add(details)
        .AddGroup(B_HORIZONTAL).Add(fRetry).Add(fEdit)
            .AddGlue().Add(new BButton("Close",new BMessage(B_QUIT_REQUESTED))).End();
    BMessage tick(kRefresh); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,1000000); Refresh();
}
OutboxWindow::~OutboxWindow() { while (auto* item=fList->RemoveItem(int32(0))) delete item; }
void OutboxWindow::Refresh()
{
    Id selected=0; bool selectedCopy=false; auto index=fList->CurrentSelection();
    if (index>=0 && static_cast<std::size_t>(index)<fItems.size()) selected=fItems[index].id;
    else if (index>=0 && static_cast<std::size_t>(index)<fItems.size()+fCopies.size()) { selectedCopy=true; selected=fCopies[index-fItems.size()].id; }
    auto items=fStore->outbox(false); auto copies=fStore->sentCopies();
    bool changed=items.size()!=fItems.size() || copies.size()!=fCopies.size();
    for (std::size_t i=0;!changed && i<items.size();++i) changed=items[i].id!=fItems[i].id || rowLabel(items[i])!=rowLabel(fItems[i]);
    for (std::size_t i=0;!changed && i<copies.size();++i) changed=copies[i].id!=fCopies[i].id || rowLabel(copies[i])!=rowLabel(fCopies[i]);
    fItems=std::move(items); fCopies=std::move(copies);
    if (changed) {
        while (auto* item=fList->RemoveItem(int32(0))) delete item;
        for (std::size_t i=0;i<fItems.size();++i) {
            fList->AddItem(new BStringItem(rowLabel(fItems[i]).c_str()));
            if (!selectedCopy && fItems[i].id==selected) fList->Select(i);
        }
        for (std::size_t i=0;i<fCopies.size();++i) {
            fList->AddItem(new BStringItem(rowLabel(fCopies[i]).c_str()));
            if (selectedCopy && fCopies[i].id==selected) fList->Select(fItems.size()+i);
        }
        if (fList->CurrentSelection()<0 && fList->CountItems()) fList->Select(0);
    }
    UpdateSelection();
}
void OutboxWindow::UpdateSelection()
{
    auto index=fList->CurrentSelection();
    if (index>=0 && static_cast<std::size_t>(index)>=fItems.size() && static_cast<std::size_t>(index)<fItems.size()+fCopies.size()) {
        const auto& copy=fCopies[index-fItems.size()];
        fRetry->SetLabel(copy.state=="uncertain"?"Retry saving copy…":"Check server"); fEdit->SetLabel("Keep local copy");
        fRetry->SetEnabled(!copyActive(copy)); fEdit->SetEnabled(!copyActive(copy));
        auto text=copyLabel(copy)+"\nDelivery to recipients is complete. This entry only tracks the copy in the server's Sent folder.";
        if (!copy.error.empty()) text+="\n"+copy.error;
        if (copy.state=="uncertain") text+="\nRetrying can create a second Sent copy. It does not send the message to recipients again.";
        text+="\nKeep local copy stops checking this entry. It does not remove any server copy.";
        if (text!=fDetails->Text()) fDetails->SetText(text.c_str());
        return;
    }
    fRetry->SetLabel("Retry delivery"); fEdit->SetLabel("Return to draft");
    const auto* item=index>=0 && static_cast<std::size_t>(index)<fItems.size()?&fItems[index]:nullptr;
    fRetry->SetEnabled(item && (item->state=="failed" || item->state=="uncertain"));
    fEdit->SetEnabled(item && item->state!="sending");
    std::string text=fItems.empty()?"No messages waiting to send. Delivered messages are in Sent.":"Select a message to review its delivery.";
    if (item) {
        text="From: "+item->from+"\nRecipients: ";
        for (const auto& recipient:item->recipients) text+=recipient+"  ";
        text+="\nStatus: "+stateLabel(item->state);
        if (!item->error.empty()) text+="\n"+item->error;
        if (item->state=="uncertain") text+="\nThe recipient may already have this message. Check Sent and the server before retrying. Mail will not retry automatically.";
        else if (item->state=="failed") text+="\nMail will not retry automatically. Retry delivery, or return to Drafts to edit the message.";
        else if (item->state=="sending") text+="\nWait for this delivery attempt to finish before making changes.";
    }
    if (text!=fDetails->Text()) fDetails->SetText(text.c_str());
}
void OutboxWindow::MessageReceived(BMessage* message)
{
    try {
        if (message->what==kRefresh) { Refresh(); return; }
        if (message->what==kOutboxActivate) { Refresh(); Activate(); return; }
        if (message->what==kSelected) { UpdateSelection(); return; }
        auto index=fList->CurrentSelection();
        if ((message->what==kRetry || message->what==kEdit) && index>=0 && static_cast<std::size_t>(index)>=fItems.size()
            && static_cast<std::size_t>(index)<fItems.size()+fCopies.size()) {
            auto id=fCopies[index-fItems.size()].id; auto current=fStore->sentCopies();
            auto found=std::find_if(current.begin(),current.end(),[&](const SentCopy& copy){return copy.id==id;});
            if (found==current.end() || copyActive(*found)) { Refresh(); return; }
            if (message->what==kEdit) { fStore->keepSentCopyLocal(id); fOwner.SendMessage(kOutboxChanged); }
            else {
                if (found->state=="uncertain") {
                    auto* alert=new BAlert("Sent copy unconfirmed","The server may already have saved this Sent copy. Check its Sent folder before retrying. Saving again may create a second copy, but will not send the message to recipients again.","Cancel","Retry saving copy",nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT);
                    alert->SetShortcut(0,B_ESCAPE); alert->SetDefaultButton(alert->ButtonAt(0));
                    if (alert->Go()!=1) return;
                    fStore->retrySentCopy(id,true);
                }
                fOwner.SendMessage(kCheckSentCopies);
            }
            Refresh(); return;
        }
        if ((message->what==kRetry || message->what==kEdit) && index>=0 && static_cast<std::size_t>(index)<fItems.size()) {
            // Re-read the selected ID, never a replacement at the same row index.
            auto id=fItems[index].id; auto current=fStore->outbox(false);
            auto found=std::find_if(current.begin(),current.end(),[&](const Outgoing& item){return item.id==id;});
            if (found==current.end() || found->state=="sending" || (message->what==kRetry && found->state=="queued")) { Refresh(); return; }
            const auto item=*found;
            bool accepted=false;
            if (item.state=="uncertain") {
                const bool retry=message->what==kRetry;
                const char* warning=retry?"The recipient may already have this message. Check Sent and the server before retrying. Retry delivery now? Sending it again may create a duplicate."
                    :"The recipient may already have this message. Returning it to Drafts does not recall it. Sending the edited draft later may create a duplicate. Return to Drafts?";
                auto* alert=new BAlert("Delivery uncertain",warning,"Cancel",retry?"Retry delivery":"Return to draft",nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT);
                alert->SetShortcut(0,B_ESCAPE); alert->SetDefaultButton(alert->ButtonAt(0));
                accepted=alert->Go()==1;
                if (!accepted) return;
            }
            if (message->what==kRetry) { fStore->retryOutgoing(item.id,accepted); fOwner.SendMessage(kSendQueued); }
            else { auto draft=fStore->returnToDraft(item.id,accepted); fOwner.SendMessage(kOutboxChanged); BMessage open(kOpenDraft); open.AddInt64("draft",draft); fOwner.SendMessage(&open); }
            Refresh(); return;
        }
        BWindow::MessageReceived(message);
    } catch (const std::exception& error) { (new BAlert("Outbox",error.what(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go(); }
}
}
