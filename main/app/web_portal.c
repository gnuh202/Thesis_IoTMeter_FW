#include "web_portal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp: Content-Type matching is case-insensitive */

/* sdkconfig.h must precede any #if CONFIG_* gate below: an undefined macro in a
 * preprocessor conditional silently evaluates to 0, which would compile the
 * calibration section out even when APP_WEB_CALIB_ENABLE is set. */
#include "sdkconfig.h"

#include "cert_store.h"
#include "config_manager.h"
/* energy_meter_task.h is needed only by the portal calibration section
 * (calib_format_profile / calib_auto_post_handler / the /save persistence hook).
 * With calibration compiled out the header is unused, so it rides the same gate
 * as its only users. */
#if CONFIG_APP_WEB_CALIB_ENABLE
#include "energy_meter_task.h"
#endif
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "web_portal";

static httpd_handle_t s_httpd;
static bool s_dns_running;
static TaskHandle_t s_dns_task;
static int s_dns_sock = -1;
static char s_session_token[17];

#define DNS_PORT 53
#define DNS_MAX_PACKET 512
#define DNS_QR_RESPONSE 0x8000
#define DNS_A_RECORD 1
#define DNS_IN_CLASS 1

static void captive_dns_task(void *arg)
{
    (void)arg;
    uint8_t rx[DNS_MAX_PACKET];
    uint8_t tx[DNS_MAX_PACKET];
    const uint32_t ap_ip = inet_addr("192.168.4.1");

    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_dns_sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (len < 12) {
            continue;
        }

        int q_end = 12;
        while (q_end < len && rx[q_end] != 0) {
            q_end += rx[q_end] + 1;
        }
        if (q_end + 5 > len) {
            continue;
        }

        memcpy(tx, rx, q_end + 5);
        tx[2] = 0x81; /* response + recursion desired */
        tx[3] = 0x80; /* recursion available, no error */
        tx[6] = 0x00;
        tx[7] = 0x01; /* one answer */
        tx[8] = 0x00;
        tx[9] = 0x00;
        tx[10] = 0x00;
        tx[11] = 0x00;

        int pos = q_end + 5;
        if (pos + 16 > (int)sizeof(tx)) {
            continue;
        }
        tx[pos++] = 0xC0;
        tx[pos++] = 0x0C; /* pointer to queried name */
        tx[pos++] = 0x00;
        tx[pos++] = DNS_A_RECORD;
        tx[pos++] = 0x00;
        tx[pos++] = DNS_IN_CLASS;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x3C; /* TTL 60s */
        tx[pos++] = 0x00;
        tx[pos++] = 0x04;
        memcpy(&tx[pos], &ap_ip, 4);
        pos += 4;

        sendto(s_dns_sock, tx, pos, 0, (struct sockaddr *)&from, from_len);
    }
}

static esp_err_t captive_dns_start(void)
{
    if (s_dns_running) {
        return ESP_OK;
    }

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 5, &s_dns_task) != pdPASS) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    s_dns_running = true;
    return ESP_OK;
}

static void captive_dns_stop(void)
{
    if (!s_dns_running) {
        return;
    }
    s_dns_running = false;
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);
        close(s_dns_sock);
        s_dns_sock = -1;
    }
    if (s_dns_task != NULL) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
}

#ifndef CONFIG_APP_CONSOLE_AUTH_ENABLE
#define CONFIG_APP_CONSOLE_AUTH_ENABLE 0
#endif
#ifndef CONFIG_APP_CONSOLE_AUTH_USERNAME
#define CONFIG_APP_CONSOLE_AUTH_USERNAME ""
#endif
#ifndef CONFIG_APP_CONSOLE_AUTH_PASSWORD
#define CONFIG_APP_CONSOLE_AUTH_PASSWORD ""
#endif

/*
 * GitHub Primer's visual language, written out by hand: its tokens (#0969da accent,
 * #1f883d primary button, #d1d9e0 borders, #1f2328 text, 6px radii, system font
 * stack) and its card / form / btn shapes. The published CSS cannot be used as-is —
 * the device serves this page from its own SoftAP with no route to the internet, so
 * a CDN link loads as an unstyled page, and shipping the built bundle would put a
 * Node toolchain in the middle of the ESP-IDF build for one page of markup.
 *
 * Deliberately a shade darker than stock Primer: the page ground is canvas.inset
 * (#eaeef2) rather than white, so the white cards and the inset panels inside them
 * read as three distinct layers. Still a light theme throughout.
 *
 * Motion is Primer's: 80ms ease-out on hover/press for anything clickable, plus a
 * 1px press travel so a tap feels answered on a touchscreen with no hover state.
 * All of it collapses under prefers-reduced-motion.
 *
 * Mobile-first: everything is one column, and the two-column form grid only turns
 * on from 640px. Fields are grid *cells* (.field wraps label+control) — when a
 * label and its input were siblings in the grid instead, each took its own cell
 * and a wide desktop window dealt them into unrelated columns.
 */
static const char *HTML_STYLE =
    "*,*::before,*::after{box-sizing:border-box}"
    ":root{--acc:#0969da;--acc-sub:#ddf4ff;--acc-bd:#54aeff66;"
    "--pri:#1f883d;--pri-h:#1a7f37;--pri-a:#197935;"
    "--ink:#1f2328;--body:#59636e;--muted:#6e7781;"
    "--line:#d1d9e0;--line-s:#d8dee4;--card:#fff;--inset:#f6f8fa;--inset-h:#eff2f5;"
    "--inset-a:#e6eaef;--canvas:#eaeef2;--dan:#d1242f;--dan-sub:#ffebe9;--dan-bd:#ff818266;"
    "--t:80ms cubic-bezier(.33,1,.68,1)}"
    "html{-webkit-text-size-adjust:100%}"
    /* .btn is inline-flex, which would otherwise beat the attribute's own
     * display:none — the "Delete file" button relies on [hidden] to stay hidden. */
    "[hidden]{display:none!important}"
    "body{margin:0;padding:16px;background:var(--canvas);color:var(--ink);"
    "font:400 14px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI','Noto Sans',Helvetica,Arial,sans-serif}"
    ".card{width:100%;max-width:840px;margin:0 auto;background:var(--card);border:1px solid var(--line);"
    "border-radius:6px;box-shadow:0 1px 3px #1f23280f;padding:16px}"
    "h1{margin:10px 0 4px;font-size:22px;font-weight:600;line-height:1.25}"
    "h2{margin:0 0 6px;font-size:16px;font-weight:600}h3{margin:0 0 4px;font-size:14px;font-weight:600}"
    "p{margin:6px 0;color:var(--body)}.muted{color:var(--muted);font-size:12px}"
    /* Small hint under a field label: same muted tone as .muted, but tied to the
     * field so it reads as part of the label, not page prose. */
    ".hint{display:block;margin:2px 0 0;font-size:12px;font-weight:400;color:var(--muted)}"
    "code{background:var(--inset);border:1px solid var(--line-s);border-radius:6px;padding:1px 5px;"
    "font:inherit;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px}"
    /* Primer Label: outlined, not filled. */
    ".badge{display:inline-block;background:var(--acc-sub);color:var(--acc);border:1px solid var(--acc-bd);"
    "border-radius:999px;padding:1px 10px;font-size:12px;font-weight:500}"
    ".nav{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}"
    ".nav a{color:var(--body);background:var(--card);border:1px solid var(--line);border-radius:6px;"
    "padding:5px 12px;font-size:13px;font-weight:500;text-decoration:none;"
    "transition:background var(--t),border-color var(--t),color var(--t)}"
    ".nav a:hover{background:var(--inset-h);border-color:var(--muted);color:var(--ink)}"
    ".nav a:active{background:var(--inset-a)}"
    ".section{margin-top:20px;padding-top:16px;border-top:1px solid var(--line-s)}"
    /* Collapsible sections: <details> with the same card border as .profile, but
     * full-width and without the per-device margin. The summary is the section
     * heading; the body is the section content. */
    "details.section{margin-top:20px;padding:0;border:1px solid var(--line-s);border-radius:6px;"
    "background:var(--card);transition:border-color var(--t),box-shadow var(--t)}"
    "details.section:hover{border-color:var(--line)}"
    "details.section[open]{box-shadow:0 1px 3px #1f23280f}"
    "details.section>summary{display:flex;align-items:center;gap:8px;padding:16px 16px 12px;"
    "font-size:16px;font-weight:600;color:var(--ink);cursor:pointer;list-style:none;"
    "transition:background var(--t)}"
    "details.section>summary:hover{background:var(--inset)}"
    "details.section>summary:active{background:var(--inset-a)}"
    "details.section>summary::-webkit-details-marker{display:none}"
    "details.section>summary::before{content:'\\25B8';display:inline-block;color:var(--muted);"
    "font-size:11px;transition:transform var(--t)}"
    "details.section[open]>summary::before{transform:rotate(90deg)}"
    "details.section[open]>summary{border-bottom:1px solid var(--line-s);border-radius:6px 6px 0 0}"
    "details.section>.sbody{padding:16px}"
    /* One column by default; two only when there is room for two. */
    ".row{display:grid;grid-template-columns:1fr;gap:14px;margin-top:14px}"
    ".field{min-width:0}.field.wide{grid-column:1/-1}"
    "label{display:block;margin-bottom:6px;font-size:14px;font-weight:600;color:var(--ink)}"
    /* 16px controls under 640px: iOS Safari zooms the page on focus below that. */
    ".input{display:block;width:100%;background:var(--card);color:var(--ink);border:1px solid var(--line);"
    "border-radius:6px;padding:6px 12px;font:inherit;font-size:16px;line-height:1.5;"
    "box-shadow:inset 0 1px 0 #1f23280a;transition:border-color var(--t),box-shadow var(--t)}"
    ".input::placeholder{color:var(--muted)}"
    ".input:hover{border-color:var(--muted)}"
    ".input:focus{outline:0;border-color:var(--acc);box-shadow:inset 0 1px 0 #1f232800,0 0 0 3px #0969da4d}"
    ".box{background:var(--inset);border:1px solid var(--line-s);border-radius:6px;padding:6px 12px;"
    "font-size:13px;overflow-wrap:anywhere}"
    /* Primer button: 1px press travel so a tap registers without a hover state. */
    ".btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;"
    "border:1px solid #1f883d;border-radius:6px;background:var(--pri);color:#fff;font:inherit;"
    "font-size:14px;font-weight:500;padding:6px 16px;cursor:pointer;text-decoration:none;"
    "box-shadow:0 1px 0 #1f23281a,inset 0 1px 0 #ffffff26;"
    "transition:background var(--t),border-color var(--t),box-shadow var(--t),transform var(--t)}"
    ".btn:hover{background:var(--pri-h)}"
    ".btn:active{background:var(--pri-a);box-shadow:inset 0 1px 3px #1f232840;transform:translateY(1px)}"
    ".btn:focus-visible{outline:0;box-shadow:0 0 0 3px #0969da4d}"
    ".btn.alt{background:var(--inset);color:var(--ink);border-color:var(--line)}"
    ".btn.alt:hover{background:var(--inset-h);border-color:var(--muted)}"
    ".btn.alt:active{background:var(--inset-a)}"
    ".btn.sm{padding:4px 12px;font-size:12px}.btn.block{width:100%}"
    ".rtu-toolbar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:14px}"
    ".rtu-empty{margin-top:12px;color:var(--muted);font-size:13px}"
    ".rtu-dev .rtu-actions{margin-top:10px}"
    "input[type=file]{display:block;width:100%;padding:0;background:var(--card);color:var(--body);"
    "border:1px solid var(--line);border-radius:6px;font:inherit;font-size:13px;cursor:pointer;"
    "transition:border-color var(--t),box-shadow var(--t)}"
    "input[type=file]:hover{border-color:var(--muted)}"
    "input[type=file]:focus{outline:0;border-color:var(--acc);box-shadow:0 0 0 3px #0969da4d}"
    "input[type=file]::file-selector-button{margin-right:12px;border:0;border-right:1px solid var(--line);"
    "background:var(--inset);color:var(--ink);padding:6px 12px;font:inherit;font-size:13px;"
    "font-weight:500;cursor:pointer;transition:background var(--t)}"
    "input[type=file]:hover::file-selector-button{background:var(--inset-h)}"
    "input[type=file]::-webkit-file-upload-button{margin-right:12px;border:0;border-right:1px solid var(--line);"
    "background:var(--inset);color:var(--ink);padding:6px 12px;font:inherit;font-size:13px;"
    "font-weight:500;cursor:pointer;transition:background var(--t)}"
    /* Accordion from <details>: no JavaScript. The marker is one glyph rotated on
     * open, so the state change animates instead of swapping characters. */
    "details.profile{border:1px solid var(--line);border-radius:6px;margin:10px 0;background:var(--card);"
    "transition:border-color var(--t),box-shadow var(--t)}"
    "details.profile:hover{border-color:var(--muted)}"
    "details.profile[open]{box-shadow:0 1px 3px #1f23280f}"
    "details.profile>summary{display:flex;align-items:center;flex-wrap:wrap;gap:8px;padding:12px 16px;"
    "border-radius:6px;font-size:14px;font-weight:600;cursor:pointer;list-style:none;"
    "transition:background var(--t)}"
    "details.profile>summary:hover{background:var(--inset)}"
    "details.profile>summary:active{background:var(--inset-a)}"
    "details.profile>summary::-webkit-details-marker{display:none}"
    "details.profile>summary::before{content:'\\25B8';display:inline-block;color:var(--muted);"
    "font-size:11px;transition:transform var(--t)}"
    "details.profile[open]>summary::before{transform:rotate(90deg)}"
    "details.profile[open]>summary{border-bottom:1px solid var(--line-s);border-radius:6px 6px 0 0}"
    ".pbody{padding:16px}"
    ".slot{border:1px solid var(--line-s);border-radius:6px;padding:14px;margin:10px 0;"
    "background:var(--inset);transition:border-color var(--t)}"
    ".slot:hover{border-color:var(--line)}"
    ".st{margin:6px 0;font-size:12px;font-weight:600;color:var(--body);overflow-wrap:anywhere;"
    "font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;transition:color var(--t)}"
    ".st.ok{color:var(--pri)}"
    ".st.bad{color:var(--dan)}"
    ".line{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}"
    /* Radio/checkbox rows: the control sits inline with its own label text, so it
     * must not take the .input treatment (display:block + width:100%) that would
     * stretch a 13px radio across the cell and push the words to the next line. */
    ".pick{display:flex;align-items:center;gap:8px;font-weight:500;cursor:pointer}"
    ".pick input{width:auto;margin:0;flex:none}"
    ".err{background:var(--dan-sub);color:var(--dan);border:1px solid var(--dan-bd);border-radius:6px;"
    "padding:12px;font-size:13px;margin:10px 0}"
    "@media(min-width:640px){body{padding:32px}.card{padding:24px}h1{font-size:26px}"
    ".input{font-size:14px}.row{grid-template-columns:1fr 1fr}.btn.block{width:auto}}"
    /* Respect the OS setting: no travel, no fades. */
    "@media(prefers-reduced-motion:reduce){*{transition:none!important}"
    ".btn:active{transform:none}}";

