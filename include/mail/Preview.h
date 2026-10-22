#pragma once
#include "Compose.h"
#include <optional>

namespace mail {
struct PreviewThreadItem { Id id=0; std::string sender,date; };
struct ReceivedPreview {
    Id id=0;
    std::string from,to,cc,subject,date,text,html;
    std::vector<Attachment> files;
    std::vector<PreviewThreadItem> thread;
};
struct PreviewSnapshot { Draft draft; std::string from,key; std::optional<ReceivedPreview> received{}; };
inline constexpr std::size_t kPreviewSnapshotLimit=40*1024*1024;
inline constexpr char kPreviewPolicy[]="default-src 'none'; img-src mail-preview:; font-src mail-preview:; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-src 'none'; object-src 'none'";
std::string encodePreview(const PreviewSnapshot&);
PreviewSnapshot decodePreview(const std::string&);
// Uses the same body HTML as delivery, replacing CID resources with the
// preview's private scheme and making links inert while retaining their URLs.
std::string previewHtml(const Draft&,const std::string& resourceRoot);
// Parses MIME HTML into an allowlisted document. Only matching CID images are
// given resource URLs; scripts, navigation, forms and embedded documents are removed.
std::string receivedHtml(const ReceivedPreview&,const std::string& resourceRoot,bool plain=false);
bool previewImageType(const std::string&);
void validateReceivedPreview(const ReceivedPreview&);
std::string previewDocument(const std::string& body,const std::string& resourceRoot);
}
