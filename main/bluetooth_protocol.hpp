#pragma once
#include "bluetooth_bridge.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tab5bt {
inline bool unsigned_value(const char *text,uint32_t &value) {
    if(!text || !*text)return false;
    value=0;
    for(;*text;text++) {
        if(*text<'0' || *text>'9')return false;
        const uint32_t digit=uint32_t(*text-'0');
        if(value>(UINT32_MAX-digit)/10)return false;
        value=value*10+digit;
    }
    return true;
}
inline bool mac_valid(const char *value) {
    if(!value || std::strlen(value)!=17)return false;
    for(unsigned i=0;i<17;i++) {
        if(i%3==2){if(value[i]!=':')return false;}
        else if(!std::isxdigit(static_cast<unsigned char>(value[i])))return false;
    }
    return true;
}
inline bool integer_field(const char *line,const char *key,long &out) {
    const size_t length=std::strlen(key);
    for(const char *p=line;p && *p;) {
        if(!std::strncmp(p,key,length) && p[length]=='=') {
            char *end=nullptr;const long value=std::strtol(p+length+1,&end,10);
            if(end==p+length+1 || (*end && *end!=' '))return false;
            out=value;return true;
        }
        p=std::strchr(p,' ');if(p)++p;
    }
    return false;
}
inline bool unsigned_field(const char *line,const char *key,uint32_t &out) {
    const size_t length=std::strlen(key);
    for(const char *p=line;p && *p;) {
        if(!std::strncmp(p,key,length) && p[length]=='=') {
            const char *start=p+length+1,*end=std::strchr(start,' ');
            const size_t count=end?size_t(end-start):std::strlen(start);
            if(!count || count>10)return false;
            char number[11]{};std::memcpy(number,start,count);
            uint32_t value=0;if(!unsigned_value(number,value))return false;
            out=value;return true;
        }
        p=std::strchr(p,' ');if(p)++p;
    }
    return false;
}
inline bool device_line(const char *line,bt_bridge_device_t &out) {
    int number=0,rssi=0,saved=0,selected=0,at=0;char mac[18]{};
    if(std::sscanf(line,"DEVICE %d %17s %d %d %d %n",&number,mac,&rssi,&saved,&selected,&at)!=5 ||
       number<1 || number>32 || !mac_valid(mac) || rssi<-127 || rssi>20 ||
       (saved!=0 && saved!=1) || (selected!=0 && selected!=1) || at<=0)return false;
    out={};
    for(unsigned i=0;i<17;i++)out.mac[i]=char(std::toupper(static_cast<unsigned char>(mac[i])));
    std::snprintf(out.name,sizeof(out.name),"%s",line[at]?line+at:"Unnamed device");
    for(char &c:out.name)if(c && static_cast<unsigned char>(c)<32)c=' ';
    out.rssi=int16_t(rssi);out.saved=saved!=0;out.selected=selected!=0;return true;
}
inline bt_remote_action_t remote_line(const char *line) {
    if(!std::strcmp(line,"REMOTE PLAY"))return BT_REMOTE_PLAY;
    if(!std::strcmp(line,"REMOTE PAUSE"))return BT_REMOTE_PAUSE;
    if(!std::strcmp(line,"REMOTE NEXT"))return BT_REMOTE_NEXT;
    if(!std::strcmp(line,"REMOTE PREVIOUS"))return BT_REMOTE_PREVIOUS;
    return BT_REMOTE_NONE;
}
/* Preserve desired SD-backed volume until an exact ACK, including retries and
 * later STATUS messages. A remote headset's own volume is deliberately absent. */
class VolumeUpdate {
public:
    void queue(uint8_t volume){desired_=volume;pending_=true;sent_=false;}
    bool busy()const{return pending_;}
    int take(uint32_t now) {
        if(!pending_ || (sent_ && uint32_t(now-sent_at_)<2000))return -1;
        sent_=true;sent_at_=now;return desired_;
    }
    void acknowledge(int volume){if(sent_ && volume==desired_)pending_=false;}
private:
    int desired_=31;bool pending_=false,sent_=false;uint32_t sent_at_=0;
};