/*
 * Calibration-only CSS, gated with the section it styles so a production build
 * carries no rule that matches nothing. Injected right after HTML_STYLE inside
 * the same <style> block.
 */
#if CONFIG_APP_WEB_CALIB_ENABLE
static const char *HTML_STYLE_CALIB =
    /* Space the calib submit away from the true-value field above it. */
    "#calib form .btn{margin-top:18px}";
#endif

/*
 * Keeps a file upload or delete from reloading the page and wiping fields the
 * operator typed but has not saved. Both go out as fetch(&api=1) and the reply
 * (that slot's new status line) is written into its .st paragraph. The forms keep
 * their action attribute, so a browser without scripting still works.
 *
 * The delete button is shown or hidden here too: without a reload the server
 * cannot do it, and a slot that just received its first file must offer delete.
 */
/*
 * Cert upload uses multipart; calib uses urlencoded.
 * Calib UI also: hide phase B for voltage when active profile is 3P3W, and
 * refresh Active profile after a successful calibrate so mode/freq/PGA stay true.
 *
 * The calib-only helpers live in their own constant, gated by
 * CONFIG_APP_WEB_CALIB_ENABLE. The shared submit delegate in HTML_SCRIPT below
 * still carries `data-calib` branches, but they only execute for a form that
 * sets data-calib — and with the Calibration section compiled out no such form
 * is ever rendered, so calibRefreshProfile()/calibApplyPhaseUi() are never
 * called and their absence is harmless (no ReferenceError).
 */
#if CONFIG_APP_WEB_CALIB_ENABLE
static const char *HTML_SCRIPT_CALIB =
    "function calibIs3w(){"
    "var p=document.getElementById('calib-profile');"
    "return !!(p&&p.textContent.indexOf('3P3W')>=0);}"
    "function calibApplyPhaseUi(){"
    "var f=document.getElementById('calib-field');"
    "var ph=document.getElementById('calib-phase');"
    "var b=document.getElementById('calib-phase-b');"
    "if(!f||!ph||!b)return;"
    "var hide=calibIs3w()&&(f.value==='V'||f.value==='v'||f.value==='U'||f.value==='u');"
    "b.hidden=hide;b.disabled=hide;"
    "if(hide&&ph.value==='B')ph.value='A';}"
    "function calibRefreshProfile(){"
    "var p=document.getElementById('calib-profile');if(!p)return;"
    "fetch('/api/calib/profile').then(function(r){return r.text().then(function(t){"
    "if(r.ok){p.textContent=t;calibApplyPhaseUi();}"
    "});}).catch(function(){});}"
    "document.addEventListener('DOMContentLoaded',function(){"
    "var f=document.getElementById('calib-field');"
    "if(f)f.addEventListener('change',calibApplyPhaseUi);"
    "calibApplyPhaseUi();"
    "});";
#endif

