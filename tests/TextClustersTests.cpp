#include "mail/TextClusters.h"
#include <unicode/uchar.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

using mail::TextClusters;
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
std::string utf8(uint32_t c)
{
    std::string out;
    if(c<0x80)out+=char(c);
    else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
    else if(c<0x10000){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    return out;
}
}
int main(int argc,char** argv)
{
    try {
        check(argc==2,"Pass the Unicode conformance data directory");
        UVersionInfo version;u_getUnicodeVersion(version);
        auto unicode=std::to_string(version[0])+'.'+std::to_string(version[1])+'.'+std::to_string(version[2]);
        std::ifstream input(std::string(argv[1])+"/GraphemeBreakTest-"+unicode+".txt");
        if(!input)throw std::runtime_error("Missing conformance corpus for ICU Unicode "+unicode);
        std::string line;int cases=0,lineNumber=0,expectedCases=0;
        while(std::getline(input,line)) {
            if(line.rfind("# Lines:",0)==0)expectedCases=std::stoi(line.substr(8));
            ++lineNumber;line.erase(line.find('#')==std::string::npos?line.size():line.find('#'));
            std::istringstream fields(line);std::string token,text;std::vector<int32_t> expected;
            while(fields>>token) {
                if(token=="÷")expected.push_back(text.size());
                else if(token!="×")text+=utf8(std::stoul(token,nullptr,16));
            }
            if(expected.empty())continue;
            if(TextClusters(text).Boundaries()!=expected)throw std::runtime_error("Unicode grapheme conformance failed on line "+std::to_string(lineNumber));
            ++cases;
        }
        check(expectedCases>0 && cases==expectedCases,"Complete official Unicode conformance corpus");
        for(const std::string cluster:{"👩🏽‍💻","🇦🇺","👨‍👩‍👧‍👦","👩‍👩‍👧‍👧","1️⃣","é","🏳️‍🌈","👩🏾‍❤️‍👩🏼","\r\n","각"}) {
            TextClusters index("A"+cluster+"Z");int32_t end=1+cluster.size();
            check(index.Boundaries()==std::vector<int32_t>({0,1,end,end+1}),"Joined emoji and combining text are single graphemes");
            for(int32_t at=2;at<end;++at)check(index.Floor(at)==1 && index.Ceil(at)==end && !index.IsBoundary(at),"Interior bytes snap to complete grapheme boundaries");
            check(index.Before(end)==1 && index.After(1)==end,"Caret navigation crosses whole graphemes");
        }
        TextClusters empty("");check(empty.Boundaries()==std::vector<int32_t>{0} && empty.Before(0)==0 && empty.After(0)==0,"Empty input has one boundary");
        TextClusters flags("🇦🇺🇬🇧🇨");check(flags.Boundaries()==std::vector<int32_t>({0,8,16,20}),"Regional indicators pair from the start of the run");
        TextClusters ascii("abc");check(ascii.Floor(-1)==0 && ascii.Ceil(99)==3 && ascii.Before(99)==3 && ascii.After(-1)==0,"Out-of-range positions clamp safely");
        std::string malformed("a\xff\x80z",4);TextClusters broken(malformed);check(broken.Boundaries().front()==0 && broken.Boundaries().back()==4,"Malformed UTF-8 retains bounded byte positions");
        std::string longCluster="e";for(int i=0;i<20000;++i)longCluster+="́";
        check(TextClusters(longCluster).Boundaries()==std::vector<int32_t>({0,static_cast<int32_t>(longCluster.size())}),"Long combining sequences remain atomic");
        check(TextClusters(std::string(2*1024*1024,'x')).Boundaries().size()==2*1024*1024+1,"Full-size draft segmentation remains bounded");
        std::cout<<checks<<" grapheme checks passed; "<<cases<<" Unicode "<<unicode<<" conformance cases passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
