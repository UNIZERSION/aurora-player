#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#ifdef BASELINE
#include "fixtures/main090.cpp"
#else
#include "../src/main.cpp"
#endif
#include "bluetooth_protocol.hpp"
#include "bluetooth_dsp.hpp"
struct ReaderDone {};
static void start_session(){connectionChanged(ESP_A2D_CONNECTION_STATE_CONNECTED,nullptr);audioChanged(ESP_A2D_AUDIO_STATE_STARTED,nullptr);Print out;command("STREAM 100",out);assert(out.output=="STREAM_ACK 100\n");}
static void feed_block(int16_t sample,bool transition=false){
 int calls=0;mock_read=[&](void*dst,size_t bytes,size_t*read){if(calls++)throw ReaderDone{};
  for(size_t i=0;i<bytes/sizeof(Frame);i++)((Frame*)dst)[i]={sample,int16_t(-sample)};
  if(transition){Print out;command("STOP",out);command("STREAM 101",out);}
  *read=bytes;return ESP_OK;};
 try{i2sReader(nullptr);}catch(const ReaderDone&){}
}
static bool silent(const Frame*frames,size_t n){for(size_t i=0;i<n;i++)if(frames[i].channel1||frames[i].channel2)return false;return true;}
int main(int argc,char**argv){
 (void)argv;setup();start_session();
 if(argc>1){feed_block(12000,true);std::fprintf(stderr,"PCM queued across STOP/STREAM: %zu frames (expected 0)\n",ringUsed);assert(ringUsed==0);return 0;}
 Print out;Frame output[512];
 command("VOLUME 100",out);feed_block(12000);feed_block(12000);audio(output,512);assert(silent(output,512));
 feed_block(12000);audio(output,512);assert(output[511].channel1==12000&&output[511].channel2==-12000);
 audio(output,512);assert(emptyFrames==256&&streamUnderruns==1);
 audio(output,512);assert(silent(output,512));
 feed_block(12000);feed_block(12000);feed_block(12000);audio(output,512);assert(!silent(output,512));
 command("STOP",out);audio(output,512);assert(silent(output,512)&&ringUsed==0);
 start_session();for(int i=0;i<20;i++)feed_block(1000);assert(ringUsed==4096&&droppedFrames==1024);
 for(int i=0;i<8;i++)audio(output,512);assert(ringUsed==0);
 start_session();feed_block(8000,true);assert(ringUsed==0);for(int i=0;i<3;i++)feed_block(9000);audio(output,512);assert(output[511].channel1==9000);
 // Actual Pico command handler and Aurora's production handshake.
 tab5bt::StreamHandshake hs;hs.update(true,true,1);auto request=hs.next(0);char line[64];
 snprintf(line,sizeof(line),"STREAM %u",request.token);out.output.clear();command(line,out);
 uint32_t token=0;auto reply=out.output.substr(11);reply.pop_back();assert(tab5bt::unsigned_value(reply.c_str(),token));assert(hs.acknowledge(token,true,1));
 hs.update(true,true,3);request=hs.next(20);assert(!hs.acknowledge(token,true,3));snprintf(line,sizeof(line),"STREAM %u",request.token);out.output.clear();command(line,out);assert(out.output=="STREAM_ACK "+std::to_string(request.token)+"\n");assert(hs.acknowledge(request.token,true,3));
 command("STOP",out);assert(!streamEnabled&&ringUsed==0&&!bt.media_active);
 command("STREAM",out);assert(streamEnabled&&bt.media_active);connectionChanged(0,nullptr);out.output.clear();command("STREAM 99",out);assert(out.output.find("ERROR Headphones not connected")!=std::string::npos);audio(output,512);assert(silent(output,512));
 connectionChanged(2,nullptr);remoteKey(0x44,false);remoteKey(0x44,false);remoteKey(0x44,true);assert(remotePresses==2 && remoteReleases==1 && remoteAccepted==1 && lastRemoteKey==0x44 && lastRemoteRelease==1);Serial2.output.clear();drainRemote();assert(Serial2.output=="REMOTE PLAY\n");
 for(uint8_t key:{0x45,0x46,0x4B,0x4C}){remoteKey(key,false);remoteKey(key,true);}Serial2.output.clear();drainRemote();assert(Serial2.output=="REMOTE PAUSE\nREMOTE PAUSE\nREMOTE NEXT\nREMOTE PREVIOUS\n");
 remoteKey(0x4B,false);connectionChanged(0,nullptr);connectionChanged(2,nullptr);Serial2.output.clear();drainRemote();assert(Serial2.output.empty());
 command("VOLUME 50",out);assert(volumePercent==50);for(int i=0;i<4;i++)feed_block(16000);audio(output,512);assert(output[511].channel1==4000);command("VOLUME 101",out);assert(volumePercent==50);
 command("STATUS",out);assert(out.output.find("version=0.9.")!=std::string::npos);
 // A lost ACK and retransmitted token must preserve queued PCM and generation.
 start_session();for(int i=0;i<4;i++)feed_block(9000);
 const auto duplicateEpoch=ringEpoch.load();const size_t duplicateSize=ringUsed;
 out.output.clear();command("STREAM 100",out);
 assert(out.output=="STREAM_ACK 100\n" && ringEpoch==duplicateEpoch && ringUsed==duplicateSize);
 // The SDK flush control reaches the application without unsigned len wrap.
 const auto beforeFlush=sdkFlushes.load();assert(audio(nullptr,-1)==0);
 assert(ringUsed==0 && sdkFlushes==beforeFlush+1 && ringEpoch!=duplicateEpoch);
 assert(audio(nullptr,0)==0);assert(audio(output,0)==0);
 // Exercise the real main-loop watchdog and its tagged bridge messages.
 bluetoothRunning=true;command("STREAM 9001",out);Serial2.output.clear();
 const auto beforeRecovery=bt.media_recoveries;
 for(unsigned i=0;i<40;i++){mock_ms+=100;receivedBlocks.fetch_add(17);loop();}
 assert(bt.media_recoveries==beforeRecovery+1);
 assert(Serial2.output.find("RECOVERY 9001 TRANSPORT_STALLED\n")!=std::string::npos);
 Serial2.output.clear();for(unsigned i=0;i<40;i++){mock_ms+=100;loop();}
 assert(Serial2.output.find("STREAM_FAULT 9001 INPUT_STALLED\n")!=std::string::npos);
 command("STOP",out);Serial2.output.clear();
 for(unsigned i=0;i<40;i++){mock_ms+=100;useLease.renew(mock_ms);loop();}
 assert(Serial2.output.find("RECOVERY")==std::string::npos);
 assert(Serial2.output.find("STREAM_FAULT")==std::string::npos);
 command("STREAM 9002",out);Serial2.output.clear();
 const auto limitedBefore=bt.media_recoveries;
 for(unsigned i=0;i<520;i++){mock_ms+=100;useLease.renew(mock_ms);receivedBlocks.fetch_add(17);loop();}
 assert(bt.media_recoveries==limitedBefore+3&&!streamEnabled&&!bt.media_active);
 assert(Serial2.output.find("STREAM_FAULT 9002 TRANSPORT_FAILED\n")!=std::string::npos);
 command("STREAM 9003",out);assert(streamEnabled&&bt.media_active);
 useLease.renew(mock_ms);start_session();
 for(unsigned rate:{8000,11025,16000,22050,32000,44100,48000,88200,96000})for(unsigned speed:{50,75,100,125,150}){
  // Engine produces base-rate PCM after its speed resampler. The BT wire format remains 44.1k/16-bit/stereo.
  tab5bt::Resampler sampler;assert(sampler.configure(rate));std::vector<int16_t> pcm(rate*2);for(size_t i=0;i<rate;i++){pcm[2*i]=int16_t(10000*std::sin(6.283185307*440*speed/100*i/rate));pcm[2*i+1]=-pcm[2*i];}
  size_t n=0;assert(sampler.push(pcm.data(),rate,[&](const int16_t*p,size_t count){for(size_t i=0;i<count;i++)assert(std::abs(int(p[2*i])+int(p[2*i+1]))<=1);n+=count;return true;}));assert(n==44100);
 }

 // 20,000 deterministic scheduling operations with a separate FIFO oracle.
 command("VOLUME 100",out);start_session();for(int i=0;i<4;i++)feed_block(1000);audio(output,512);start_session();
 std::deque<Frame> oracle;bool primed=false;uint32_t random=0x9ac57213;unsigned sessions=0,underflows=0;
 for(unsigned tick=0;tick<20000;tick++){
  random=random*1664525u+1013904223u;
  if((random>>28)==0){command("STOP",out);command("STREAM 700",out);oracle.clear();primed=false;++sessions;continue;}
  if(random&1){const int16_t sample=int16_t(1+random%25000);feed_block(sample);
   for(unsigned i=0;i<256;i++){if(oracle.size()==ringCapacity)oracle.pop_front();oracle.push_back({sample,int16_t(-sample)});}
  }else{
   const size_t count=(random&2)?512:128;Frame block[512];
   if(!primed&&oracle.size()>=std::max<size_t>(768,count))primed=true;
   audio(block,int32_t(count));
   for(size_t i=0;i<count;i++){
    Frame expected{};if(primed&&!oracle.empty()){expected=oracle.front();oracle.pop_front();}
    assert(block[i].channel1==expected.channel1&&block[i].channel2==expected.channel2);
   }
   if(primed&&oracle.empty()&&emptyFrames.load()>underflows){primed=false;underflows=emptyFrames.load();}
  }
  assert(ringUsed==oracle.size());
 }
 // Paused diagnostic tone restarts transport only for its duration, and a
 // later STOP cancels a diagnostic that was still waiting for A2DP start.
 command("STOP",out);audioChanged(ESP_A2D_AUDIO_STATE_STOPPED,nullptr);const auto tones=toneRequest.load();
 command("TEST",out);assert(testPending && bt.media_active);loop();assert(toneRequest==tones);
 audioChanged(ESP_A2D_AUDIO_STATE_STARTED,nullptr);loop();assert(!testPending && toneRequest==tones+1 && !abortTone);
 // T1 completes just before TEST T2 is queued: pending T2 owns the transport.
 const uint32_t firstTone=toneRequest.load();toneFinishedRequest=firstTone;toneFinished=true;
 command("TEST",out);loop();assert(bt.media_active && toneRequest==firstTone+1 && !testPending);
 // T1's delayed callback arrives after T2 started: generation rejects it.
 toneFinishedRequest=firstTone;toneFinished=true;loop();assert(bt.media_active);
 // Only T2's own completion restores the paused transport.
 toneFinishedRequest=toneRequest.load();toneFinished=true;loop();assert(!bt.media_active && !streamEnabled);
 command("TEST",out);command("STOP",out);loop();assert(!testPending && !bt.media_active);
 // Every signed PCM16 value: unity is exact; at 50 percent, the quadratic
 // taper is 0.25 with bounded integer quantization, never added clipping.
 PcmGain exhaustive;Frame settle[512]{};exhaustive.target(100);exhaustive.block(settle,512);
 for(int n=-32768;n<=32767;n++){Frame f{int16_t(n),int16_t(n)};exhaustive.block(&f,1);assert(f.channel1==n&&f.channel2==n);}
 exhaustive.target(50);exhaustive.block(settle,512);
 for(int n=-32768;n<=32767;n++){Frame f{int16_t(n),int16_t(n)};exhaustive.block(&f,1);assert(f.channel1==n/4&&f.channel2==n/4);}
 std::printf("PASS randomized ring/session simulation: 20000 operations, %u STOP/STREAM cycles; exhaustive PCM16 gain 131072 samples.\n",sessions);
 command("VOLUME 62",out);try{command("SLEEP",out);assert(false);}catch(const Reset&){}
 assert(prefs.getUChar("volume")==62&&!streamEnabled&&restartIntent==sleepIntent);
 puts("PASS actual Pico main: stream/session ACK, pause/resume race, old buffer isolation, preroll/underflow/refill/overflow, volume, AVRCP, disconnect, 45 rate-speed format cases.");
}