static const char *HTML_SCRIPT =
    "document.addEventListener('submit',function(e){"
    "var f=e.target;if(!f.dataset.cert&&!f.dataset.calib)return;e.preventDefault();"
    "var s=document.getElementById(f.dataset.st||'')||f.parentNode.querySelector('.st');"
    "var d=f.querySelector('[data-del]');"
    "if(s){s.className='st';s.textContent=f.dataset.calib?'Working...':'Uploading...';}"
    "var opt={method:'POST'};"
    "if(f.dataset.cert){opt.body=new FormData(f);}"
    "else{opt.headers={'Content-Type':'application/x-www-form-urlencoded'};"
    "opt.body=new URLSearchParams(new FormData(f));}"
    "fetch(f.action+(f.action.indexOf('?')>=0?'&':'?')+'api=1',opt)"
    ".then(function(r){return r.text().then(function(t){"
    /* OK → green (.st.ok); fail → red (.st.bad). Neutral .st while pending. */
    "if(s){s.className=r.ok?'st ok':'st bad';s.textContent=r.ok?t:"
    "(f.dataset.calib?t||'Calibration failed. Check the reference value.':'Upload failed. PEM file must be under 8192 bytes.');}"
    "if(f.dataset.calib)calibRefreshProfile();"
    "if(r.ok&&!f.dataset.keep)f.reset();if(r.ok&&d)d.hidden=false;"
    "if(f.dataset.calib)calibApplyPhaseUi();});})"
    ".catch(function(){if(s){s.className='st bad';s.textContent='Lost connection to the device.';}});"
    "});"
    /* MQTT cert slots follow the Connection security dropdown: hidden for "off",
     * CA only for "ca", all three for "mutual". Runs on load (the server already
     * pre-hides the right blocks; this keeps them in sync) and on every change. */
    "function mqttApplyCertUi(){"
    "var sel=document.querySelector('select[name=\"mqtt_tls\"]');"
    "var all=document.getElementById('mqtt-certs');"
    "var mut=document.getElementById('mqtt-certs-mutual');"
    "if(!sel||!all)return;"
    "var v=sel.value;"
    "all.hidden=(v==='off');"
    "if(mut)mut.hidden=(v!=='mutual');}"
    "document.addEventListener('DOMContentLoaded',function(){"
    "mqttApplyCertUi();"
    "var sel=document.querySelector('select[name=\"mqtt_tls\"]');"
    "if(sel)sel.addEventListener('change',mqttApplyCertUi);"
    "});"
    "document.addEventListener('click',function(e){"
    "var b=e.target.closest('[data-del]');if(!b)return;e.preventDefault();"
    "if(!confirm('Delete this file from the device?'))return;"
    "var s=b.closest('form').parentNode.querySelector('.st');s.className='st';s.textContent='Deleting...';"
    "fetch(b.dataset.del,{method:'POST'})"
    ".then(function(r){return r.text().then(function(t){"
    "s.className=r.ok?'st ok':'st bad';s.textContent=r.ok?t:'Delete failed.';"
    "if(r.ok)b.hidden=true;});})"
    ".catch(function(){s.className='st bad';s.textContent='Lost connection to the device.';});"
    "});"
    /* RTU master: dynamic device cards (portal = settings only; Active is LCD). */
    "function rtuList(){return document.getElementById('rtu-list');}"
    "function rtuEmpty(){return document.getElementById('rtu-empty');}"
    "function rtuUsedCount(){return rtuList()?rtuList().querySelectorAll('.rtu-dev').length:0;}"
    "function rtuRefreshEmpty(){var e=rtuEmpty();if(!e)return;e.hidden=rtuUsedCount()>0;}"
    "function rtuNextSlot(){"
    "for(var i=0;i<8;i++){if(!document.getElementById('rtu-dev-'+i))return i;}"
    "return -1;}"
    "function rtuField(label,node){"
    "var d=document.createElement('div');d.className='field';"
    "var l=document.createElement('label');l.textContent=label;d.appendChild(l);d.appendChild(node);return d;}"
    "function rtuAddDevice(pre){"
    "pre=pre||{};var list=rtuList();if(!list)return;"
    "var i=('slot' in pre)?pre.slot:rtuNextSlot();"
    "if(i<0){alert('Maximum 8 devices on this bus.');return;}"
    "if(document.getElementById('rtu-dev-'+i))return;"
    "var name=pre.name||('M'+i);"
    "var id=pre.id||String(i+1);"
    "var type=pre.type||'pm710';"
    "var el=document.createElement('details');"
    "el.className='profile rtu-dev';el.id='rtu-dev-'+i;"
    "if(pre.open!==false)el.open=true;"
    "var sum=document.createElement('summary');"
    "sum.textContent='Device '+i+(name?(' - '+name):'');"
    "el.appendChild(sum);"
    "var hid=document.createElement('input');"
    "hid.type='hidden';hid.name='mb'+i+'_used';hid.value='1';hid.setAttribute('form','cfg');"
    "el.appendChild(hid);"
    "var row=document.createElement('div');row.className='row';"
    "var sel=document.createElement('select');"
    "sel.className='input';sel.name='mb'+i+'_type';sel.setAttribute('form','cfg');"
    "var o1=document.createElement('option');o1.value='pm710';o1.textContent='Schneider PM710';"
    "if(type==='pm710')o1.selected=true;sel.appendChild(o1);"
    "var o2=document.createElement('option');o2.value='em07k';o2.textContent='TENSE EM-07K';"
    "if(type==='em07k')o2.selected=true;sel.appendChild(o2);"
    "row.appendChild(rtuField('Type',sel));"
    "var idIn=document.createElement('input');"
    "idIn.className='input';idIn.name='mb'+i+'_id';idIn.value=id;idIn.setAttribute('form','cfg');"
    "row.appendChild(rtuField('Slave ID (1-247)',idIn));"
    "var nameIn=document.createElement('input');"
    "nameIn.className='input';nameIn.name='mb'+i+'_name';nameIn.value=name;"
    "nameIn.maxLength=15;nameIn.setAttribute('form','cfg');"
    "row.appendChild(rtuField('Name',nameIn));"
    "el.appendChild(row);"
    "var act=document.createElement('div');act.className='rtu-actions';"
    "var rm=document.createElement('button');"
    "rm.type='button';rm.className='btn alt sm';rm.textContent='Remove device';"
    "rm.setAttribute('data-rtu-del',String(i));"
    "act.appendChild(rm);el.appendChild(act);"
    "nameIn.addEventListener('input',function(){"
    "sum.textContent='Device '+i+(this.value?(' - '+this.value):'');"
    "});"
    "list.appendChild(el);rtuRefreshEmpty();"
    "}"
    "document.addEventListener('DOMContentLoaded',function(){"
    "var add=document.getElementById('rtu-add');"
    "if(add)add.addEventListener('click',function(e){e.preventDefault();rtuAddDevice({open:true});});"
    "var list=rtuList();"
    "if(list)list.addEventListener('click',function(e){"
    "var b=e.target.closest('[data-rtu-del]');if(!b)return;e.preventDefault();"
    "var id=b.getAttribute('data-rtu-del');"
    "var el=document.getElementById('rtu-dev-'+id);if(el)el.remove();"
    "rtuRefreshEmpty();"
    "});"
    "var seed=document.getElementById('rtu-seed');"
    "if(seed&&seed.textContent){"
    "try{var arr=JSON.parse(seed.textContent);"
    "for(var k=0;k<arr.length;k++){arr[k].open=false;rtuAddDevice(arr[k]);}"
    "}catch(err){}"
    "}"
    "rtuRefreshEmpty();"
    "});";

static void send_redirect(httpd_req_t *req, const char *location)
{
    static const char login_page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=/login\"></head>"
        "<body><a href=\"/login\">Open login</a></body></html>";
    static const char root_page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=/\"></head>"
        "<body><a href=\"/\">Open config portal</a></body></html>";

    httpd_resp_set_type(req, "text/html");
    if (location != NULL && strcmp(location, "/login") == 0) {
        httpd_resp_send(req, login_page, sizeof(login_page) - 1);
    } else {
        httpd_resp_send(req, root_page, sizeof(root_page) - 1);
    }
}

static bool request_has_valid_session(httpd_req_t *req)
{
#if CONFIG_APP_CONSOLE_AUTH_ENABLE
    if (s_session_token[0] == '\0') {
        return false;
    }

    char cookie[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    const char *needle = "wp_session=";
    const char *p = strstr(cookie, needle);
    if (p == NULL) {
        return false;
    }
    p += strlen(needle);
    return strncmp(p, s_session_token, strlen(s_session_token)) == 0;
#else
    return true;
#endif
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r != '\0'; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w++ = (char)((hexval(r[1]) << 4) | hexval(r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool form_get_value(char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    char *p = body;
    while (p != NULL && *p != '\0') {
        char *next = strchr(p, '&');
        if (next != NULL) {
            *next = '\0';
        }
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            strlcpy(out, p + key_len + 1, out_len);
            url_decode(out);
            if (next != NULL) {
                *next = '&';
            }
            return true;
        }
        if (next == NULL) {
            break;
        }
        *next = '&';
        p = next + 1;
    }
    return false;
}

static char *read_form_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) {
        return NULL;
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }

    int total = 0;
    while (total < req->content_len) {
        int got = httpd_req_recv(req, body + total, req->content_len - total);
        if (got <= 0) {
            free(body);
            return NULL;
        }
        total += got;
    }
    body[total] = '\0';
    return body;
}

static bool form_get_u32(char *body, const char *key, uint32_t *out)
{
    char tmp[32];
    if (!form_get_value(body, key, tmp, sizeof(tmp)) || tmp[0] == '\0') {
        return false;
    }
    *out = (uint32_t)strtoul(tmp, NULL, 10);
    return true;
}

static esp_err_t send_page_start(httpd_req_t *req, const char *title)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    httpd_resp_sendstr_chunk(req, "<title>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</title><style>");
    httpd_resp_sendstr_chunk(req, HTML_STYLE);
#if CONFIG_APP_WEB_CALIB_ENABLE
    httpd_resp_sendstr_chunk(req, HTML_STYLE_CALIB);
#endif
    return httpd_resp_sendstr_chunk(req, "</style></head><body><main class=\"card\">");
}

static esp_err_t send_page_end(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, "</main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t require_auth_or_redirect(httpd_req_t *req)
{
    if (!request_has_valid_session(req)) {
        send_redirect(req, "/login");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_escaped(httpd_req_t *req, const char *s)
{
    for (const char *p = s; p != NULL && *p != '\0'; p++) {
        switch (*p) {
        case '&': httpd_resp_sendstr_chunk(req, "&amp;"); break;
        case '<': httpd_resp_sendstr_chunk(req, "&lt;"); break;
        case '>': httpd_resp_sendstr_chunk(req, "&gt;"); break;
        case '"': httpd_resp_sendstr_chunk(req, "&quot;"); break;
        default: {
            char c[2] = { *p, '\0' };
            httpd_resp_sendstr_chunk(req, c);
            break;
        }
        }
    }
    return ESP_OK;
}

/* Every settings field belongs to this one form, so one button saves the page.
 * The fields are not nested inside it — certificate uploads are multipart forms
 * of their own and HTML forbids nested forms — they join it by the HTML5 "form"
 * attribute, which is what lets a server's fields and its file pickers share a
 * block. */
#define CFG_FORM_ID "cfg"

/*
 * A field is label + control inside one .field wrapper, so the grid places the
 * pair as a single cell. Emitting the label and the control as grid siblings is
 * what broke the desktop layout: at two columns each one claimed its own cell and
 * labels ended up over the wrong input.
 *
 * `wide` spans both columns — for the long labels that would otherwise wrap into
 * two lines and knock the row out of alignment.
 */
static esp_err_t send_input_ex(httpd_req_t *req, const char *label, const char *name,
                               const char *value, bool wide)
{
    httpd_resp_sendstr_chunk(req, wide ? "<div class=\"field wide\"><label>" : "<div class=\"field\"><label>");
    httpd_resp_sendstr_chunk(req, label);
    httpd_resp_sendstr_chunk(req, "</label><input class=\"input\" form=\"" CFG_FORM_ID "\" name=\"");
    httpd_resp_sendstr_chunk(req, name);
    httpd_resp_sendstr_chunk(req, "\" value=\"");
    send_escaped(req, value != NULL ? value : "");
    return httpd_resp_sendstr_chunk(req, "\"></div>");
}

static esp_err_t send_input_with_hint(httpd_req_t *req, const char *label, const char *hint,
                                      const char *name, const char *value, bool wide)
{
    httpd_resp_sendstr_chunk(req, wide ? "<div class=\"field wide\"><label>" : "<div class=\"field\"><label>");
    httpd_resp_sendstr_chunk(req, label);
    if (hint != NULL && hint[0] != '\0') {
        httpd_resp_sendstr_chunk(req, "<span class=\"hint\">");
        httpd_resp_sendstr_chunk(req, hint);
        httpd_resp_sendstr_chunk(req, "</span>");
    }
    httpd_resp_sendstr_chunk(req, "</label><input class=\"input\" form=\"" CFG_FORM_ID "\" name=\"");
    httpd_resp_sendstr_chunk(req, name);
    httpd_resp_sendstr_chunk(req, "\" value=\"");
    send_escaped(req, value != NULL ? value : "");
    return httpd_resp_sendstr_chunk(req, "\"></div>");
}

static esp_err_t send_input(httpd_req_t *req, const char *label, const char *name, const char *value)
{
    return send_input_ex(req, label, name, value, false);
}

/*
 * A password field always renders empty — a stored secret is never sent back to
 * the browser. Whether one exists is shown in the placeholder instead: greyed
 * hint text inside the empty box, which is not part of the submitted value, so
 * leaving the field alone still means "keep the current password". This replaces
 * the separate read-only "password stored" box that used to sit beside it.
 */
static esp_err_t send_secret_input(httpd_req_t *req, const char *label, const char *name,
                                   bool has_value, bool wide)
{
    httpd_resp_sendstr_chunk(req, wide ? "<div class=\"field wide\"><label>" : "<div class=\"field\"><label>");
    httpd_resp_sendstr_chunk(req, label);
    httpd_resp_sendstr_chunk(req, "</label><input class=\"input\" type=\"password\" "
                                  "autocomplete=\"new-password\" form=\"" CFG_FORM_ID "\" name=\"");
    httpd_resp_sendstr_chunk(req, name);
    httpd_resp_sendstr_chunk(req, "\" placeholder=\"");
    httpd_resp_sendstr_chunk(req, has_value ? "Password set — leave blank to keep it"
                                            : "No password set");
    return httpd_resp_sendstr_chunk(req, "\"></div>");
}

static void send_select_start(httpd_req_t *req, const char *label, const char *name, bool wide)
{
    httpd_resp_sendstr_chunk(req, wide ? "<div class=\"field wide\"><label>" : "<div class=\"field\"><label>");
    httpd_resp_sendstr_chunk(req, label);
    httpd_resp_sendstr_chunk(req, "</label><select class=\"input\" form=\"" CFG_FORM_ID "\" name=\"");
    httpd_resp_sendstr_chunk(req, name);
    httpd_resp_sendstr_chunk(req, "\">");
}

static void send_option(httpd_req_t *req, const char *value, const char *text, bool selected)
{
    httpd_resp_sendstr_chunk(req, "<option value=\"");
    httpd_resp_sendstr_chunk(req, value);
    httpd_resp_sendstr_chunk(req, selected ? "\" selected>" : "\">");
    httpd_resp_sendstr_chunk(req, text);
    httpd_resp_sendstr_chunk(req, "</option>");
}

/* Closes the select and its .field wrapper. Extra markup for the same cell (the
 * insecure-mode warning) goes between this and the select via a separate chunk. */
static void send_select_end(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, "</select>");
}

static void send_field_end(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, "</div>");
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(900));
    ESP_LOGW(TAG, "rebooting to apply web portal config");
    esp_restart();
}

