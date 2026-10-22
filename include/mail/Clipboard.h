#pragma once
#include "Compose.h"
#include <string_view>

namespace mail {
inline constexpr std::size_t kClipboardHtmlLimit=8*1024*1024;
// Converts untrusted clipboard HTML to the composer's semantic text model.
// No URLs or files are fetched. Embedded data images become owned attachments.
Draft importClipboardHtml(std::string_view);
// MIME draft variant: resolves only supplied CID attachments, retains unused
// attachments as files, and never fetches remote or filesystem image sources.
Draft importDraftHtml(std::string_view,const std::vector<Attachment>&);
// Self-contained HTML for other applications. Throws if it exceeds the bound;
// native text/font runs and Mail's binary fragment remain available separately.
std::string exportClipboardHtml(const Draft&);
}
