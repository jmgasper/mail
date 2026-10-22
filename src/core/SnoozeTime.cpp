#include "mail/Snooze.h"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace mail {
namespace {
std::tm local(Timestamp value)
{
    if(value<0 || value>253402300799LL || value>std::numeric_limits<std::time_t>::max()) throw std::invalid_argument("Choose a valid date.");
    auto seconds=static_cast<std::time_t>(value); std::tm tm{};
    if(!localtime_r(&seconds,&tm)) throw std::invalid_argument("Could not read the local time zone.");
    return tm;
}
std::string format(Timestamp value,const char* pattern)
{
    auto tm=local(value); char text[128];
    if(!std::strftime(text,sizeof(text),pattern,&tm)) throw std::invalid_argument("Could not format this date.");
    return text;
}
int digits(const std::string& text,std::size_t offset,std::size_t length)
{
    int value=0;
    for(std::size_t i=offset;i<offset+length;++i) {
        if(text[i]<'0' || text[i]>'9') throw std::invalid_argument("Use YYYY-MM-DD for the date and HH:MM for the time.");
        value=value*10+text[i]-'0';
    }
    return value;
}
// Gregorian civil date as seconds on a UTC-like calendar. This deliberately
// does not consult mktime: Haiku's ICU backend ignores tm_isdst, so changing
// that flag cannot enumerate both occurrences of an autumn clock repeat.
Timestamp civil(int year,int month,int day,int hour=0,int minute=0,int second=0)
{
    year-=month<=2;
    const int era=(year>=0?year:year-399)/400;
    const unsigned y=static_cast<unsigned>(year-era*400);
    const unsigned d=(153*(month+(month>2?-3:9))+2)/5+day-1;
    const unsigned days=y*365+y/4-y/100+d;
    return (static_cast<Timestamp>(era)*146097+days-719468)*86400+hour*3600+minute*60+second;
}
}
std::vector<Timestamp> localSnoozeTimes(const std::string& date,const std::string& time)
{
    if(date.size()!=10 || date[4]!='-' || date[7]!='-' || time.size()!=5 || time[2]!=':')
        throw std::invalid_argument("Use YYYY-MM-DD for the date and HH:MM for the time.");
    int year=digits(date,0,4),month=digits(date,5,2),day=digits(date,8,2),hour=digits(time,0,2),minute=digits(time,3,2);
    if(year<1970 || month<1 || month>12 || day<1 || day>31 || hour>23 || minute>59)
        throw std::invalid_argument("Choose a valid date and a time from 00:00 to 23:59.");
    std::vector<Timestamp> result;
    auto requested=civil(year,month,day,hour,minute);
    // Sample offsets on both sides of nearby transitions, including half-hour
    // changes and date-line jumps, then verify every candidate by round trip.
    for(int hours=-48;hours<=48;hours+=6) {
        auto sample=requested+hours*3600;
        if(sample<0 || sample>253402300799LL) continue;
        auto sampleTime=local(sample);
        auto offset=civil(sampleTime.tm_year+1900,sampleTime.tm_mon+1,sampleTime.tm_mday,sampleTime.tm_hour,sampleTime.tm_min,sampleTime.tm_sec)-sample;
        auto value=requested-offset; if(value<0 || value>253402300799LL) continue;
        auto round=local(value);
        if(round.tm_year!=year-1900 || round.tm_mon!=month-1 || round.tm_mday!=day || round.tm_hour!=hour || round.tm_min!=minute) continue;
        if(std::find(result.begin(),result.end(),value)==result.end()) result.push_back(value);
    }
    if(result.empty()) throw std::invalid_argument("This date or local clock time does not exist. Check the date and any daylight saving change.");
    std::sort(result.begin(),result.end()); return result;
}
Timestamp snoozePreset(Timestamp now,SnoozePreset preset)
{
    auto tm=local(now);
    if(preset==SnoozePreset::Hour) { local(now+3600); return now+3600; }
    auto days=preset==SnoozePreset::Tomorrow?1:(tm.tm_wday==1?7:(8-tm.tm_wday)%7);
    auto calendar=static_cast<std::time_t>(civil(tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday)+days*86400);
    std::tm target{}; if(!gmtime_r(&calendar,&target)) throw std::invalid_argument("Could not choose a future date.");
    char date[32]; std::snprintf(date,sizeof(date),"%04d-%02d-%02d",target.tm_year+1900,target.tm_mon+1,target.tm_mday);
    auto value=localSnoozeTimes(date,"09:00").front();
    if(value<=now) throw std::invalid_argument("Could not choose a future date.");
    return value;
}
std::string localSnoozeDate(Timestamp value) { return format(value,"%Y-%m-%d"); }
std::string localSnoozeClock(Timestamp value) { return format(value,"%H:%M"); }
std::string describeSnoozeTime(Timestamp value) { return format(value,"%a %d %b %Y, %H:%M %Z (%z)"); }
}