/* Three choices, not four: MQTT_TLS_INSECURE skips server verification and is a
 * console-only bring-up mode, so it is never selectable here. */
static bool tls_mode_from_form(const char *s, mqtt_tls_mode_t *out)
{
    if (strcmp(s, "off") == 0) { *out = MQTT_TLS_DISABLE; return true; }
    if (strcmp(s, "ca") == 0) { *out = MQTT_TLS_CA_ONLY; return true; }
    if (strcmp(s, "mutual") == 0) { *out = MQTT_TLS_MUTUAL; return true; }
    return false;
}

/*
 * Bind the broker's cert paths to the certificate store, like the console's
 * `mqtt-cfg set --tls`: the device has one broker and it owns store index 0, so
 * it uses ca/cert/key at that index. Paths only, never PEM
 * content. An empty ca_path in CA_ONLY is meaningful — the runtime then verifies
 * against the certificate bundle built into the image, which is what a broker with
 * a public CA needs.
 */
static void profile_bind_cert_paths(config_mqtt_profile_t *p)
{
    char path[CERT_STORE_PATH_MAX];
    const int profile = 0;   /* the single broker's certificate-store index */

    p->ca_path[0] = '\0';
    p->cert_path[0] = '\0';
    p->key_path[0] = '\0';

    if (p->tls_mode == MQTT_TLS_CA_ONLY || p->tls_mode == MQTT_TLS_MUTUAL) {
        cert_slot_info_t info = { 0 };
        bool have_ca = (cert_store_stat(profile, CERT_SLOT_CA, &info) == ESP_OK) && info.present;
        if (have_ca || p->tls_mode == MQTT_TLS_MUTUAL) {
            strlcpy(p->ca_path, cert_store_slot_path(profile, CERT_SLOT_CA, path, sizeof(path)),
                    sizeof(p->ca_path));
        }
    }
    if (p->tls_mode == MQTT_TLS_MUTUAL) {
        strlcpy(p->cert_path, cert_store_slot_path(profile, CERT_SLOT_CERT, path, sizeof(path)),
                sizeof(p->cert_path));
        strlcpy(p->key_path, cert_store_slot_path(profile, CERT_SLOT_KEY, path, sizeof(path)),
                sizeof(p->key_path));
    }
}

/*
 * Saves every text setting on the page, then reboots to apply. The reboot is not
 * optional: almost nothing the page changes is applied live, so a save without a
 * restart only looked like it worked.
 *
 * Everything goes through the Configuration Manager — update() replaces the RAM
 * snapshot, save() persists it and merges into the legacy config_store domains.
 * Certificates are not part of this form; only the *path* of an uploaded file
 * reaches the snapshot.
 */
static esp_err_t save_all_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;
    char *body = read_form_body(req);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    /* config_manager_t is ~1.2 KB (one MQTT broker); keep it off the httpd task
     * stack. */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    esp_err_t ret = config_manager_get(cfg);
    if (ret != ESP_OK) {
        free(cfg);
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "configuration unavailable");
        return ESP_OK;
    }

    /* Device identity + WiFi. A blank password field means "keep the stored
     * one", so a saved secret is never wiped by a save that did not touch it. */
    form_get_value(body, "device_name", cfg->device_name, sizeof(cfg->device_name));
    form_get_value(body, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    char secret[CONFIG_MANAGER_MQTT_PASS_LEN];
    if (form_get_value(body, "wifi_pass", secret, sizeof(secret)) && secret[0] != '\0') {
        strlcpy(cfg->wifi_pass, secret, sizeof(cfg->wifi_pass));
    }
    /* SoftAP portal credentials. Blank password keeps the stored one (same as WiFi). */
    form_get_value(body, "ap_ssid", cfg->ap_ssid, sizeof(cfg->ap_ssid));
    if (form_get_value(body, "ap_pass", secret, sizeof(secret)) && secret[0] != '\0') {
        strlcpy(cfg->ap_pass, secret, sizeof(cfg->ap_pass));
    }

    /* MQTT: the single broker, plus the publish interval.
     *
     * No enable parse: mqtt.enable is owned by the LCD (Settings > MQTT) and the
     * page sends no key for it, so the value already in the snapshot is written
     * back unchanged by update() below.
     *
     * The period is submitted in whole seconds and stored in milliseconds;
     * anything outside 1..60 is ignored so a bad edit cannot make the whole
     * save fail — the stored value simply survives the attempt. */
    uint32_t v = 0;
    if (form_get_u32(body, "publish_period_s", &v) &&
        v >= CONFIG_MANAGER_MQTT_PERIOD_MIN_MS / 1000U &&
        v <= CONFIG_MANAGER_MQTT_PERIOD_MAX_MS / 1000U) {
        cfg->mqtt_publish_ms = v * 1000U;
    }

    {
        config_mqtt_profile_t *p = &cfg->mqtt;

        form_get_value(body, "mqtt_name", p->name, sizeof(p->name));
        form_get_value(body, "mqtt_uri", p->broker, sizeof(p->broker));
        if (form_get_u32(body, "mqtt_port", &v) && v > 0 && v <= UINT16_MAX) {
            p->port = (uint16_t)v;
        }
        if (form_get_u32(body, "mqtt_keepalive", &v) && v > 0 && v <= UINT16_MAX) {
            p->keepalive_s = (uint16_t)v;
        }
        form_get_value(body, "mqtt_user", p->username, sizeof(p->username));
        if (form_get_value(body, "mqtt_pass", secret, sizeof(secret)) && secret[0] != '\0') {
            strlcpy(p->password, secret, sizeof(p->password));
        }

        char mode_str[16];
        mqtt_tls_mode_t mode;
        if (form_get_value(body, "mqtt_tls", mode_str, sizeof(mode_str)) &&
            tls_mode_from_form(mode_str, &mode)) {
            p->tls_mode = mode;
            profile_bind_cert_paths(p);
        }
    }

    /* RTU master bus params + device table (settings only).
     * mb_enabled / per-slot enabled are owned by LCD Active — never written here. */
    {
        char tmpv[24];
        if (form_get_u32(body, "mb_baud", &v) && v <= 4U) {
            cfg->mb_baud_code = (uint8_t)v;
        }
        if (form_get_u32(body, "mb_parity", &v) && v <= 2U) {
            cfg->mb_parity_code = (uint8_t)v;
        }
        if (form_get_u32(body, "mb_period", &v) && v >= 200U && v <= 600000U) {
            cfg->mb_poll_period_ms = v;
        }

        /* Rebuild slots from submitted cards only. Preserve enable flags when
         * the same slot index is still present so LCD Active state survives. */
        config_mb_slot_t old_slots[CONFIG_MANAGER_MB_SLOT_COUNT];
        memcpy(old_slots, cfg->mb_slots, sizeof(old_slots));
        memset(cfg->mb_slots, 0, sizeof(cfg->mb_slots));

        for (int i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
            config_mb_slot_t *s = &cfg->mb_slots[i];
            char key[24];
            char type_s[12];
            char name_s[CONFIG_MANAGER_MB_NAME_LEN];

            snprintf(key, sizeof(key), "mb%d_used", i);
            bool used = form_get_value(body, key, tmpv, sizeof(tmpv)) &&
                        (strcmp(tmpv, "1") == 0 || strcmp(tmpv, "on") == 0);
            if (!used) {
                continue;
            }

            s->used = true;
            /* Keep prior enable if slot already existed; new slot defaults on. */
            if (old_slots[i].used) {
                s->enabled = old_slots[i].enabled;
            } else {
                s->enabled = true;
            }

            snprintf(key, sizeof(key), "mb%d_type", i);
            type_s[0] = '\0';
            form_get_value(body, key, type_s, sizeof(type_s));
            if (strcmp(type_s, "em07k") == 0 || strcmp(type_s, "1") == 0) {
                s->type = 1; /* METER_DEV_EM07K */
            } else {
                s->type = 0; /* METER_DEV_PM710 */
            }

            snprintf(key, sizeof(key), "mb%d_id", i);
            if (form_get_u32(body, key, &v) && v >= 1U && v <= 247U) {
                s->slave_id = (uint8_t)v;
            } else {
                s->slave_id = 1;
            }

            snprintf(key, sizeof(key), "mb%d_name", i);
            name_s[0] = '\0';
            form_get_value(body, key, name_s, sizeof(name_s));
            if (name_s[0] != '\0') {
                strlcpy(s->name, name_s, sizeof(s->name));
            } else {
                snprintf(s->name, sizeof(s->name), "M%d", i);
            }
        }
    }

    free(body);
    ret = config_manager_update(cfg);
    free(cfg);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "update config failed");
        return ESP_OK;
    }

    ret = config_manager_save();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config_manager_save failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "persist config failed");
        return ESP_OK;
    }

    /* Calibration has its own NVS blob. Persist any gains applied from the
     * Calibration section so one "Save and restart" covers config + meter.
     * Only meaningful when that section is compiled in; LCD/console calibration
     * persists itself at the point of change, so there is nothing to flush here
     * otherwise (and the symbol is not declared with the gate off). */
