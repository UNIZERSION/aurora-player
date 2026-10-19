#ifdef NDEBUG
#undef NDEBUG
#endif
#include "particles_response.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>

using Model = aurora_particles::Response<56>;
using Input = std::array<float,32>;
static Input uniform(float level) {Input input;input.fill(level);return input;}
static void step(Model &model,const Input &input,float dt=.01f,bool playing=true) {
    model.advance(input.data(),input.size(),dt,playing);
}
static void settle(Model &model,const Input &input,unsigned frames=1000) {
    for(unsigned i=0;i<frames;++i)step(model,input);
}
static void check_bounds(const Model &model) {
    assert(std::isfinite(model.bass)&&model.bass>=0&&model.bass<=1.000001f);
    assert(std::isfinite(model.pulse)&&model.pulse>=0&&model.pulse<=.250001f);
    for(unsigned lane=0;lane<3;++lane)for(unsigned i=0;i<44;++i) {
        const auto p=model.point(lane,i);
        const float extent=p.radius+1.7f+1; // Existing soft_dot halo/AA support.
        assert(std::isfinite(p.x)&&std::isfinite(p.y));
        assert(p.x-extent>=0&&p.x+extent<453&&p.y-extent>=0&&p.y+extent<110);
        assert(p.radius>=1&&p.radius<3.55f&&p.alpha>=0&&p.alpha<=255);
        assert(p.glow_alpha>=0&&p.glow_alpha<=43&&p.core_alpha>=0&&p.core_alpha<=100);
    }
}
static void endpoints_and_faults() {
    Model silence;
    settle(silence,uniform(0));
    for(float b:silence.bands)assert(b==0);
    assert(silence.bass==0&&silence.bed==0&&silence.pulse==0);
    Model full;settle(full,uniform(1));
    for(float b:full.bands)assert(std::fabs(b-1)<1e-5f);
    assert(full.pulse<1e-5f); // A held note does not become a fake beat generator.
    settle(full,uniform(0));
    assert(full.bass<1e-8f&&full.pulse<1e-8f);
    Input invalid{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-5};
    Model safe;settle(safe,invalid);check_bounds(safe);assert(safe.bass==0);
    const auto frozen=safe;
    step(safe,uniform(1),0);step(safe,uniform(1),-1);step(safe,uniform(1),std::numeric_limits<float>::infinity());
    assert(safe.bass==frozen.bass&&safe.phase==frozen.phase);
    Model null;null.advance(nullptr,32,.01f,true);assert(null.bass==0);
    null.advance(invalid.data(),0,.01f,true);assert(null.bass==0);
    puts("PASS silence, steady tone, release to silence, invalid inputs/dt and absent bands.");
}
static void rise_release_and_frame_rate() {
    Model quick;
    float previous=0;
    for(unsigned i=0;i<10;++i){step(quick,uniform(.6f));assert(quick.bands[0]>=previous);previous=quick.bands[0];}
    const float target=aurora_particles::drive(.6f);
    const float expected=target*(1-std::exp(-.1f/.055f));
    assert(std::fabs(quick.bands[0]-expected)<1e-5f);
    assert(quick.bands[0]>.83f*target&&quick.bands[0]<.84f*target);
    for(unsigned i=0;i<10;++i){step(quick,uniform(0));assert(quick.bands[0]<=previous);previous=quick.bands[0];}
    assert(std::fabs(quick.bands[0]-expected*std::exp(-.1f/.180f))<1e-5f);
    Model ten,fifty;
    for(unsigned i=0;i<30;++i)step(ten,uniform(.6f),.01f);
    for(unsigned i=0;i<6;++i)step(fifty,uniform(.6f),.05f);
    assert(std::fabs(ten.bands[0]-fifty.bands[0])<1e-5f);
    assert(std::fabs(ten.bass-fifty.bass)<1e-5f);
    assert(std::fabs(ten.pulse-fifty.pulse)<.015f);
    Model stalled;step(stalled,uniform(.6f),10.f);Model capped;step(capped,uniform(.6f),.05f);
    assert(stalled.bass==capped.bass&&stalled.phase==capped.phase);
    const auto paused=quick;step(quick,uniform(.6f),.033f,false);assert(quick.phase==paused.phase);
    printf("PASS monotonic attack/release; band at100ms=%.5f (%.1f%% of target), 10/50ms frame-rate agreement and stall bound.\n",expected,100*expected/target);
}
static void real_onsets_and_noise() {
    Model music;settle(music,uniform(.2f));
    float max_pulse=0,max_radius_jump=0;
    auto prev=music.point(0,21);
    for(unsigned i=0;i<240;++i) {
        const float level=i%60<12?.7f:.2f; // Measured-band-like quarter-second notes.
        step(music,uniform(level));max_pulse=std::max(max_pulse,music.pulse);
        const auto p=music.point(0,21);max_radius_jump=std::max(max_radius_jump,std::fabs(p.radius-prev.radius));prev=p;
    }
    assert(max_pulse>.10f&&max_pulse<=.25f&&max_radius_jump<.2f);
    settle(music,uniform(.2f));assert(music.pulse<1e-5f);
    // Alternate +/-0.005 source fluctuation: check steady-state output ripple,
    // not moving ribbon coordinates (their phase is intentionally continuous).
    float low=1,high=0;
    for(unsigned i=0;i<200;++i){step(music,uniform(.2f+(i%2?.005f:-.005f)));if(i>100){low=std::min(low,music.bands[0]);high=std::max(high,music.bands[0]);}}
    assert(high-low<.0012f);
    printf("PASS real repeated onsets pulse=%.4f, largest10ms radius step=%.4fpx; input noise0.010 -> envelope ripple%.6f.\n",max_pulse,max_radius_jump,high-low);
}
static void visibility_and_geometry() {
    Model m;settle(m,uniform(.45f));m.phase=.8f;
    // Moderate sustained music need not add a third raster pass to every dot.
    assert(m.point(0,21).core_alpha==0);
    double old_area_light=0,new_area_light=0;
    for(unsigned lane=0;lane<3;++lane)for(unsigned i=0;i<44;++i){
        const float u=i/43.f,a=u*2*aurora_particles::pi+m.phase+lane*2*aurora_particles::pi/3;
        const float depth=(std::cos(a)+1)*.5f;
        const float size=.85f+depth*.7f+.45f*1.45f,alpha=35+depth*55+.45f*155;
        const auto p=m.point(lane,i);old_area_light+=size*size*alpha;new_area_light+=p.radius*p.radius*p.alpha;
    }
    const double ratio=new_area_light/old_area_light;
    assert(ratio>1.3&&ratio<1.9); // Moderate visibility increase, not a saturated field.
    // Exhaustively cover each full ribbon period at the maximum allowed envelope.
    for(float &b:m.bands)b=1;
    m.bass=1;m.pulse=.25f;
    for(unsigned i=0;i<1000;++i){m.phase=i*4*aurora_particles::pi/1000;check_bounds(m);}
    std::mt19937 rng(139);std::uniform_real_distribution<float> level(-.2f,1.2f),dt(.004f,.06f);
    for(unsigned i=0;i<1000;++i){Input input;for(float &v:input)v=level(rng);step(m,input,dt(rng));check_bounds(m);}
    printf("PASS moderate-level dot area*alpha ratio=%.3f; 264000 points retain halo bounds and finite colors/radii across phase/envelope extremes.\n",ratio);
}
int main(){endpoints_and_faults();rise_release_and_frame_rate();real_onsets_and_noise();visibility_and_geometry();puts("PASS PARTICLES response only; no rendering/audio/SD/device FPS claim.");}
