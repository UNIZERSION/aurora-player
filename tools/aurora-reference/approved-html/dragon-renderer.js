/* Shared dragon texture mesh. Gallery and player use the same accepted wing motion. */
(()=>{
'use strict';
const vertex=`
attribute vec2 aUV;
uniform float uTime;
uniform float uVariant;
uniform vec2 uFit;
varying vec2 vUV;
float ease(float a,float b,float x){return smoothstep(a,b,x);}
mat2 rotation(float a){return mat2(cos(a),sin(a),-sin(a),cos(a));}
void main(){
 vec2 q=aUV;
 float t=uTime;
 float v=uVariant;
 // Anchored wing deformation. The feather-free right bat wing hinges near the shoulder.
 float wing=ease(.56,.72,q.x)*(1.-ease(.61,.73,q.y))*ease(.14,.22,q.y);
 float tail=ease(.64,.94,q.y);
 float head=(1.-ease(.30,.48,q.y))*(1.-ease(.62,.80,q.x));
 float breath=sin(t*1.5707963);
 if(v>1.5&&v<2.5){
  // Full wing attachment, from the end of the nape to the start of the tail.
  // Endpoints are normalized coordinates in the supplied dragon artwork.
  vec2 nape=vec2(.555,.405);
  vec2 tailRoot=vec2(.775,.615);
  vec2 local=aUV-nape;
  vec2 along=normalize(tailRoot-nape);
  vec2 span=vec2(along.y,-along.x);
  float reach=max(0.,dot(local,span));
  // Follow the neck contour rather than using a fixed right-hand strip.
  // This brings the inner wing into the stroke while protecting head/horns.
  float neckEdge=mix(.635,.540,ease(.345,.445,aUV.y));
  float flightWing=ease(neckEdge,neckEdge+.020,aUV.x)
    *ease(.145,.205,aUV.y)*(1.-ease(.680,.730,aUV.y));
  float cycle=fract(t/1.65);
  // Shorter power stroke, longer recovery, with zero velocity at each reversal.
  float stroke=cycle<.42
    ?.5-.5*cos(3.14159265*cycle/.42)
    :.5+.5*cos(3.14159265*(cycle-.42)/.58);
  float phase=cycle*6.2831853;
  // Project the flap around the diagonal attachment axis. Displacement grows
  // from zero at that axis instead of stretching a narrow wing/body seam.
  vec2 flap=reach*(-.30*stroke*span+.10*stroke*along);
  float distal=ease(.05,.38,reach);
  flap+=along*(reach*.018*distal*sin(phase-.55));
  q+=flightWing*flap;
  // Tail movement starts only at the low curled tip, well below the hip.
  float tip=ease(.86,.985,aUV.y);
  q.x+=tip*.004*sin(phase-.85);
  q.y+=.0045*cos(phase-.6);
 }else if(v>.5){
  float phase=t*(v<1.5?2.0943951:v<2.5?4.1887902:1.5707963);
  float fold=.5-.5*cos(phase);
  float strength=v<1.5?.85:v<2.5?1.0:.14;
  vec2 hinge=vec2(.605,.462);
  vec2 local=q-hinge;
  vec2 flap=rotation(sin(phase)*.15*strength)*vec2(local.x*(1.-.49*fold*strength),local.y);
  flap.y-=.035*sin(phase)*strength;
  q+=wing*(flap-local);
  if(v>2.5){
   // Small breathing expansion and a head nod blend smoothly into the neck.
   float chest=exp(-dot((aUV-vec2(.47,.52))*vec2(4.,5.),(aUV-vec2(.47,.52))*vec2(4.,5.)));
   q+=vec2((aUV.x-.49)*.045,(aUV.y-.57)*.017)*chest*breath;
   vec2 neck=vec2(.49,.44);
   q+=head*(rotation(.028*breath)*(aUV-neck)-(aUV-neck));
   q.x+=tail*.028*sin(t*1.5707963-aUV.y*6.);
   q.y+=tail*.006*cos(t*1.5707963-aUV.y*4.);
  }
 }else{
  // Same one-degree sway and one-pixel float as the 84 x 96 player emblem.
  q=vec2(.5)+rotation(-.0174533*cos(t*1.5707963))*(q-vec2(.5));
  q.y+=.0104167*cos(t*1.5707963);
 }
 gl_Position=vec4((q-.5)*uFit*vec2(1.,-1.),0.,1.);
 vUV=aUV;
}`;
const fragment=`
precision mediump float;
uniform sampler2D uImage;
varying vec2 vUV;
void main(){gl_FragColor=texture2D(uImage,vUV);}`;
function compile(gl,type,code){const s=gl.createShader(type);gl.shaderSource(s,code);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(s));return s;}
function create(canvas,source,options={}){
 const gl=canvas.getContext('webgl',{alpha:true,antialias:true,premultipliedAlpha:false,preserveDrawingBuffer:false,powerPreference:'low-power'});
 if(!gl)throw Error('WebGL is unavailable. Open the preview in a browser with graphics acceleration enabled.');
 const program=gl.createProgram(),vs=compile(gl,gl.VERTEX_SHADER,vertex),fs=compile(gl,gl.FRAGMENT_SHADER,fragment);
 gl.attachShader(program,vs);gl.attachShader(program,fs);gl.linkProgram(program);
 if(!gl.getProgramParameter(program,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(program));
 gl.deleteShader(vs);gl.deleteShader(fs);gl.useProgram(program);
 const nx=72,ny=88,points=[],indices=[];
 for(let y=0;y<=ny;y++)for(let x=0;x<=nx;x++)points.push(x/nx,y/ny);
 for(let y=0;y<ny;y++)for(let x=0;x<nx;x++){const a=y*(nx+1)+x,b=a+1,c=a+nx+1,d=c+1;indices.push(a,b,c,b,d,c);}
 const vb=gl.createBuffer(),ib=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,vb);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(points),gl.STATIC_DRAW);
 gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,ib);gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,new Uint16Array(indices),gl.STATIC_DRAW);
 const location=gl.getAttribLocation(program,'aUV');gl.enableVertexAttribArray(location);gl.vertexAttribPointer(location,2,gl.FLOAT,false,0,0);
 const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);
 gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
 gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
 gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,source);
 const uniforms=Object.fromEntries(['uTime','uVariant','uFit'].map(n=>[n,gl.getUniformLocation(program,n)]));
 const renderer={canvas,draw({time=0,variant=2,colors=true}={}){
  if(gl.isContextLost())return;
  const dpr=Math.min(Math.max(devicePixelRatio||1,options.minPixelRatio||1),2),r=canvas.getBoundingClientRect();
  const width=Math.max(1,Math.round(r.width*dpr)),height=Math.max(1,Math.round(r.height*dpr));
  if(canvas.width!==width||canvas.height!==height){canvas.width=width;canvas.height=height;}
  gl.viewport(0,0,width,height);gl.clearColor(0,0,0,0);gl.clear(gl.COLOR_BUFFER_BIT);
  gl.useProgram(program);
  const aspect=source.naturalWidth/source.naturalHeight;
  const fit=Math.min(width/aspect,height)*(options.fit??.90);
  gl.uniform2f(uniforms.uFit,2*fit*aspect/width,2*fit/height);
  gl.uniform1f(uniforms.uTime,time);gl.uniform1f(uniforms.uVariant,variant);
  gl.drawElements(gl.TRIANGLES,indices.length,gl.UNSIGNED_SHORT,0);
  canvas.style.filter=colors?`hue-rotate(${(time*45)%360}deg)`:'none';
  canvas.dataset.pose=time.toFixed(3);canvas.dataset.variant=String(variant);
 }};
 canvas.addEventListener('webglcontextlost',event=>{event.preventDefault();options.onError?.('Graphics context lost. Reload the page to resume the animation.');});
 return renderer;
}
window.Tab5Dragon=Object.freeze({create});
})();
