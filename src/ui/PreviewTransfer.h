#pragma once
#include "mail/Preview.h"
#include <Messenger.h>

namespace mail::ui {
inline constexpr char kPreviewSignature[]="application/x-vnd.air-Mail-Preview";
constexpr uint32 kPreviewClosed='pvcl',kPreviewRefresh='pvrf',kPreviewEdit='pved',kPreviewSelect='pvsl';
struct TransferredPreview { PreviewSnapshot snapshot; BMessenger owner; };
std::string WritePreviewTransfer(const PreviewSnapshot&,const BMessenger&);
TransferredPreview ReadPreviewTransfer(const std::string&);
void LaunchPreview(const PreviewSnapshot&,const BMessenger&);
}
