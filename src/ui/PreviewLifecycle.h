#pragma once
#include <OS.h>
#include <unistd.h>
#include <string_view>

namespace mail::ui {
// View/context destruction queues engine teardown. Its helper processes must
// finish while the owning BApplication loop is still able to dispatch replies.
// Match only this process's children, never a browser's separate renderer.
inline bool PreviewHasPendingRenderer()
{
    int32 cookie=0; team_info team;
    while(get_next_team_info(&cookie,&team)==B_OK) {
        if(team.parent!=getpid()) continue;
        std::string_view executable(team.args); executable=executable.substr(0,executable.find(' '));
        if(executable.ends_with("/WebProcess") || executable.ends_with("/NetworkProcess")) return true;
    }
    return false;
}
}
