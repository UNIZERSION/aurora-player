#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#ifdef AUDIO_WIRE_BASELINE
#include "fixtures/main094_audio_wire.cpp"
#else
#include "../src/main.cpp"
#endif
#include "bluetooth_protocol.hpp"

static void check_state(int sdk_state, int wire_state) {
  audioChanged(sdk_state, nullptr);
  Print out;
  command("STATUS", out);
  long value=-1;
  assert(tab5bt::integer_field(out.output.c_str(), "audio", value));
  if(value!=wire_state) std::fprintf(stderr,
      "STATUS audio: SDK=%d, wire=%ld, required=%d\n",sdk_state,value,wire_state);
  assert(value==wire_state);
  // This is the deployed Aurora receiver's predicate, independent of Pico SDK.
  assert((value==2)==(sdk_state==ESP_A2D_AUDIO_STATE_STARTED));

  Serial.output.clear();Serial2.output.clear();
  loop();
  const std::string expected="STATE connection=2 audio="+std::to_string(wire_state)+"\n";
  assert(Serial.output.find(expected)!=std::string::npos);
  assert(Serial2.output.find(expected)!=std::string::npos);
  assert(audioState.load()==sdk_state); // Wire normalization must not change DSP gating.
}

int main() {
  setup();connectionChanged(ESP_A2D_CONNECTION_STATE_CONNECTED,nullptr);
  check_state(ESP_A2D_AUDIO_STATE_STARTED,2);
  check_state(ESP_A2D_AUDIO_STATE_STOPPED,0);
  check_state(ESP_A2D_AUDIO_STATE_STARTED,2);
  check_state(ESP_A2D_AUDIO_STATE_SUSPEND,0);
  std::printf("PASS audio wire protocol: SDK STARTED=%d, STOPPED=%d; STATUS and STATE preserve Aurora audio=2 / 0.\n",
      ESP_A2D_AUDIO_STATE_STARTED,ESP_A2D_AUDIO_STATE_STOPPED);
}
