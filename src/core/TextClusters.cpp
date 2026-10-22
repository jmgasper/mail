#include "mail/TextClusters.h"
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

namespace mail {
TextClusters::TextClusters(std::string_view text)
{
    if(text.size()>static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        throw std::invalid_argument("Text is too large for Unicode segmentation.");
    UErrorCode error=U_ZERO_ERROR;
    std::unique_ptr<UBreakIterator,decltype(&ubrk_close)> iterator(ubrk_open(UBRK_CHARACTER,"root",nullptr,0,&error),ubrk_close);
    if(U_FAILURE(error) || !iterator)throw std::runtime_error("Unicode character boundaries are unavailable.");
    UText input=UTEXT_INITIALIZER;
    utext_openUTF8(&input,text.empty()?"":text.data(),text.size(),&error);
    struct Close { UText* input; ~Close(){utext_close(input);} } close{&input};
    ubrk_setUText(iterator.get(),&input,&error);
    if(U_FAILURE(error))throw std::runtime_error("Could not segment Unicode text.");
    for(auto at=ubrk_first(iterator.get());at!=UBRK_DONE;at=ubrk_next(iterator.get()))fBoundaries.push_back(at);
}
int32_t TextClusters::Floor(int32_t at) const
{
    auto found=std::upper_bound(fBoundaries.begin(),fBoundaries.end(),at);
    return found==fBoundaries.begin()?0:*--found;
}
int32_t TextClusters::Ceil(int32_t at) const
{
    auto found=std::lower_bound(fBoundaries.begin(),fBoundaries.end(),at);
    return found==fBoundaries.end()?fBoundaries.back():*found;
}
int32_t TextClusters::Before(int32_t at) const
{
    auto found=std::lower_bound(fBoundaries.begin(),fBoundaries.end(),at);
    return found==fBoundaries.begin()?0:*--found;
}
int32_t TextClusters::After(int32_t at) const
{
    auto found=std::upper_bound(fBoundaries.begin(),fBoundaries.end(),at);
    return found==fBoundaries.end()?fBoundaries.back():*found;
}
bool TextClusters::IsBoundary(int32_t at) const { return std::binary_search(fBoundaries.begin(),fBoundaries.end(),at); }
}