#if CONFIG_APP_WEB_CALIB_ENABLE
    esp_err_t calib_ret = energy_meter_save_calibration();
    if (calib_ret != ESP_OK) {
        ESP_LOGW(TAG, "energy_meter_save_calibration on portal save: %s",
                 esp_err_to_name(calib_ret));
    }
#endif

    /* The page goes out before the reboot task starts, so the browser has the
     * reply in hand by the time the device drops off the network. */
    send_page_start(req, "Restarting");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Saved</span>"
        "<h1>Restarting...</h1>"
        "<p>Settings are saved and the device is restarting to apply them. "
        "Wait a few seconds, then reconnect.</p>");
    send_page_end(req);

    if (xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create reboot task failed");
    }
    return ESP_OK;
}

/*
 * MQTT TLS certificate upload / delete / status (Feature 13B).
 *
 * Target: ?slot=ca|cert|key&profile=0 — the device's single broker owns the one
 * certificate triple. The profile parameter is kept so cert_store needs no
 * knowledge of how many brokers the configuration layer supports; 0 is the only
 * valid value.
 * Body: raw PEM for curl, or multipart/form-data for the portal's file pickers.
 * The extension is never inspected; cert_store_write() checks the PEM envelope,
 * so .pem/.crt/.cer all work and DER is rejected.
 *
 * SECURITY: write-only. No handler returns a stored PEM; status reports presence,
 * size and a short SHA-256 fingerprint only.
 */
static bool cert_target_from_query(httpd_req_t *req, int *profile, cert_slot_t *slot)
{
    char query[80];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[16];
    if (httpd_query_key_value(query, "slot", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    if (!cert_store_slot_from_name(value, slot)) {
        return false;
    }

    /* Missing profile means 0: the portal's forms always send it, and a
     * single-broker curl setup uses profile 0. An out-of-range value is rejected
     * rather than clamped, so a typo fails loudly instead of writing somewhere
     * unexpected. */
    *profile = 0;
    if (httpd_query_key_value(query, "profile", value, sizeof(value)) == ESP_OK) {
        char *end = NULL;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0' || v < 0 || v >= CERT_STORE_PROFILE_COUNT) {
            return false;
        }
        *profile = (int)v;
    }
    return true;
}

/* Presence test for a query key, used to tell a click in the portal apart from a
 * curl call so each gets a reply it can use. */
static bool query_flag_set(httpd_req_t *req, const char *key)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[8];
    return httpd_query_key_value(query, key, value, sizeof(value)) == ESP_OK;
}

/* Plain text rather than the HTML card pages: these endpoints are driven by curl
 * or fetch(), not by a browser form. */
static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

/* One line describing a slot: rendered into the page, and returned after an
 * upload or delete so the script can refresh that line in place. Presence, size
 * and fingerprint only, never content. Returns presence so the caller can decide
 * whether a delete button belongs there, without stat'ing (and re-hashing) twice. */
static bool cert_status_text(int profile, cert_slot_t slot, char *out, size_t out_len)
{
    cert_slot_info_t info = { 0 };
    if (cert_store_stat(profile, slot, &info) == ESP_OK && info.present) {
        snprintf(out, out_len, "Loaded · %u bytes · sha256 %s",
                 (unsigned)info.size, info.fingerprint);
        return true;
    }
    snprintf(out, out_len, "Not loaded");
    return false;
}

/* True when the request came from a browser form rather than curl, so the reply
 * should be a page with a link back instead of one line of text. */
static bool request_is_form_post(httpd_req_t *req)
{
    char type[80];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
        return false;
    }
    return strncasecmp(type, "multipart/form-data", strlen("multipart/form-data")) == 0;
}

/*
 * Narrow the buffer to the file payload of a multipart/form-data body.
 *
 * Handles only what the portal's own form sends: one file part, payload starting
 * after the blank line that ends the part headers and ending at the next
 * boundary. Anything malformed fails rather than being guessed at, so it cannot
 * reach the filesystem as a PEM. Adjusts *body / *len in place; the caller still
 * owns the allocation.
 */
static bool multipart_extract_file(httpd_req_t *req, char **body, size_t *len)
{
    char type[128];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
        return false;
    }

    const char *b = strstr(type, "boundary=");
    if (b == NULL) {
        return false;
    }
    b += strlen("boundary=");

    /* RFC 2046 allows the boundary to be quoted. */
    char boundary[80];
    size_t n = 0;
    if (*b == '"') {
        b++;
        while (*b != '\0' && *b != '"' && n < sizeof(boundary) - 1) boundary[n++] = *b++;
    } else {
        while (*b != '\0' && *b != ';' && *b != ' ' && n < sizeof(boundary) - 1) boundary[n++] = *b++;
    }
    boundary[n] = '\0';
    if (n == 0) {
        return false;
    }

    /* The part headers end at the first blank line; the payload starts after it. */
    char *start = NULL;
    for (size_t i = 0; i + 4 <= *len; i++) {
        if (memcmp(*body + i, "\r\n\r\n", 4) == 0) {
            start = *body + i + 4;
            break;
        }
    }
    if (start == NULL) {
        return false;
    }

    /* The payload ends at the CRLF that introduces the next boundary line. */
    size_t remaining = *len - (size_t)(start - *body);
    char needle[84];
    int needle_len = snprintf(needle, sizeof(needle), "\r\n--%s", boundary);
    if (needle_len <= 0 || (size_t)needle_len >= sizeof(needle)) {
        return false;
    }

    char *end = NULL;
    for (size_t i = 0; i + (size_t)needle_len <= remaining; i++) {
        if (memcmp(start + i, needle, (size_t)needle_len) == 0) {
            end = start + i;
            break;
        }
    }
    if (end == NULL) {
        return false;
    }

    *body = start;
    *len = (size_t)(end - start);
    return *len > 0;
}

static esp_err_t cert_upload_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;

    bool from_form = request_is_form_post(req);

    int profile = 0;
    cert_slot_t slot;
    if (!cert_target_from_query(req, &profile, &slot)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need slot=ca|cert|key and profile=0");
        return ESP_OK;
    }

    /* A multipart body carries the boundary and part headers on top of the PEM,
     * so it is allowed to be larger than the PEM limit itself. The PEM extracted
     * from it is still checked against CERT_STORE_PEM_MAX by cert_store_write(). */
    size_t limit = from_form ? CERT_STORE_PEM_MAX + 1024 : CERT_STORE_PEM_MAX;
    if (req->content_len <= 0 || (size_t)req->content_len > limit) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body or PEM over 8192 bytes");
        return ESP_OK;
    }

    /* The PEM never goes on the httpd task stack; it is heap only, and freed
     * before responding whether the write succeeded or not. */
    size_t len = (size_t)req->content_len;
    char *buf = malloc(len);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }

    size_t total = 0;
    while (total < len) {
        int got = httpd_req_recv(req, buf + total, len - total);
        if (got <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        total += (size_t)got;
    }

    /* pem/pem_len may point inside buf after this; buf stays the allocation to free. */
    char *pem = buf;
    size_t pem_len = len;
    if (from_form && !multipart_extract_file(req, &pem, &pem_len)) {
        free(buf);
        ESP_LOGE(TAG, "upload profile %d %s: could not find a file part in the form body",
                 profile, cert_store_slot_name(slot));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no file selected");
        return ESP_OK;
    }

    esp_err_t ret = cert_store_write(profile, slot, pem, pem_len);
    free(buf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "store profile %d %s certificate failed: %s",
                 profile, cert_store_slot_name(slot), esp_err_to_name(ret));
        if (from_form && !query_flag_set(req, "api")) {
            send_page_start(req, "Upload failed");
            httpd_resp_sendstr_chunk(req, "<h1>Upload failed</h1><p class=\"err\">The file was not saved. "
                                          "Check that it is a PEM file (open it in a text editor and look "
                                          "for <code>-----BEGIN ...</code>), smaller than 8192 bytes, and "
                                          "that the certificate store is ready.</p>"
                                          "<p><a class=\"btn\" href=\"/#mqtt\">Back to settings</a></p>");
            return send_page_end(req);
        }
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Not saved: the file must be PEM and smaller than 8192 bytes");
        return ESP_OK;
    }

    char path[CERT_STORE_PATH_MAX];
    char msg[128];
    snprintf(msg, sizeof(msg), "stored profile %d %s at %s (%u bytes)\n", profile,
             cert_store_slot_name(slot), cert_store_slot_path(profile, slot, path, sizeof(path)),
             (unsigned)pem_len);
    ESP_LOGI(TAG, "web upload: %s", msg);

    /* api=1 is what the page's script adds: it wants the new status line back so it
     * can refresh that one slot without reloading and losing typed-in fields.
     * A no-script browser posts the form itself and gets the redirect. */
    if (query_flag_set(req, "api")) {
        char status[128];
        cert_status_text(profile, slot, status, sizeof(status));
        return send_text(req, status);
    }
    if (from_form) {
        send_redirect(req, "/#mqtt");
        return ESP_OK;
    }
    return send_text(req, msg);
}

static esp_err_t cert_delete_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;

    int profile = 0;
    cert_slot_t slot;
    if (!cert_target_from_query(req, &profile, &slot)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need slot=ca|cert|key and profile=0");
        return ESP_OK;
    }

    esp_err_t ret = cert_store_delete(profile, slot);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
        return ESP_OK;
    }

    char path[CERT_STORE_PATH_MAX];
    char msg[96];
    snprintf(msg, sizeof(msg), "deleted profile %d %s (%s)\n", profile,
             cert_store_slot_name(slot),
             cert_store_slot_path(profile, slot, path, sizeof(path)));
    ESP_LOGI(TAG, "web delete: %s", msg);

    /* api=1 comes from the page's script and asks for the slot's new status line;
     * ui=1 is the no-script path back to the page. curl gets the plain line. */
    if (query_flag_set(req, "api")) {
        char status[128];
        cert_status_text(profile, slot, status, sizeof(status));
        return send_text(req, status);
    }
    if (query_flag_set(req, "ui")) {
        send_redirect(req, "/#mqtt");
        return ESP_OK;
    }
    return send_text(req, msg);
}

