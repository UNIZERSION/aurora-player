#ifdef NDEBUG
#undef NDEBUG
#endif
#include "bluetooth_dsp.hpp"
#include "bluetooth_protocol.hpp"
#include "airpods_playback.hpp"
#include "pico-v090/RemoteControl.hpp"
#include "pico-v090/PcmGain.hpp"
#include "pico-v090/UseLease.hpp"
#include "pico-v090/CommandLine.hpp"
#include "pico-v090/StreamPreroll.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static std::vector<int16_t> resample(uint32_t rate,const std::vector<int16_t> &input,bool split) {
    tab5bt::Resampler sampler;assert(sampler.configure(rate));
    std::vector<int16_t> output;
    const auto emit=[&](const int16_t *pcm,size_t frames){output.insert(output.end(),pcm,pcm+frames*2);return true;};
    size_t offset=0;
    while(offset<input.size()/2) {
        const size_t frames=std::min(input.size()/2-offset,split?size_t((offset*17+13)%1152+1):input.size()/2);
        assert(sampler.push(input.data()+offset*2,frames,emit));offset+=frames;
    }
    return output;
}
static std::vector<int16_t> sine(uint32_t rate,double frequency) {
    std::vector<int16_t> input(rate*2);
    for(size_t i=0;i<rate;i++)input[i*2]=input[i*2+1]=int16_t(std::lround(12000*std::sin(6.283185307179586*frequency*i/rate)));
    return input;
}
static double rms(const std::vector<int16_t> &samples) {
    double energy=0;size_t count=0;
    for(size_t i=2048;i<samples.size();i++){energy+=double(samples[i])*samples[i];++count;}
    return std::sqrt(energy/count);
}
static void legacy_stream_health_tests(){
    using Health=tab5bt::LegacyStreamHealth;
    for(uint32_t start:{0u,UINT32_MAX-2500}){
        Health health;const uint32_t initial_drop=UINT32_MAX-1000;
        assert(!health.observe(start,true,7,initial_drop,4096));
        for(unsigned second=1;second<=6;++second)
            assert(health.observe(start+second*1000,true,7,initial_drop+second*20000,4096)==(second==6));
        assert(!health.observe(start+7000,true,7,initial_drop+140000,4096));
    }
    // Full-ring overflow must be sustained and sampled at a plausible cadence.
    // Missing traffic, ordinary drop bursts and an empty input are not proof
    // that the receiver's playback callback stopped.
    for(unsigned mode=0;mode<5;++mode){
        Health health;uint32_t now=0,dropped=0;
        for(unsigned n=0;n<80;++n){
            const uint32_t step=mode==3?100:mode==4?3000:1000;
            now+=step;dropped+=mode==0?0:mode==1?1000:20000;
            assert(!health.observe(now,true,1,dropped,mode==2?512:4096));
        }
    }
    Health reset;uint32_t dropped=0;
    for(unsigned second=0;second<5;++second){dropped+=20000;assert(!reset.observe(second*1000,true,1,dropped,4096));}
    assert(!reset.observe(5000,false,1,dropped,4096)); // Pause cancels suspicion.
    for(unsigned second=6;second<11;++second){dropped+=20000;assert(!reset.observe(second*1000,true,2,dropped,4096));}
    assert(!reset.observe(11000,true,3,dropped+20000,4096)); // New session too.
    assert(!reset.observe(12000,true,3,0,4096)); // Receiver counter reset is not overflow.
    std::puts("Legacy stream health: sustained full-ring overflow only; quiet/drops/low-buffer/cadence false positives rejected, pause/revision reset, timer and dropped-counter wrap passed");
}
static void airpods_playback_tests(){
    using Playback=tab5bt::AirPodsPlayback;using Action=Playback::Action;
    Playback playback;
    assert(playback.update(true,true,0,true,false)==Action::None); // Initial out-of-ear never auto-pauses.
    assert(playback.update(true,true,2,true,false)==Action::None);
    assert(playback.update(true,true,1,true,false)==Action::Pause);
    assert(playback.update(true,true,1,false,true)==Action::None);
    assert(playback.update(true,true,2,false,true)==Action::Resume);
    assert(playback.update(true,true,2,false,true)==Action::None); // Resume is one-shot.
    assert(playback.update(true,true,1,true,false)==Action::Pause);
    playback.manual();assert(playback.update(true,true,2,false,true)==Action::None);
    // A fast reinsertion can arrive before the asynchronous pause finishes.
    assert(playback.update(true,true,0,true,false)==Action::Pause);
    assert(playback.update(true,true,2,true,false)==Action::None);
    assert(playback.update(true,true,2,false,true)==Action::Resume);
    for(unsigned invalid=0;invalid<4;invalid++){
        Playback p;assert(p.update(true,true,2,true,false)==Action::None);assert(p.update(true,true,1,true,false)==Action::Pause);
        assert(p.update(invalid!=0,invalid!=1,invalid==2?-1:invalid==3?3:1,false,true)==Action::None);
        assert(p.update(true,true,2,false,true)==Action::None); // Unknown/disconnect/disable revokes ownership.
    }
    Playback paused;assert(paused.update(true,true,2,false,true)==Action::None);
    assert(paused.update(true,true,0,false,true)==Action::None);assert(paused.update(true,true,2,false,true)==Action::None);
    std::puts("AirPods auto-pause: initial/unknown states, owned pause/resume, manual cancellation, rapid reinsertion before pause ACK, disconnected/disabled revocation and one-shot resume passed");
}
int main() {
    legacy_stream_health_tests();
    airpods_playback_tests();
    for(uint32_t rate:{8000u,11025u,16000u,22050u,32000u,44100u,48000u,88200u,96000u}) {
        const auto input=sine(rate,1000),whole=resample(rate,input,false),split=resample(rate,input,true);
        assert(whole==split && whole.size()==44100*2);
        assert(rms(whole)>8000 && rms(whole)<8900);
        if(rate==44100)assert(whole==input);
    }
    const double pass=rms(resample(96000,sine(96000,1000),true));
    const double stop=rms(resample(96000,sine(96000,30000),true));
    assert(stop/pass<0.02);
    std::printf("96 -> 44.1 kHz FIR alias suppression at 30 kHz: %.1f dB\n",20*std::log10(stop/pass));
    tab5bt::Resampler invalid;assert(!invalid.configure(7999) && !invalid.configure(96001));
    assert(invalid.configure(48000));int16_t frame[4]{};
    assert(!invalid.push(frame,2,[](const int16_t*,size_t){return false;}));

    assert(tab5bt::mac_valid("aa:BB:01:23:45:67"));
    for(const char *bad:{"AA:BB:CC:DD:EE","AA:BB:CC:DD:EE:FFX","AA-BB:CC:DD:EE:FF","GG:00:00:00:00:00"})assert(!tab5bt::mac_valid(bad));
    bt_bridge_device_t device{};
    assert(tab5bt::device_line("DEVICE 1 aa:bb:cc:dd:ee:ff -54 1 1 AirPods Pro 2",device));
    assert(!std::strcmp(device.mac,"AA:BB:CC:DD:EE:FF") && !std::strcmp(device.name,"AirPods Pro 2") && device.saved && device.selected);
    for(const char *bad:{"DEVICE 0 aa:bb:cc:dd:ee:ff -54 1 1 x","DEVICE 1 aa:bb:cc:dd:ee:ff -128 1 1 x","DEVICE 1 aa:bb:cc:dd:ee:ff -54 2 1 x","DEVICE 1 invalid -54 1 1 x"})assert(!tab5bt::device_line(bad,device));
    long value=0;assert(tab5bt::integer_field("STATUS connection=2 audio=2","connection",value) && value==2);
    assert(!tab5bt::integer_field("STATUS notconnection=2","connection",value));
    assert(!tab5bt::integer_field("STATUS connection=2garbage","connection",value));
    uint32_t token=0;
    assert(tab5bt::unsigned_field("STREAM_FAULT token=4294967295 reason=INPUT_STALLED","token",token));
    assert(token==UINT32_MAX);
    assert(tab5bt::unsigned_field("STREAM_FAULT token=0","token",token)&&token==0);
    for(const char *bad:{"STREAM_FAULT token=", "STREAM_FAULT token=12bad",
            "STREAM_FAULT token=-1", "STREAM_FAULT token=+1", "STREAM_FAULT token=4294967296",
            "STREAM_FAULT token=12345678901", "STREAM_FAULT nottoken=123", "STREAM_FAULT reason=INPUT_STALLED"}){
        token=0x12345678;
        assert(!tab5bt::unsigned_field(bad,"token",token));
        assert(token==0x12345678); // Invalid reports must never replace the active session token.
    }
    std::puts("Unsigned protocol fields: full uint32 range, malformed/missing/overflow rejection preserves caller value");
    assert(tab5bt::remote_line("REMOTE PLAY")==BT_REMOTE_PLAY && tab5bt::remote_line("REMOTE PLAY NOW")==BT_REMOTE_NONE);
    tab5bt::VolumeUpdate volume;volume.queue(30);assert(volume.take(UINT32_MAX-100)==30);
    volume.acknowledge(20);assert(volume.busy() && volume.take(50)==-1 && volume.take(2000)==30);
    volume.queue(70);volume.acknowledge(30);assert(volume.take(2100)==70);volume.acknowledge(70);assert(!volume.busy());
    using Handshake=tab5bt::StreamHandshake;
    Handshake handshake;handshake.update(true,true,1);
    const auto first=handshake.next(10);assert(first.command==Handshake::Command::Stream && first.tagged);
    assert(!handshake.acknowledge(first.token,false,1));
    assert(handshake.acknowledge(first.token,true,1));
    assert(handshake.matches(first.token,1));assert(!handshake.matches(first.token+1,1));assert(!handshake.matches(first.token,2));
    /* PAUSE/PLAY can both occur between UART task iterations. The changed
     * revision must still force a new handshake, regardless of final playing. */
    handshake.update(true,true,3);const auto resumed=handshake.next(20);
    assert(!handshake.matches(first.token,3)&&!handshake.matches(resumed.token,3));
    assert(resumed.command==Handshake::Command::Stream && resumed.token!=first.token);
    assert(!handshake.acknowledge(first.token,true,3));
    assert(!handshake.acknowledge(resumed.token,true,4));
    assert(handshake.next(519).command==Handshake::Command::None);
    const auto retry=handshake.next(520);assert(retry.token==resumed.token && retry.command==Handshake::Command::Stream);
    assert(handshake.acknowledge(resumed.token,true,3));
    assert(handshake.matches(resumed.token,3));
    /* New wake, late ACK from previous session, then matching ACK. */
    handshake.update(false,true,4);assert(handshake.next(530).command==Handshake::Command::Stop);
    handshake.update(true,true,5);const auto wake=handshake.next(540);
    assert(!handshake.acknowledge(resumed.token,true,5));assert(handshake.acknowledge(wake.token,true,5));
    Handshake legacy;legacy.update(true,false,1);
    assert(legacy.next(0).command==Handshake::Command::Stop);
    assert(!legacy.acknowledge(0,false,1));legacy.silent(1);
    assert(legacy.next(20).command==Handshake::Command::Stream);assert(legacy.acknowledge(0,false,1));
    uint32_t unsigned_number=0;
    assert(tab5bt::unsigned_value("4294967295",unsigned_number) && unsigned_number==UINT32_MAX);
    assert(!tab5bt::unsigned_value("4294967296",unsigned_number));

    CommandLine line;
    for(char c:std::string("USE\r"))assert(line.put(c)==CommandLine::Result::More);
    assert(line.put('\n')==CommandLine::Result::Ready && !std::strcmp(line.text(),"USE"));line.reset();
    for(unsigned i=0;i<100;i++)line.put('A');assert(line.put('\n')==CommandLine::Result::Overflow);line.reset();
    line.put('U');line.put(0);assert(line.put('\n')==CommandLine::Result::Overflow);line.reset();
    unsigned number=0;assert(commandNumber("100",100,number) && number==100);
    for(const char *bad:{"","-1","101","1x","999999999999999999"})assert(!commandNumber(bad,100,number));
    assert(commandNumber("4294967295",UINT32_MAX,number) && number==UINT32_MAX);
    assert(!commandNumber("4294967296",UINT32_MAX,number));
    uint8_t mac[6]{};assert(commandMac("AA:01:02:03:04:FF",mac) && mac[0]==170 && mac[5]==255);
    assert(!commandMac("AA-01:02:03:04:FF",mac));
    remote::Inbox remote;assert(!remote.key(0x44,false,1));remote.connected(true);
    assert(remote.key(0x44,false,10) && !remote.key(0x44,false,11));
    assert(!remote.key(0x44,true,12) && remote.pop(13)==remote::Action::Play);
    assert(remote.key(0x4b,false,20));remote.connected(false);remote.connected(true);assert(remote.pop(21)==remote::Action::None);
    assert(remote.key(0x46,false,50));assert(remote.pop(2051)==remote::Action::None);
    struct Frame {int16_t channel1,channel2;};Frame pcm[300];
    PcmGain gain;gain.target(100);for(auto &f:pcm)f={32767,-32768};gain.block(pcm,300);assert(pcm[299].channel1==32767 && pcm[299].channel2==-32768);
    gain.target(50);for(auto &f:pcm)f={20000,-20000};gain.block(pcm,300);assert(pcm[299].channel1==5000 && pcm[299].channel2==-5000);
    gain.target(0);gain.block(pcm,300);for(auto &f:pcm)f={123,-123};gain.block(pcm,300);for(const auto &f:pcm)assert(!f.channel1 && !f.channel2);
    UseLease lease;lease.renew(UINT32_MAX-100);assert(!lease.expired(100) && lease.expired(15000));
    StreamPreroll preroll;assert(!preroll.ready(512,512,1));assert(preroll.ready(768,512,1));
    assert(preroll.ready(128,512,1));preroll.underflow();assert(!preroll.ready(512,512,1));
    assert(preroll.ready(1024,512,1));assert(!preroll.ready(100,512,2));
    std::puts("Bluetooth tests: FIR rates/chunk invariance, protocol bounds, MAC selection, volume ACK, Pico framing, AVRCP, ramps and lease/preroll passed");
}
