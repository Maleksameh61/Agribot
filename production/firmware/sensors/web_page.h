// AgriBot Field OS - the page Sensors serves at http://192.168.4.1
// Join the "AgriBot" WiFi on your phone, open that address, and "Add to Home Screen".
#pragma once

static const char WEB_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ar" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-capable" content="yes">
<title>AgriBot Field OS</title>
<style>
:root{--bg:#0f1712;--card:#18241c;--line:#2a3b2f;--txt:#e7f0e9;--dim:#8fa596;--green:#3ecf6e;--red:#e5484d;--amber:#f5a524}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--txt);font:15px system-ui,"Segoe UI",Tahoma,sans-serif;padding:10px;padding-bottom:40px}
h2{font-size:14px;color:var(--dim);margin:14px 4px 6px;font-weight:600}
.top{display:flex;align-items:center;gap:8px;justify-content:space-between}
.top b{font-size:18px}.dot{width:10px;height:10px;border-radius:50%;background:var(--red);display:inline-block}
.dot.on{background:var(--green)}
.pill{background:var(--card);border:1px solid var(--line);border-radius:99px;padding:3px 10px;font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:10px}
#status{margin-top:8px;font-size:14px}
.cam{position:relative;aspect-ratio:4/3;background:#000;border-radius:14px;overflow:hidden;margin-top:8px}
.cam img{width:100%;height:100%;object-fit:cover}
.cam span{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:var(--dim)}
.cam .tag{position:absolute;inset:auto auto 8px 8px;background:#000a;padding:2px 8px;border-radius:8px;font-size:12px}
.row{display:flex;gap:8px;margin-top:8px}.row>*{flex:1}
button{background:#22342a;color:var(--txt);border:1px solid var(--line);border-radius:12px;padding:12px 6px;font:inherit;font-weight:600}
button:active{filter:brightness(1.3)}
button.stop{background:var(--red);border-color:var(--red);font-size:20px;padding:16px}
button.go{background:#1f5b35;border-color:var(--green)}
#joy{position:relative;height:210px;touch-action:none;background:radial-gradient(circle,#22342a 0,#18241c 70%);border-radius:14px;border:1px solid var(--line);margin-top:8px}
#knob{position:absolute;width:70px;height:70px;border-radius:50%;background:var(--green);opacity:.85;left:50%;top:50%;transform:translate(-50%,-50%);pointer-events:none}
#joy small{position:absolute;top:6px;right:10px;color:var(--dim)}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:6px}
.kv{background:#132018;border-radius:10px;padding:7px 9px}.kv i{display:block;font-style:normal;color:var(--dim);font-size:12px}
.kv b{font-size:17px}
label{display:flex;align-items:center;gap:8px;margin-top:8px}input[type=range]{flex:1}
ol{margin:6px 0;padding-inline-start:22px;max-height:160px;overflow:auto;font-size:13px;direction:ltr;text-align:left}
a{color:var(--green)}
</style></head><body>
<div class="top"><b>AgriBot</b><span class="pill"><span id="ws" class="dot"></span> <span id="mode">...</span></span><span class="pill" id="batt">-- V</span></div>
<div class="card" id="status">بيوصل...</div>

<div class="cam"><span>الكاميرا مش شغالة</span><img id="cam" alt=""><div class="tag" id="camtag"></div></div>

<div class="row"><button class="stop" onclick="cmd('stop')">وقّف ✋</button></div>

<h2>سواقة يدوي</h2>
<div id="joy"><small>اسحب عشان تسوق</small><div id="knob"></div></div>
<label>يمين/شمال الكاميرا<input type="range" id="pan" min="0" max="180" value="90"></label>
<label>فوق/تحت الكاميرا<input type="range" id="tilt" min="0" max="180" value="90"></label>
<div class="row"><button onclick="cmd('horn')">كلاكس 📢</button><button onclick="cmd('center')">الكاميرا في النص</button></div>

<h2>المهمة</h2>
<div class="card">
 <div id="wpinfo">مفيش نقط</div>
 <ol id="wplist"></ol>
 <label><input type="checkbox" id="wpprobe" checked> قيس التربة عند النقطة دي</label>
 <div class="row"><button onclick="cmd('wp_here',{probe:wpprobe.checked?1:0})">ضيف مكاني دلوقتي 📍</button><button onclick="if(confirm('تمسح كل النقط؟'))cmd('wp_clear')">امسح الكل</button></div>
 <div class="row"><button class="go" onclick="cmd('start')">ابدأ / كمّل ▶</button><button onclick="cmd('pause')">استنى ⏸</button></div>
</div>

<h2>المجس</h2>
<div class="row"><button onclick="cmd('probe')">نزّل المجس واقرا ⬇</button><button onclick="cmd('raise')">طلّع المجس ⬆</button><button onclick="cmd('home')">ظبط المجس</button></div>

<h2>آخر قراية للتربة</h2>
<div class="grid" id="soil"></div>
<h2>الجو</h2>
<div class="grid" id="env"></div>
<h2>الروبوت</h2>
<div class="grid" id="robot"></div>
<p><a href="/log.csv">نزّل كل القرايات (CSV)</a></p>

<script>
const STEP={idle:'واقف',wait_gps:'مستني الـGPS',acquire:'بيعرف هو باصص فين',turn:'بيلف ناحية النقطة',drive:'ماشي للنقطة',avoid:'بيتفادى حاجة قدامه',probing:'بيقيس التربة',paused:'المهمة واقفة مؤقتاً',done:'خلّص المهمة 🎉',aborted:'المهمة اتلغت'};
const MODE={idle:'واقف',manual:'يدوي',mission:'مهمة'};
const PROBE=['مش متظبط','فوق','بينزل','تحت','بيطلع','بيتظبط','فيه مشكلة'];
let ws,st={},drive={t:0,s:0},sending=null;
const $=id=>document.getElementById(id);
function connect(){
 ws=new WebSocket('ws://'+location.host+'/ws');
 ws.onopen=()=>{$('ws').classList.add('on');sending=setInterval(tick,50)};
 ws.onclose=()=>{$('ws').classList.remove('on');clearInterval(sending);setTimeout(connect,1000)};
 ws.onmessage=e=>{try{st=JSON.parse(e.data);render()}catch(x){}};
}
function tick(){ if(ws.readyState==1) ws.send(JSON.stringify({c:'d',t:drive.t,s:drive.s,p:+$('pan').value,ti:+$('tilt').value})) }
function cmd(c,extra){ if(c=='center'){$('pan').value=90;$('tilt').value=90;return}
 if(navigator.vibrate)navigator.vibrate(30); if(ws&&ws.readyState==1) ws.send(JSON.stringify(Object.assign({c},extra||{}))) }
function kv(el,items){ el.innerHTML=items.map(([k,v])=>`<div class="kv"><i>${k}</i><b>${v}</b></div>`).join('') }
function f(v,d,u){ return (v===null||v===undefined)?'--':(+v).toFixed(d)+(u||'') }
function render(){
 $('mode').textContent=MODE[st.mode]||st.mode;
 $('batt').textContent=f(st.batt,1,' V'); $('batt').style.color=st.batt<11.6?'var(--red)':'';
 $('status').textContent=(STEP[st.step]||st.step)+(st.msg?' — '+st.msg:'');
 $('wpinfo').textContent=st.wpn?`النقطة ${Math.min(st.wpi+1,st.wpn)} من ${st.wpn}`:'مفيش نقط — روح مكان وداس "ضيف مكاني"';
 if(st.wps) $('wplist').innerHTML=st.wps.map((w,i)=>`<li${i==st.wpi?' style="color:var(--green)"':''}>${w[0].toFixed(6)}, ${w[1].toFixed(6)}${w[2]?' 🧪':''}</li>`).join('');
 const s=st.soil||{};
 kv($('soil'),[['الرطوبة',f(s.m,1,'%')],['حرارة التربة',f(s.t,1,'°')],['الملوحة EC',f(s.ec,0,' µS/cm')],['الحموضة pH',f(s.ph,1)],['نيتروجين N',f(s.n,0,' mg/kg')],['فوسفور P',f(s.p,0,' mg/kg')],['بوتاسيوم K',f(s.k,0,' mg/kg')],['وقت القراية',s.at||'--']]);
 kv($('env'),[['حرارة الجو',f(st.airT,1,'°')],['رطوبة الجو',f(st.hum,0,'%')],['الضغط',f(st.press,0,' hPa')],['الإضاءة',f(st.lux,0,' lux')]]);
 kv($('robot'),[['أقمار GPS',st.sats??'--'],['الاتجاه',st.hdgOk?f(st.hdg,0,'°'):'لسه'],['الميلان',f(st.roll,0,'°')+' / '+f(st.pitch,0,'°')],['قدامه (شمال/يمين)',(st.usL>4000?'—':f(st.usL/10,0))+' / '+(st.usR>4000?'—':f(st.usR/10,0))+' سم'],
  ['المجس',(PROBE[st.probe]||'?')+(st.probeMm?' '+st.probeMm+'mm':'')],['Motion',st.lm?'✅':'❌'],['Audio',st.la?'✅':'❌'],['الشريحة',st.sim>=3?'✅ '+st.rssi:(st.sim==2?'مش متسجلة':'❌')]]);
 $('camtag').textContent=st.cam||'';
}
// joystick
const joy=$('joy'),knob=$('knob');
function joyMove(e){ const r=joy.getBoundingClientRect(); let x=(e.clientX-r.left)/r.width*2-1, y=(e.clientY-r.top)/r.height*2-1;
 x=Math.max(-1,Math.min(1,x)); y=Math.max(-1,Math.min(1,y));
 knob.style.left=((x+1)/2*100)+'%'; knob.style.top=((y+1)/2*100)+'%';
 // screen is RTL but the stick isn't: right on the screen = turn right
 drive.t=Math.round(-y*1000); drive.s=Math.round(x*1000); }
function joyEnd(){ drive.t=0;drive.s=0;knob.style.left='50%';knob.style.top='50%' }
joy.addEventListener('pointerdown',e=>{joy.setPointerCapture(e.pointerId);joyMove(e)});
joy.addEventListener('pointermove',e=>{if(e.buttons||e.pointerType=='touch')joyMove(e)});
joy.addEventListener('pointerup',joyEnd); joy.addEventListener('pointercancel',joyEnd);
document.addEventListener('visibilitychange',()=>{if(document.hidden)joyEnd()});
// camera: the CAM has its own IP on the AgriBot WiFi
const cam=$('cam'); cam.onerror=()=>{cam.style.display='none';setTimeout(()=>{cam.style.display='';cam.src='http://192.168.4.10:81/stream?'+Date.now()},3000)};
cam.src='http://192.168.4.10:81/stream';
connect();
</script></body></html>)HTML";
