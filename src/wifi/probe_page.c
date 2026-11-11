/* probe_page.c - /probe: the RAM probe for a person.  Its state, live, from GET /bbc, a few
   buttons that use the same routes, and the routes themselves.  The page is static; the script
   does the rest, so it costs the poll nothing between requests.  webserver.c serves it. */
#include <stddef.h>

#include "probe_page.h"

const char probe_page_off[] =
   "<h1>Probe</h1>"
   "<div class=\"card\"><p>The RAM probe isn't running on this Pi1MHz.</p></div>";

const char *const probe_page_body[] = {
   /* the state */
   "<style>pre{overflow-x:auto}</style><h1>Probe</h1>"
   "<div class=\"card\"><table>"
   "<tr><th>State</th><td id=\"st\">-</td></tr>"
   "<tr><th>Installed</th><td id=\"in\">-</td></tr>"
   "<tr><th>Stopped at</th><td id=\"rg\">-</td></tr>"
   "<tr><th>Resets seen</th><td id=\"rs\">-</td></tr>"
   "<tr><th>Last error</th><td id=\"er\" class=\"err\"></td></tr>"
   "<tr><th>Build</th><td id=\"bd\" class=\"muted\">-</td></tr>"
   "</table></div>",

   /* the buttons */
   "<div class=\"card\"><p>"
   "<input type=\"submit\" value=\"Halt\" onclick=\"act('/bbc/halt?rung=1')\"> "
   "<input type=\"submit\" value=\"Continue\" onclick=\"act('/bbc/continue')\"></p>"
   "<p>Peek (while halted) &amp;<input id=\"pa\" value=\"7C00\" size=\"5\"> "
   "length <input id=\"pl\" value=\"64\" size=\"4\"> "
   "<input type=\"submit\" value=\"Peek\" onclick=\"peek()\"></p>"
   "<pre id=\"out\" class=\"muted\"></pre></div>",

   /* the install line, and what it opens */
   "<div class=\"card\"><p>On the BBC, once after switching on:</p>"
   "<pre>X%=23:CALL &amp;FC88</pre>"
   "<p class=\"muted\">It answers <i>Pi1MHz probe on</i>, and every Break after puts it back "
   "by itself (it sets <code>*KEY10</code>; <code>X%=23:Y%=1:CALL &amp;FC88</code> leaves yours); "
   "Ctrl-Break and power-on clear it.</p>"
   "<p><b>This opens the BBC to remote code execution over your WiFi network: anyone on it can "
   "then stop the BBC, read and write its memory and run code on it, until Ctrl-Break or "
   "power-off.</b></p></div>",

   /* the routes */
   "<div class=\"card\"><p>The API (docs/dev/ramprobe.md): every action is a POST; add "
   "<code>wait=1</code> and it answers when done, or poll <a id=\"js\" href=\"/bbc\">GET /bbc</a> "
   "until <code>busy</code> is false.  <code>rung</code> is how the BBC is asked in: 0 the vsync "
   "event (the default), 1 an IRQ, 2 an NMI, which also needs <code>nmi=1</code> (for a BBC that "
   "answers nothing else: after a disc access the NMI lands in the DFS's code).</p><pre>"
   "GET  /bbc                               status, JSON\n"
   "POST /bbc/halt[?rung=]                  stop the BBC\n"
   "POST /bbc/continue[?pc=&amp;a=&amp;x=&amp;y=&amp;p=]       let it go on\n"
   "POST /bbc/peek?addr=&amp;len=[&amp;bank=&amp;rung=]   read up to 64 KB, then GET /bbc/data\n"
   "POST /bbc/poke?addr=[&amp;hex=&amp;rung=]       write: hex, or up to 32 KB as the body\n"
   "POST /bbc/jsr?addr=[&amp;a=&amp;x=&amp;y=]          call code\n"
   "POST /bbc/run?hex=[&amp;a=&amp;x=&amp;y=]           run up to 80 bytes of code\n"
   "GET  /bbc/data[?offset=&amp;len=]            the bytes peeked, hex</pre></div>",

   /* the script: the state, once a second, and the buttons */
   "<script>"
   "function h(n,w){return n.toString(16).toUpperCase().padStart(w,'0')}"
   "function show(s){"
   "document.getElementById('st').textContent=s.state=='waiting'?"
   "'waiting: nothing is known of the BBC until the install line is typed':s.state+(s.busy?' (busy)':'');"
   "document.getElementById('in').textContent=s.installed?'yes':'no: type the line below';"
   "var r=s.stopped;document.getElementById('rg').textContent=s.state=='halted'?"
   "'PC &'+h(r.pc,4)+'  A &'+h(r.a,2)+'  X &'+h(r.x,2)+'  Y &'+h(r.y,2)+'  P &'+h(r.p,2)+'  S &'+h(r.s,2):'-';"
   "document.getElementById('rs').textContent=s.resets;"
   "document.getElementById('er').textContent=s.error;"
   "document.getElementById('bd').textContent='api '+s.api+', '+s.build}"
   "function get(){return fetch('/bbc').then(function(r){return r.json()})}"
   "function wait(){return get().then(function(s){show(s);"
   "return s.busy?new Promise(function(k){setTimeout(k,20)}).then(wait):s})}"
   "function act(p){return fetch(p,{method:'POST'}).then(wait)}",

   "function peek(){var a=parseInt(document.getElementById('pa').value,16),"
   "n=parseInt(document.getElementById('pl').value,10),o=document.getElementById('out');"
   "act('/bbc/peek?addr='+a+'&len='+n+(s0.state=='running'?'&rung=1':'')).then(function(s){if(s.error){o.textContent='';return}"
   "return fetch('/bbc/data?len='+n).then(function(r){return r.text()}).then(function(t){"
   "var x='';for(var i=0;i<t.trim().length/2;i+=16)x+=h(a+i,4)+'  '+t.substr(2*i,32).replace(/(..)/g,'$1 ')+'\\n';"
   "o.textContent=x})})}"
   "var s0={};"
   "function tick(){get().then(function(s){s0=s;show(s)}).catch(function(){}).then(function(){setTimeout(tick,1000)})}"
   "tick();"
   "</script>",

   NULL
};
