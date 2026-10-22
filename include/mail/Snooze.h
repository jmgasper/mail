#pragma once
#include "Model.h"

namespace mail {
enum class SnoozePreset { Hour, Tomorrow, Monday };
// Local civil time can have zero (spring gap), one, or two (autumn repeat)
// corresponding instants. Callers must not silently normalize a skipped time.
std::vector<Timestamp> localSnoozeTimes(const std::string& date,const std::string& time);
Timestamp snoozePreset(Timestamp now,SnoozePreset);
std::string localSnoozeDate(Timestamp);
std::string localSnoozeClock(Timestamp);
std::string describeSnoozeTime(Timestamp);
}