static esp_err_t cert_status_get_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;

    if (!cert_store_ready()) {
        return send_text(req, "cert store not mounted\n");
    }

    /* All profiles are listed, not just the active one: an operator uploading for
     * profile 1 while profile 0 is live needs to see what landed. */
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    for (int prof = 0; prof < CERT_STORE_PROFILE_COUNT; prof++) {
        for (int i = 0; i < CERT_SLOT_COUNT; i++) {
            cert_slot_info_t info;
            char path[CERT_STORE_PATH_MAX];
            char line[160];
            const char *name = cert_store_slot_name((cert_slot_t)i);
            cert_store_slot_path(prof, (cert_slot_t)i, path, sizeof(path));
            if (cert_store_stat(prof, (cert_slot_t)i, &info) != ESP_OK) {
                snprintf(line, sizeof(line), "profile %d %-4s error\n", prof, name);
            } else if (info.present) {
                snprintf(line, sizeof(line), "profile %d %-4s %s present size=%u sha256=%s\n",
                         prof, name, path, (unsigned)info.size, info.fingerprint);
            } else {
                snprintf(line, sizeof(line), "profile %d %-4s %s absent\n", prof, name, path);
            }
            httpd_resp_sendstr_chunk(req, line);
        }
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

/*
 * Portal calibration: enter a true V/I reference → auto-calibrate chip now.
 * Persistence is via the page "Save and restart" (also writes meter NVS).
 * Live readings stay on the LCD; file backup/restore is LCD + SD only.
 *
 * Active profile shows wiring mode only (freq/PGA are Kconfig/console, not
 * end-user portal knobs). JS still detects "3P3W" in this string to hide VB.
 * Refresh after each calibrate so the operator does not calib a stale mode.
 *
 * Whole section is gated by CONFIG_APP_WEB_CALIB_ENABLE (default n). Calibration
 * writes gain/offset registers on the metering chip, so it is developer-only;
 * with the gate off these handlers, the page section and the two /api/calib
 * routes are not compiled in and cannot be reached from the end-user portal.
 */
#if CONFIG_APP_WEB_CALIB_ENABLE
static void calib_format_profile(char *out, size_t out_len)
{
    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) != ESP_OK) {
        snprintf(out, out_len, "Meter not ready");
        return;
    }
    snprintf(out, out_len, "%s",
             calib.wiring_mode == ATM90E32AS_WIRING_3P3W ? "3P3W" : "3P4W");
}

static esp_err_t calib_profile_get_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;
    char line[64];
    calib_format_profile(line, sizeof(line));
    return send_text(req, line);
}

static esp_err_t calib_auto_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;

    bool api_mode = query_flag_set(req, "api");
    char *body = read_form_body(req);
    if (body == NULL) {
        if (api_mode) {
            return send_text(req, "bad form");
        }
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    char phase_s[8] = { 0 };
    char field_s[8] = { 0 };
    char value_s[24] = { 0 };
    form_get_value(body, "phase", phase_s, sizeof(phase_s));
    form_get_value(body, "field", field_s, sizeof(field_s));
    form_get_value(body, "value", value_s, sizeof(value_s));
    free(body);

    atm90e32as_phase_t phase;
    if (phase_s[0] == 'a' || phase_s[0] == 'A') {
        phase = ATM90E32AS_PHASE_A;
    } else if (phase_s[0] == 'b' || phase_s[0] == 'B') {
        phase = ATM90E32AS_PHASE_B;
    } else if (phase_s[0] == 'c' || phase_s[0] == 'C') {
        phase = ATM90E32AS_PHASE_C;
    } else {
        if (api_mode) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_text(req, "phase must be A, B or C");
        }
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "phase must be A, B or C");
        return ESP_OK;
    }

    bool current = false;
    if (field_s[0] == 'i' || field_s[0] == 'I') {
        current = true;
    } else if (field_s[0] == 'u' || field_s[0] == 'U' || field_s[0] == 'v' || field_s[0] == 'V') {
        current = false;
    } else {
        if (api_mode) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_text(req, "field must be V or I");
        }
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "field must be V or I");
        return ESP_OK;
    }

    /* Calibration requires a neutral, so it is blocked entirely in 3P3W (both V
     * and I). Phase gains are shared across wiring modes, so calibrate in 3P4W
     * and the result carries over. Mirrors the gate in the meter task. */
    {
        atm90e32as_calib_t calib;
        if (energy_meter_get_calibration(&calib) == ESP_OK &&
            calib.wiring_mode == ATM90E32AS_WIRING_3P3W) {
            const char *m = "Calibration is disabled in 3P3W. Switch to 3P4W "
                            "(developer console), calibrate, then switch back.";
            if (api_mode) {
                httpd_resp_set_status(req, "400 Bad Request");
                return send_text(req, m);
            }
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, m);
            return ESP_OK;
        }
    }

    bool offset = (strcmp(value_s, "offset") == 0 || strcmp(value_s, "OFFSET") == 0);
    float ref = 0.0f;
    if (!offset) {
        char *end = NULL;
        ref = strtof(value_s, &end);
        if (end == value_s || ref <= 0.0f) {
            if (api_mode) {
                httpd_resp_set_status(req, "400 Bad Request");
                return send_text(req, "enter a positive value, or offset");
            }
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad reference value");
            return ESP_OK;
        }
    }

    energy_meter_auto_calib_request_t request = {
        .phase = phase,
        .current = current,
        .calibrate_offset = offset,
        .source = ENERGY_METER_CALIB_REFERENCE_MANUAL,
        .manual_reference = offset ? 0.0f : ref,
        .samples = 20,
        .settle_ms = 200,
        .tolerance_percent = 0.2f,
    };
    energy_meter_auto_calib_result_t result;
    esp_err_t ret = energy_meter_auto_calibrate(&request, &result);

    char msg[220];
    if (ret == ESP_OK) {
        if (offset) {
            snprintf(msg, sizeof(msg),
                     "OK %s%c offset %d->%d. Check LCD, then Save and restart.",
                     current ? "I" : "V", 'A' + (int)phase,
                     (int)result.old_offset, (int)result.new_offset);
        } else {
            snprintf(msg, sizeof(msg),
                     "OK %s%c %.3f -> %.3f. Check LCD, then Save and restart.",
                     current ? "I" : "V", 'A' + (int)phase,
                     result.measured_before, result.measured_after);
        }
        ESP_LOGI(TAG, "web calib: %s", msg);
    } else if (ret == ESP_ERR_INVALID_SIZE && current) {
        snprintf(msg, sizeof(msg),
                 "Failed: current out of range at PGA x%d. "
                 "Use developer console to set PGA (1/2/4), then Calibrate again.",
                 1 << result.old_pga);
        ESP_LOGW(TAG, "web calib: %s", msg);
    } else if (ret == ESP_ERR_INVALID_SIZE && !current) {
        snprintf(msg, sizeof(msg),
                 "Failed: voltage gain out of range. Check divider / wiring.");
        ESP_LOGW(TAG, "web calib: %s", msg);
    } else {
        snprintf(msg, sizeof(msg), "Failed: %s%s",
                 esp_err_to_name(ret), result.rolled_back ? " (rolled back)" : "");
        ESP_LOGW(TAG, "web calib failed: %s", msg);
    }

    if (api_mode) {
        if (ret != ESP_OK) {
            httpd_resp_set_status(req, "400 Bad Request");
        }
        return send_text(req, msg);
    }
    if (ret == ESP_OK) {
        send_redirect(req, "/#calib");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    return ESP_OK;
}
#endif /* CONFIG_APP_WEB_CALIB_ENABLE */

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;

    /* No button on the page posts here — /save reboots by itself. Kept for curl
     * and scripts. Persisting first so a reboot never drops configuration that
     * only lives in the RAM snapshot; saving twice is harmless. */
    esp_err_t save_ret = config_manager_save();
    if (save_ret != ESP_OK) {
        ESP_LOGE(TAG, "config_manager_save before reboot failed: %s", esp_err_to_name(save_ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "persist config failed; not rebooting");
        return ESP_OK;
    }

    send_page_start(req, "Restarting");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Saved</span>"
        "<h1>Restarting...</h1>"
        "<p>Wait a few seconds, then reconnect.</p>");
    send_page_end(req);

    BaseType_t ok = xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create reboot task failed");
    }
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    send_redirect(req, "/");
    return ESP_OK;
}

/*
 * One file picker per certificate slot, inside the block of the server it belongs
 * to, so a certificate can be uploaded from a browser without curl. The form
 * carries both slot and profile index and posts to the same /api/cert endpoint as
 * curl. Delete is a plain button, not a second form, since forms cannot nest and
 * the page's script sends both actions; the form's action is the no-script path.
 *
 * Delete only shows when the slot holds a file — there is nothing to delete
 * otherwise. It ships with `hidden` instead of being left out entirely so the
 * page's script can reveal it after an upload; uploads do not reload the page, so
 * a button that only the server could create would stay missing until a reload.
 *
 * Renders presence, size and the short fingerprint only.
 */
static void send_cert_slot(httpd_req_t *req, int profile, cert_slot_t slot,
                           const char *title, const char *hint)
{
    char buf[224];

    /* title/hint go out directly: the Vietnamese text runs past 100 bytes in
     * UTF-8 and would risk silent truncation through buf. */
    httpd_resp_sendstr_chunk(req, "<div class=\"slot\"><h3>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</h3><p class=\"muted\">");
    httpd_resp_sendstr_chunk(req, hint);
    httpd_resp_sendstr_chunk(req, "</p><p class=\"st\">");
    bool present = cert_status_text(profile, slot, buf, sizeof(buf));
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, "</p>");

    /* enctype is what makes the browser send the file itself rather than only its
     * name; accept only filters the picker, the firmware validates the content. */
    snprintf(buf, sizeof(buf),
             "<form method=\"post\" enctype=\"multipart/form-data\" data-cert=\"1\" "
             "action=\"/api/cert?slot=%s&amp;profile=%d\">",
             cert_store_slot_name(slot), profile);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "<input type=\"file\" name=\"file\" accept=\".pem,.crt,.cer,.key,application/x-pem-file,text/plain\" required>"
        "<div class=\"line\"><button class=\"btn sm\" type=\"submit\">Upload file</button>");
    snprintf(buf, sizeof(buf),
             "<button class=\"btn alt sm\" type=\"button\"%s "
             "data-del=\"/api/cert/delete?slot=%s&amp;profile=%d&amp;api=1\">Delete file</button>",
             present ? "" : " hidden", cert_store_slot_name(slot), profile);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, "</div></form></div>");
}

/*
 * The device's ONE MQTT broker: connection fields, security choice and its
 * certificate files inline in the MQTT section — there is no broker list, no
 * add/remove, and no enable control here (the LCD's Settings > MQTT owns that).
 *
 * Emitted as plain fields, not a collapsible card: with a single broker a
 * summary heading would only add a click between the operator and the settings.
 * The certificate slots belong to cert-store index 0, the only one that exists.
 */
