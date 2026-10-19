#pragma once
namespace tab5bt {
/* The player owns pause/resume. A manual action always cancels the right to
 * auto-resume; initial or unknown ear reports never start/stop music. */
class AirPodsPlayback {
public:
    enum class Action {None,Pause,Resume};
    void manual(){owned_=false;}
    Action update(bool available,bool enabled,int ears,bool playing,bool paused){
        if(!available||!enabled||ears<0||ears>2){ears_=-1;owned_=false;return Action::None;}
        const int before=ears_;ears_=ears;
        if(owned_&&ears==2&&paused){owned_=false;return Action::Resume;}
        if(before==2&&ears<2&&playing){owned_=true;return Action::Pause;}
        return Action::None;
    }
private:int ears_=-1;bool owned_=false;
};
}
