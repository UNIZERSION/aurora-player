#ifdef NDEBUG
#undef NDEBUG
#endif
#define PICO_AIRPODS_HOST_TEST 1
#include "airpods_mocks.hpp"
std::atomic<int> connection{ESP_A2D_CONNECTION_STATE_CONNECTED};
#include "../native-idf/main/airpods.cpp"
#include <cassert>
struct Done{};
int main(){
 const uint8_t address[]={1,2,3,4,5,6};
 airpods_start(address,"Test AirPods Pro");task=reinterpret_cast<void*>(1);
 Print out;airpods_status(out);assert(out.output.find("mode=-1")!=std::string::npos);
 assert(airpods_command("AIRPODS MODE 0",out));assert(pendingMode==0);
 assert(airpods_command("AIRPODS AUTO_PAUSE 0",out));assert(!autoPause);
 assert(settings.getBool("auto_pause",true)==false);
 bool commanded=false,confirmed=false,accepted=false,timeoutChecked=false,disconnected=false;
 onStep=[&] {
  if(mock_ms>2200&&!commanded) {
   assert(connects==1&&sentPackets.size()==3&&linkState==1);
   assert(sentPackets[0]==std::vector<uint8_t>(airpods::handshake,airpods::handshake+sizeof(airpods::handshake)));
   assert(!memcmp(address,connectedPeer,6));assert(published.mode==-1);
   airpods_command("AIRPODS MODE 4",out);commanded=true;
  }
  if(mock_ms>2300&&!confirmed) {
   assert(sentPackets.size()==4&&sentPackets[3][7]==4&&published.mode==-1);
   // The sink declines Adaptive and reports ANC: display actual mode, never guessed request.
   receivedPackets.push_back({4,0,4,0,9,0,0x0d,2,0,0,0});
   receivedPackets.push_back({4,0,4,0,4,0,1,4,1,99,2,1});
   confirmed=true;
  }
  if(mock_ms>4400&&!timeoutChecked) {
   assert(accepted&&published.mode==-1&&linkState==0&&closes==1);
   assert(Serial2.output.find("mode_not_confirmed")!=std::string::npos);
   airpods_command("AIRPODS DISCONNECT",out);timeoutChecked=true;
  }
  if(mock_ms>2400&&!accepted) {
   assert(published.mode==2&&published.left==99&&published.right==-1);accepted=true;
  }
  if(mock_ms>4500&&!disconnected) {
   assert(linkState==0&&published.mode==-1&&closes==1);disconnected=true;
   throw Done{};
  }
 };
 try{worker(nullptr);}catch(const Done&){}
 assert(commanded&&confirmed&&timeoutChecked&&disconnected);
 esp_a2d_mcc_t codec;pico_codec_config(codec);out.output.clear();pico_codec_status(out);
 assert(out.output=="CODEC type=SBC rate=44100 channels=2 bitpool_min=2 bitpool_max=53\n");
 puts("PASS actual AirPods worker: authenticated selected-peer L2CAP, handshake order, bounded asynchronous mode request, confirmed sink state, rejection timeout, battery report, saved auto-pause, channel close reset and negotiated SBC report. Mock SDK, no hardware claim.");
}