static void send_mqtt_broker_block(httpd_req_t *req, const config_mqtt_profile_t *p,
                                   const char *period_s)
{
    char buf[224];

    httpd_resp_sendstr_chunk(req, "<div class=\"row\">");
    /* The interval sits beside the label, not the port, so the wide server address
     * below does not split the remaining half-width fields: that leaves
     * Port|Keep-alive and Username|Password each on a full row. */
    send_input(req, "Broker name", "mqtt_name", p->name);
    send_input(req, "Publish interval (seconds)", "publish_period_s", period_s);
    send_input_ex(req, "Server address", "mqtt_uri", p->broker, true);
    snprintf(buf, sizeof(buf), "%u", (unsigned)p->port);
    send_input(req, "Port", "mqtt_port", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)p->keepalive_s);
    send_input(req, "Keep-alive (seconds)", "mqtt_keepalive", buf);
    send_input(req, "Username", "mqtt_user", p->username);
    send_secret_input(req, "Password", "mqtt_pass", p->password[0] != '\0', false);

    /* MQTT_TLS_INSECURE is intentionally not offered: it skips server
     * verification and belongs to console bring-up only. A broker already set to
     * it on the console keeps that value until the operator picks one of these. */
    send_select_start(req, "Connection security", "mqtt_tls", true);
    send_option(req, "off", "No encryption (port 1883)", p->tls_mode == MQTT_TLS_DISABLE);
    send_option(req, "ca", "TLS (port 8883) — typical", p->tls_mode == MQTT_TLS_CA_ONLY);
    send_option(req, "mutual", "TLS with device certificate", p->tls_mode == MQTT_TLS_MUTUAL);
    send_select_end(req);
    if (p->tls_mode == MQTT_TLS_INSECURE) {
        httpd_resp_sendstr_chunk(req, "<p class=\"muted\">Currently in debug mode with no server "
                                      "verification. Pick one of the options above and save to leave it.</p>");
    }
    send_field_end(req);
    httpd_resp_sendstr_chunk(req, "</div>");

    if (!cert_store_ready()) {
        httpd_resp_sendstr_chunk(req, "<div class=\"err\">The certificate store is not ready, so files "
                                      "cannot be uploaded. Check the boot log at the \"Cert Store\" "
                                      "step.</div>");
    } else {
        /* Cert slots are only relevant when TLS is on. They are wrapped in
         * #mqtt-certs so the dropdown can show/hide them without a reload.
         * Server-side they are always rendered (so no-script still works); JS
         * hides them on load when the current value is "off". */
        bool show_certs = (p->tls_mode != MQTT_TLS_DISABLE);
        httpd_resp_sendstr_chunk(req, show_certs ? "<div id=\"mqtt-certs\">" : "<div id=\"mqtt-certs\" hidden>");
        send_cert_slot(req, 0, CERT_SLOT_CA, "Server certificate (CA)",
                       "Enough for TLS mode.");
        bool show_mutual = (p->tls_mode == MQTT_TLS_MUTUAL);
        httpd_resp_sendstr_chunk(req, show_mutual ? "<div id=\"mqtt-certs-mutual\">" : "<div id=\"mqtt-certs-mutual\" hidden>");
        send_cert_slot(req, 0, CERT_SLOT_CERT, "Device certificate",
                       "Only needed when the server asks the device to present one.");
        send_cert_slot(req, 0, CERT_SLOT_KEY, "Device private key",
                       "Goes with the device certificate.");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "</div>");
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!request_has_valid_session(req)) {
        send_redirect(req, "/login");
        return ESP_OK;
    }

    /* Everything on this page now reads through the Configuration Manager,
     * including the WiFi credentials (the password only to say whether one
     * exists). config_manager_t is ~1.2 KB; keep it off the httpd task stack. */
    config_manager_t *mcfg = malloc(sizeof(*mcfg));
    if (mcfg == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    config_manager_get(mcfg);

    char tmp[96];
    send_page_start(req, "PowerMeter Setting");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Settings</span>"
        "<h1>Power Meter Setting</h1>"
        "<div class=\"nav\"><a href=\"#device\">Device &amp; network</a><a href=\"#mqtt\">MQTT</a>"
        "<a href=\"#rtu\">RTU master</a>");
#if CONFIG_APP_WEB_CALIB_ENABLE
    httpd_resp_sendstr_chunk(req, "<a href=\"#calib\">Calibration</a>");
#endif
    httpd_resp_sendstr_chunk(req, "<a href=\"#save\">Save</a></div>");

    /* One .row, auto-placed on a 1fr/1fr grid (2 columns from 640px up):
     * device name is wide so it takes a row of its own, then WiFi SSID + password
     * pair up side by side, then the portal-AP pair below them. The AP fields are
     * self-identifying by their labels, so no "Config Portal AP" subheading.
     * Both password fields are half-width, which is also what makes their muted
     * placeholder text render identically (it is generated by send_secret_input).
     * No network-mode or DHCP line: the operator cannot act on either from here.
     *
     * The section is a <details> so the operator can collapse it; it ships open
     * because device identity is the first thing to check on a fresh board. */
    httpd_resp_sendstr_chunk(req, "<details class=\"section\" id=\"device\" open>"
                                  "<summary>Device &amp; network</summary>"
                                  "<div class=\"sbody\"><div class=\"row\">");
    send_input_with_hint(req, "Device name",
                         "Used for MQTT topics and client ID (sanitized: / + # and spaces become _)",
                         "device_name", mcfg->device_name, true);
    send_input(req, "WiFi name (SSID)", "wifi_ssid", mcfg->wifi_ssid);
    send_secret_input(req, "WiFi password", "wifi_pass", strlen(mcfg->wifi_pass) > 0, false);
    send_input(req, "Portal AP SSID", "ap_ssid", mcfg->ap_ssid);
    send_secret_input(req, "Portal AP password", "ap_pass", strlen(mcfg->ap_pass) > 0, false);
    httpd_resp_sendstr_chunk(req, "</div></div></details>");

    /* MQTT: exactly one broker, configured here and switched on or off on the
     * device. The publish interval is shared with the LCD and entered in whole
     * seconds (1..60) on both frontends; the snapshot stores milliseconds.
     *
     * There is deliberately no enable control on this page. Writing it from the
     * portal would let a settings save silently turn telemetry on or off under
     * an operator who only came to change a topic, so the flag stays the LCD's.
     *
     * Collapsible like the other sections; ships open because the broker is the
     * most-edited part of this page. */
    httpd_resp_sendstr_chunk(req, "<details class=\"section\" id=\"mqtt\" open>"
                                  "<summary>MQTT</summary><div class=\"sbody\">");
    httpd_resp_sendstr_chunk(req,
        "<p class=\"muted\">The device publishes to this one broker. MQTT is turned on or "
        "off from the device LCD: Settings &#8594; MQTT &#8594; Status.</p>");

    snprintf(tmp, sizeof(tmp), "%lu",
             (unsigned long)(mcfg->mqtt_publish_ms / 1000U));
    send_mqtt_broker_block(req, &mcfg->mqtt, tmp);
    httpd_resp_sendstr_chunk(req, "</div></details>");

    /* RTU master: bus settings + dynamic device list (Add device).
     * Master/slot Active is LCD-only — portal never offers enable toggles.
     * Collapsible; ships closed because the bus is configured less often than
     * network/MQTT. */
    {
        char tmpb[24];
        unsigned used_n = 0;
        for (int i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
            if (mcfg->mb_slots[i].used) {
                used_n++;
            }
        }

        httpd_resp_sendstr_chunk(req, "<details class=\"section\" id=\"rtu\">"
                                      "<summary>RTU master</summary><div class=\"sbody\">");
        httpd_resp_sendstr_chunk(req,
            "<p class=\"muted\">Configure the master bus (downstream meters) here. Turn the "
            "master on or off from the device LCD: Settings → RTU Master → Active.<br>"
            "The device's own RTU slave address and baud are set on the LCD "
            "(Settings → RTU Slave) — they do not follow these bus settings.</p>"
            "<div class=\"row\">");

        send_select_start(req, "Master baud rate", "mb_baud", false);
        send_option(req, "0", "9600", mcfg->mb_baud_code == 0);
        send_option(req, "1", "19200", mcfg->mb_baud_code == 1);
        send_option(req, "2", "38400", mcfg->mb_baud_code == 2);
        send_option(req, "3", "57600", mcfg->mb_baud_code == 3);
        send_option(req, "4", "115200", mcfg->mb_baud_code == 4);
        send_select_end(req);
        send_field_end(req);

        send_select_start(req, "Master parity", "mb_parity", false);
        send_option(req, "0", "None", mcfg->mb_parity_code == 0);
        send_option(req, "1", "Even", mcfg->mb_parity_code == 1);
        send_option(req, "2", "Odd", mcfg->mb_parity_code == 2);
        send_select_end(req);
        send_field_end(req);

        snprintf(tmpb, sizeof(tmpb), "%lu", (unsigned long)mcfg->mb_poll_period_ms);
        send_input(req, "Poll period (ms)", "mb_period", tmpb);
        httpd_resp_sendstr_chunk(req, "</div>");

        httpd_resp_sendstr_chunk(req,
            "<div class=\"rtu-toolbar\">"
            "<button type=\"button\" class=\"btn sm\" id=\"rtu-add\">Add device</button>"
            "</div>"
            "<p class=\"rtu-empty\" id=\"rtu-empty\"");
        if (used_n > 0) {
            httpd_resp_sendstr_chunk(req, " hidden");
        }
        httpd_resp_sendstr_chunk(req,
            ">No devices yet. Press Add device to configure a meter.</p>"
            "<div id=\"rtu-list\"></div>");

        /* Seed JSON for existing devices; client builds cards on DOMContentLoaded. */
        httpd_resp_sendstr_chunk(req, "<script type=\"application/json\" id=\"rtu-seed\">[");
        bool first = true;
        for (int i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
            const config_mb_slot_t *s = &mcfg->mb_slots[i];
            if (!s->used) {
                continue;
            }
            char js[256];
            char name_js[CONFIG_MANAGER_MB_NAME_LEN * 2];
            size_t o = 0;
            for (size_t k = 0; s->name[k] != '\0' && o + 2 < sizeof(name_js); k++) {
                char c = s->name[k];
                if (c == '\\' || c == '"') {
                    name_js[o++] = '\\';
                }
                if ((unsigned char)c < 0x20) {
                    continue;
                }
                name_js[o++] = c;
            }
            name_js[o] = '\0';
            snprintf(js, sizeof(js),
                     "%s{\"slot\":%d,\"type\":\"%s\",\"id\":\"%u\",\"name\":\"%s\"}",
                     first ? "" : ",",
                     i,
                     s->type == 1 ? "em07k" : "pm710",
                     (unsigned)(s->slave_id ? s->slave_id : 1),
                     name_js);
            first = false;
            httpd_resp_sendstr_chunk(req, js);
        }
        httpd_resp_sendstr_chunk(req, "]</script></div></details>");
    }

    /* Calibration: enter true V/I; check result on the device LCD. Persist via Save and restart.
     * Whole block rides CONFIG_APP_WEB_CALIB_ENABLE — with the gate off (production) the
     * section, its /api/calib endpoints and the persistence hook are all compiled out. */