/* A shutdown request is permanent until reboot. The UART worker drains old
 * commands and input before sending SLEEP, so an earlier STANDBY cannot satisfy
 * a new request. Once acknowledged it must never send a retry: UART transitions
 * can wake the Pico's EXT0 pin after it enters deep sleep. */
class ShutdownHandshake {
public:
    void request(){requested_=true;}
    bool requested()const{return requested_;}
    void already_sleeping(){if(requested_)acknowledged_=true;}
    void sleep_sent(){if(requested_)sent_=true;}
    void acknowledge(){if(requested_ && sent_)acknowledged_=true;}
    bool sent()const{return sent_;}
    bool acknowledged()const{return acknowledged_;}
private:
    bool requested_=false,sent_=false,acknowledged_=false;
};

/* Each application play/session revision invalidates all earlier responses.
 * Protocol 0.9 echoes a monotonically increasing stream token, so an ACK from
 * an earlier wake cannot open the current stream. Legacy 0.8 has no tokens;
 * its STOP/SILENT fence is best effort, not equivalent session identity. */
class StreamHandshake {
public:
    enum class Command {None,Stop,Stream};
    struct Request {Command command=Command::None;uint32_t token=0;bool tagged=false;};
    void update(bool wanted,bool tagged,uint32_t revision) {
        if(revision_==revision && wanted_==wanted && tagged_==tagged)return;
        revision_=revision;wanted_=wanted;tagged_=tagged;
        state_=wanted?(tagged?State::NeedStream:State::NeedFence):State::NeedStop;
        if(wanted && !++sequence_)++sequence_;
    }
    Request next(uint32_t now) {
        if((state_==State::AwaitFence || state_==State::AwaitStream) && uint32_t(now-sent_at_)>=500)
            state_=tagged_?State::NeedStream:State::NeedFence;
        if(state_==State::NeedStop){state_=State::Idle;return {Command::Stop,0,false};}
        if(state_==State::NeedFence){state_=State::AwaitFence;sent_at_=now;return {Command::Stop,0,false};}
        if(state_==State::NeedStream){state_=State::AwaitStream;sent_at_=now;return {Command::Stream,sequence_,tagged_};}
        return {};
    }
    void silent(uint32_t revision) {
        if(revision==revision_ && state_==State::AwaitFence)state_=State::NeedStream;
    }
    bool acknowledge(uint32_t token,bool tagged,uint32_t revision) {
        if(revision!=revision_ || state_!=State::AwaitStream || tagged!=tagged_ || (tagged && token!=sequence_))return false;
        state_=State::Ready;return true;
    }
    bool matches(uint32_t token,uint32_t revision)const {
        return wanted_ && tagged_ && state_==State::Ready && revision_==revision && token==sequence_;
    }
private:
    enum class State {Idle,NeedStop,NeedFence,AwaitFence,NeedStream,AwaitStream,Ready};
    State state_=State::Idle;
    bool wanted_=false,tagged_=false;
    uint32_t revision_=UINT32_MAX,sequence_=0,sent_at_=0;
};

/* Older Pico firmware cannot report a stopped A2DP callback. Infer only a
 * sustained full-ring overflow, never quiet PCM or a single radio hiccup.
 * The caller supplies fresh STATUS samples, not repeated cached snapshots. */
class LegacyStreamHealth {
public:
    bool observe(uint32_t now,bool wanted,uint32_t revision,uint32_t dropped,uint32_t buffered) {
        if(!wanted || !active_ || revision!=revision_) {
            active_=wanted;revision_=revision;start_=last_=now;dropped_=dropped;suspect_=0;return false;
        }
        const uint32_t elapsed=now-last_,delta=dropped-dropped_;
        last_=now;dropped_=dropped;
        if(uint32_t(now-start_)<3000 || elapsed>2500 || elapsed<500 ||
           delta>44100*10 || delta<elapsed*12 || buffered<3072){suspect_=0;return false;}
        suspect_+=elapsed;
        if(suspect_<4000)return false;
        suspect_=0;start_=now;return true;
    }
private:
    bool active_=false;uint32_t revision_=0,start_=0,last_=0,dropped_=0,suspect_=0;
};
}
