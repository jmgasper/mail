#pragma once
#include "Store.h"
#include <functional>
namespace mail {
struct DeliveryReport { int sent=0,failed=0,uncertain=0; };
DeliveryReport deliverQueued(Store&,const std::function<std::string(const Account&)>& password,
    const std::atomic<bool>& cancelled,
    const std::function<std::unique_ptr<GmailApi>(const Account&)>& gmail = {});
}
