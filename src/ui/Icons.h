#pragma once
#include <SupportDefs.h>

class BBitmap;
namespace mail::ui {
// Vector icons from Mail's resources (see resources/Icons.rdef), rendered at
// the given base size scaled for the system font. Cached for the application's
// lifetime; returns null when the icon is missing.
const BBitmap* MailIcon(const char* name,int32 size=16);
// The base size scaled the way Haiku scales icons for large fonts.
float IconSize(int32 size=16);
}