#if CONFIG_APP_WEB_CALIB_ENABLE
    {
        char profile[64];
        calib_format_profile(profile, sizeof(profile));
        bool hide_phase_b_v = false;
        atm90e32as_calib_t calib;
        if (energy_meter_get_calibration(&calib) == ESP_OK &&
            calib.wiring_mode == ATM90E32AS_WIRING_3P3W) {
            hide_phase_b_v = true; /* default channel is V → hide B on first paint */
        }
        httpd_resp_sendstr_chunk(req,
            "<details class=\"section\" id=\"calib\"><summary>Calibration</summary><div class=\"sbody\">"
            "<p class=\"muted\">Enter the real voltage (V) or current (A) you applied. "
            "Keep changes with Save and restart below.</p>"
            "<p>Active profile: <span id=\"calib-profile\" class=\"st\">");
        send_escaped(req, profile);
        httpd_resp_sendstr_chunk(req,
            "</span></p>"
            "<p>Status: <span id=\"calib-st\" class=\"st\">Ready</span></p>"
            "<form method=\"post\" action=\"/api/calib/auto\" data-calib=\"1\" data-st=\"calib-st\" data-keep=\"1\">"
            "<div class=\"row\">"
            "<div class=\"field\"><label>Phase</label>"
            "<select class=\"input\" name=\"phase\" id=\"calib-phase\">"
            "<option value=\"A\">A</option>");
        /* 3P3W has no phase-B voltage; hide option B when default channel is V. */
        if (hide_phase_b_v) {
            httpd_resp_sendstr_chunk(req,
                "<option value=\"B\" id=\"calib-phase-b\" hidden disabled>B</option>");
        } else {
            httpd_resp_sendstr_chunk(req,
                "<option value=\"B\" id=\"calib-phase-b\">B</option>");
        }
        httpd_resp_sendstr_chunk(req,
            "<option value=\"C\">C</option>"
            "</select></div>"
            "<div class=\"field\"><label>Channel</label>"
            "<select class=\"input\" name=\"field\" id=\"calib-field\">"
            "<option value=\"V\">Voltage (V)</option><option value=\"I\">Current (A)</option>"
            "</select></div>"
            "<div class=\"field wide\"><label>True value</label>"
            "<input class=\"input\" name=\"value\" placeholder=\"230 or 5.000\" required>"
            "</div></div>"
            "<button class=\"btn\" type=\"submit\">Calibrate</button>"
            "</form></div></details>");
    }
#endif /* CONFIG_APP_WEB_CALIB_ENABLE */

    /* The page's only form. Its inputs sit in the sections above, tied here by the
     * CFG_FORM_ID "form" attribute, because the certificate pickers are forms of
     * their own and HTML does not allow nesting. */
    httpd_resp_sendstr_chunk(req,
        "<section class=\"section\" id=\"save\"><h2>Save changes</h2>"
        "<p class=\"muted\">The device saves, then restarts. Takes a few seconds.</p>"
        "<form method=\"post\" action=\"/save\" id=\"" CFG_FORM_ID "\">"
        "<button class=\"btn block\" type=\"submit\">Save and restart</button>"
        "</form></section>");

    /* At the end of the body so the handlers bind to a page that already exists. */
    httpd_resp_sendstr_chunk(req, "<script>");
#if CONFIG_APP_WEB_CALIB_ENABLE
    httpd_resp_sendstr_chunk(req, HTML_SCRIPT_CALIB);
#endif
    httpd_resp_sendstr_chunk(req, HTML_SCRIPT);
    httpd_resp_sendstr_chunk(req, "</script>");

    free(mcfg);
    return send_page_end(req);
}

static esp_err_t login_get_handler(httpd_req_t *req)
{
    send_page_start(req, "Sign in - PowerMeter Setting");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Settings</span>"
        "<h1>Sign in</h1>"
        "<form method=\"post\" action=\"/login\"><div class=\"row\">"
        "<div class=\"field wide\"><label>Username</label>"
        "<input class=\"input\" name=\"user\" autocomplete=\"username\"></div>"
        "<div class=\"field wide\"><label>Password</label>"
        "<input class=\"input\" name=\"pass\" type=\"password\" autocomplete=\"current-password\"></div>"
        "<div class=\"field wide\"><button class=\"btn block\" type=\"submit\">Sign in</button></div>"
        "</div></form>");
    return send_page_end(req);
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
#if !CONFIG_APP_CONSOLE_AUTH_ENABLE
    ESP_LOGW(TAG, "console auth disabled; web login bypassed");
    send_redirect(req, "/");
    return ESP_OK;
#else
    if (req->content_len <= 0 || req->content_len >= 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    char body[256];
    int got = httpd_req_recv(req, body, req->content_len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_OK;
    }
    body[got] = '\0';

    char user[64] = "";
    char pass[96] = "";
    form_get_value(body, "user", user, sizeof(user));
    form_get_value(body, "pass", pass, sizeof(pass));

    if (strcmp(user, CONFIG_APP_CONSOLE_AUTH_USERNAME) == 0 &&
        strcmp(pass, CONFIG_APP_CONSOLE_AUTH_PASSWORD) == 0) {
        snprintf(s_session_token, sizeof(s_session_token), "%08" PRIx32 "%08" PRIx32,
                 esp_random(), esp_random());
        char cookie[96];
        snprintf(cookie, sizeof(cookie), "wp_session=%s; Path=/; HttpOnly; SameSite=Strict", s_session_token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        ESP_LOGI(TAG, "web login ok");
        send_redirect(req, "/");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "web login failed for user '%s'", user);
    send_page_start(req, "Sign in failed - PowerMeter Setting");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Settings</span>"
        "<h1>Sign in</h1>"
        "<div class=\"err\">Wrong username or password.</div>"
        "<form method=\"post\" action=\"/login\"><div class=\"row\">"
        "<div class=\"field wide\"><label>Username</label>"
        "<input class=\"input\" name=\"user\" autocomplete=\"username\"></div>"
        "<div class=\"field wide\"><label>Password</label>"
        "<input class=\"input\" name=\"pass\" type=\"password\" autocomplete=\"current-password\"></div>"
        "<div class=\"field wide\"><button class=\"btn block\" type=\"submit\">Sign in</button></div>"
        "</div></form>");
    return send_page_end(req);
#endif
}

static esp_err_t register_handlers(void)
{
    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &root), TAG, "register / failed");

    const char *probe_uris[] = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/ncsi.txt",
        "/connecttest.txt",
        "/fwlink",
    };
    for (size_t i = 0; i < sizeof(probe_uris) / sizeof(probe_uris[0]); i++) {
        const httpd_uri_t probe = {
            .uri = probe_uris[i],
            .method = HTTP_GET,
            .handler = captive_redirect_handler,
            .user_ctx = NULL,
        };
        esp_err_t ret = httpd_register_uri_handler(s_httpd, &probe);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "register captive URI %s failed: %s", probe_uris[i], esp_err_to_name(ret));
        }
    }

    const httpd_uri_t login_get = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_get_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_get), TAG, "register GET /login failed");

    const httpd_uri_t login_post = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_post), TAG, "register POST /login failed");

    /* One endpoint for every text setting on the page. It replaced
     * /save/network, /save/mqtt and /save/system, which forced the operator to
     * remember which Save button covered which field. */
    const httpd_uri_t save_all = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_all_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save_all), TAG, "register POST /save failed");

    /* MQTT TLS certificate management. Upload and delete are POST; the GET only
     * reports presence/size/fingerprint, never certificate or key content. */
    const httpd_uri_t cert_upload = {
        .uri = "/api/cert",
        .method = HTTP_POST,
        .handler = cert_upload_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &cert_upload), TAG, "register POST /api/cert failed");

    const httpd_uri_t cert_delete = {
        .uri = "/api/cert/delete",
        .method = HTTP_POST,
        .handler = cert_delete_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &cert_delete), TAG, "register POST /api/cert/delete failed");

    const httpd_uri_t cert_status = {
        .uri = "/api/cert",
        .method = HTTP_GET,
        .handler = cert_status_get_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &cert_status), TAG, "register GET /api/cert failed");

#if CONFIG_APP_WEB_CALIB_ENABLE
    const httpd_uri_t calib_profile = {
        .uri = "/api/calib/profile",
        .method = HTTP_GET,
        .handler = calib_profile_get_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &calib_profile), TAG, "register GET /api/calib/profile failed");

    const httpd_uri_t calib_auto = {
        .uri = "/api/calib/auto",
        .method = HTTP_POST,
        .handler = calib_auto_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &calib_auto), TAG, "register POST /api/calib/auto failed");
#endif /* CONFIG_APP_WEB_CALIB_ENABLE */

    const httpd_uri_t reboot = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &reboot), TAG, "register POST /reboot failed");

    return ESP_OK;
}

esp_err_t web_portal_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    s_session_token[0] = '\0';

    esp_err_t dns_ret = captive_dns_start();
    if (dns_ret == ESP_OK) {
        ESP_LOGI(TAG, "captive DNS server started");
    } else {
        ESP_LOGW(TAG, "captive DNS server start failed: %s", esp_err_to_name(dns_ret));
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 12288;
    /* Base + cert(3) + calib auto + reboot. */
    cfg.max_uri_handlers = 22;

    esp_err_t ret = httpd_start(&s_httpd, &cfg);
    if (ret != ESP_OK) {
        if (s_dns_running) {
            captive_dns_stop();
            s_dns_running = false;
        }
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = register_handlers();
    if (ret != ESP_OK) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        if (s_dns_running) {
            captive_dns_stop();
            s_dns_running = false;
        }
        return ret;
    }

    ESP_LOGI(TAG, "web portal started on SoftAP (http://192.168.4.1/)");
    return ESP_OK;
}

esp_err_t web_portal_stop(void)
{
    if (s_httpd == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_httpd);
    s_httpd = NULL;
    s_session_token[0] = '\0';
    if (s_dns_running) {
        captive_dns_stop();
        ESP_LOGI(TAG, "captive DNS server stopped");
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "web portal stopped");
    }
    return ret;
}

bool web_portal_is_running(void)
{
    return s_httpd != NULL;
}
