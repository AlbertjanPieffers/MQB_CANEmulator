#include "WebUi.h"
#include "AppConfig.h"

#include <WiFi.h>
#include <Update.h>
#include "esp_ota_ops.h"

#include <Print.h>

class ChunkedHttpPrint : public Print {
public:
    explicit ChunkedHttpPrint(WebServer& server)
        : _server(server) {
    }

    size_t write(uint8_t byte) override {
        return write(&byte, 1);
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        if (buffer == nullptr || size == 0) {
            return 0;
        }

        _server.sendContent(
            reinterpret_cast<const char*>(buffer),
            size
        );

        return size;
    }

private:
    WebServer& _server;
};

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MQB Guided Logger</title>
<style>
body{margin:0;font-family:Arial,sans-serif;background:#0d1117;color:#eef2f7}
header{padding:18px 20px;background:#161b22;border-bottom:1px solid #30363d}
header h1{margin:0;font-size:22px}.sub{color:#8b949e;margin-top:4px}
main{max-width:980px;margin:auto;padding:18px}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:18px;margin-bottom:16px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:10px}
.metric{background:#0d1117;border-radius:9px;padding:12px;border:1px solid #21262d}
.label{font-size:12px;color:#8b949e}.value{font-size:20px;margin-top:5px}
.stepTitle{font-size:26px;margin:8px 0}.instruction{font-size:18px;line-height:1.5;color:#d8dee9}
button,.button{display:inline-block;border:0;border-radius:8px;padding:11px 13px;margin:5px 5px 5px 0;background:#1f6feb;color:white;font-weight:600;text-decoration:none;cursor:pointer}
.secondary{background:#30363d}.danger{background:#a83c3c}.good{background:#238636}
input[type=text]{width:100%;box-sizing:border-box;padding:12px;background:#0d1117;border:1px solid #30363d;color:white;border-radius:8px}
table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:9px;border-bottom:1px solid #30363d}
.progress{height:8px;background:#21262d;border-radius:5px;overflow:hidden;margin-top:15px}.bar{height:100%;background:#1f6feb;width:0}
nav a{color:#c9d1d9;text-decoration:none;margin-right:14px}.small{font-size:13px;color:#8b949e}
.warn{color:#f2cc60}
</style>
</head>
<body>
<header>
<h1>MQB Guided Logger</h1>
<div class="sub">CAN / BAP research tool <span id="firmwareVersion">-</span></div>
</header>

<main>
<div class="card">
<nav>
<a href="#guide">Guided test</a>
<a href="#logs">Logs</a>
<a href="/firmware">Firmware</a>
</nav>
</div>

<div class="card">
<div class="grid">
<div class="metric"><div class="label">CAN bus</div><div class="value" id="bus">-</div></div>
<div class="metric"><div class="label">Frames / second</div><div class="value" id="fps">0</div></div>
<div class="metric"><div class="label">Logged frames</div><div class="value" id="logged">0</div></div>
<div class="metric"><div class="label">Dropped frames</div><div class="value" id="dropped">0</div></div>
<div class="metric"><div class="label">Buffer</div><div class="value" id="buffer">0%</div></div>
<div class="metric"><div class="label">Buffer memory</div><div class="value" id="memory">-</div></div>
<div class="metric"><div class="label">Flash size</div><div class="value" id="flashSize">-</div></div>
<div class="metric"><div class="label">PSRAM size</div><div class="value" id="psramSize">-</div></div>
<div class="metric"><div class="label">Free PSRAM</div><div class="value" id="psramFree">-</div></div>
<div class="metric"><div class="label">LittleFS</div><div class="value" id="fsState">-</div></div>
<div class="metric"><div class="label">Storage total</div><div class="value" id="storageTotal">-</div></div>
<div class="metric"><div class="label">Storage used</div><div class="value" id="storageUsed">-</div></div>
<div class="metric"><div class="label">Storage free</div><div class="value" id="storage">-</div></div>
<div class="metric"><div class="label">Logger</div><div class="value" id="logging">Stopped</div></div>
</div>
<div id="storageWarning" class="small warn" style="margin-top:12px"></div>
</div>


<div class="card">
<h3>Build information</h3>
<div class="grid">
<div class="metric"><div class="label">Firmware version</div><div class="value" id="buildVersion">-</div></div>
<div class="metric"><div class="label">Build date</div><div class="value" id="buildDate">-</div></div>
<div class="metric"><div class="label">Hardware</div><div class="value" id="hardware">-</div></div>
<div class="metric"><div class="label">Running OTA slot</div><div class="value" id="otaSlot">-</div></div>
</div>
</div>

<div class="card" id="guide">
<div class="small" id="stepCounter">Step</div>
<div class="stepTitle" id="stepTitle">Loading...</div>
<div class="instruction" id="instruction"></div>
<div class="small" id="wait"></div>
<div class="progress"><div class="bar" id="bar"></div></div>

<div style="margin-top:16px">
<button onclick="post('/api/session/start')">Start new session</button>
<button onclick="post('/api/step/next')">Step completed</button>
<button class="secondary" onclick="post('/api/step/previous')">Previous step</button>
<button class="danger" onclick="post('/api/session/stop')">Stop session</button>
</div>
</div>

<div class="card">
<h3>Add event marker</h3>
<input id="markerText" type="text" placeholder="Example: warning appeared on display">
<button onclick="addMarker()">Add marker</button>
</div>

<div class="card" id="logs">
<h3>Stored sessions</h3>
<div class="small">Sessions are stored internally as compact .mqblog files. Export converts them on demand.</div>
<table>
<thead><tr><th>File</th><th>Size</th><th>Export</th><th></th></tr></thead>
<tbody id="sessions"></tbody>
</table>
</div>
</main>

<script>
async function post(url, body=''){
    const response = await fetch(url,{
        method:'POST',
        headers:{'Content-Type':'text/plain'},
        body
    });

    if(!response.ok){
        alert(await response.text());
    }

    await refresh();
}

async function addMarker(){
    const input=document.getElementById('markerText');

    if(!input.value.trim()){
        return;
    }

    await post('/api/marker',input.value.trim());
    input.value='';
}

async function removeSession(name){
    if(!confirm('Delete '+name+'?')){
        return;
    }

    const response=await fetch(
        '/api/session/delete?name='+encodeURIComponent(name),
        {method:'POST'}
    );

    if(!response.ok){
        alert(await response.text());
    }

    await loadSessions();
    await refresh();
}

async function refresh(){
    try{
        const r=await fetch('/api/status');
        const s=await r.json();

        document.getElementById('firmwareVersion').textContent='v'+s.version;
        document.getElementById('buildVersion').textContent='v'+s.version;
        document.getElementById('buildDate').textContent=s.buildDate+' '+s.buildTime;
        document.getElementById('hardware').textContent=s.hardware;
        document.getElementById('otaSlot').textContent=s.otaSlot;

        document.getElementById('bus').textContent=s.busActive?'ACTIVE':'NO TRAFFIC';
        document.getElementById('fps').textContent=s.fps.toLocaleString();
        document.getElementById('logged').textContent=s.loggedFrames.toLocaleString();
        document.getElementById('dropped').textContent=s.droppedFrames.toLocaleString();
        document.getElementById('buffer').textContent=s.bufferPercent+'%';
        document.getElementById('memory').textContent=s.usingPsram?'PSRAM':'INTERNAL RAM';
        document.getElementById('flashSize').textContent=(s.flashBytes/1048576).toFixed(2)+' MiB';
        document.getElementById('psramSize').textContent=s.psramDetected?(s.psramBytes/1048576).toFixed(2)+' MiB':'NOT FOUND';
        document.getElementById('psramFree').textContent=s.psramDetected?(s.freePsramBytes/1048576).toFixed(2)+' MiB':'-';
        document.getElementById('fsState').textContent=s.storageReady?'READY':'ERROR';
        document.getElementById('storageTotal').textContent=s.storageReady?(s.storageTotalBytes/1048576).toFixed(2)+' MiB':'-';
        document.getElementById('storageUsed').textContent=s.storageReady?(s.storageUsedBytes/1048576).toFixed(2)+' MiB':'-';
        document.getElementById('storage').textContent=s.storageReady?s.storageFreePercent+'%':'ERROR';
        document.getElementById('logging').textContent=s.logging?'RECORDING':'STOPPED';

        document.getElementById('stepCounter').textContent=
            'Step '+(s.step+1)+' of '+s.stepCount;

        document.getElementById('stepTitle').textContent=s.stepTitle;
        document.getElementById('instruction').textContent=s.instruction;

        document.getElementById('wait').textContent=
            s.waitSeconds>0 ?
            'Recommended wait: '+s.waitSeconds+' seconds' :
            '';

        document.getElementById('bar').style.width=
            ((s.step+1)/s.stepCount*100)+'%';

        let warning='';
        if(!s.storageReady){
            warning='LittleFS is not mounted correctly. Check the partition table / label.';
        } else if(s.storageFreePercent<=10){
            warning='Storage is almost full. The logger automatically stops at 5% free space.';
        } else if(!s.psramDetected){
            warning='PSRAM was not detected. Logging uses the smaller internal RAM buffer.';
        }
        document.getElementById('storageWarning').textContent=warning;
    }
    catch(e){}
}

async function loadSessions(){
    try{
        const r=await fetch('/api/sessions');
        const rows=await r.json();
        const el=document.getElementById('sessions');

        el.innerHTML='';

        for(const row of rows){
            const tr=document.createElement('tr');
            const kb=(row.size/1024).toFixed(1);

            tr.innerHTML=
                '<td>'+row.name+'</td>'+
                '<td>'+kb+' KB</td>'+
                '<td>'+
                '<a class="button good" href="/api/raw?name='+encodeURIComponent(row.name)+'">MQBLOG</a>'+
                '<a class="button good" href="/api/export?format=csv&name='+encodeURIComponent(row.name)+'">CSV</a>'+
                '<a class="button good" href="/api/export?format=asc&name='+encodeURIComponent(row.name)+'">ASC</a>'+
                '<a class="button good" href="/api/export?format=candump&name='+encodeURIComponent(row.name)+'">candump</a>'+
                '</td>'+
                '<td><button class="danger" onclick="removeSession(\''+
                row.name.replaceAll("'","\\'")+
                '\')">Delete</button></td>';

            el.appendChild(tr);
        }
    }
    catch(e){}
}

setInterval(refresh,1000);
setInterval(loadSessions,5000);

refresh();
loadSessions();
</script>
</body>
</html>
)HTML";

static const char FIRMWARE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware Update</title>
<style>
body{font-family:Arial,sans-serif;background:#0d1117;color:#eef2f7;margin:0}
main{max-width:650px;margin:50px auto;padding:20px}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:20px}
input,button{margin-top:12px}
button{border:0;border-radius:8px;padding:12px 14px;background:#1f6feb;color:white;font-weight:600}
a{color:#8dbdff}
</style>
</head>
<body>
<main>
<div class="card">
<h2>Firmware Update</h2>
<p>Select a compiled ESP32-S3 firmware .bin file.</p>
<p>Logging is stopped automatically before the update starts.</p>
<form method="POST" action="/api/firmware" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin" required>
<br>
<button type="submit">Install firmware</button>
</form>
<p><a href="/">Back to logger</a></p>
</div>
</main>
</body>
</html>
)HTML";

void WebUi::begin(
    CanManager& canManager,
    LoggerManager& loggerManager,
    ProfileManager& profileManager
) {
    _can = &canManager;
    _logger = &loggerManager;
    _profile = &profileManager;

    WiFi.mode(WIFI_AP);

    IPAddress ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);

    WiFi.softAPConfig(ip, gateway, subnet);
    WiFi.softAP(APP_WIFI_SSID, APP_WIFI_PASSWORD);

    _dns.start(53, "*", ip);

    configureRoutes();
    _server.begin();
}

void WebUi::loop() {
    _dns.processNextRequest();
    _server.handleClient();
}

void WebUi::configureRoutes() {
    _server.on("/", HTTP_GET, [this]() {
        handleRoot();
    });

    _server.on("/api/status", HTTP_GET, [this]() {
        handleStatus();
    });

    _server.on("/api/session/start", HTTP_POST, [this]() {
        handleStartSession();
    });

    _server.on("/api/session/stop", HTTP_POST, [this]() {
        handleStopSession();
    });

    _server.on("/api/step/next", HTTP_POST, [this]() {
        handleNextStep();
    });

    _server.on("/api/step/previous", HTTP_POST, [this]() {
        handlePreviousStep();
    });

    _server.on("/api/marker", HTTP_POST, [this]() {
        handleAddMarker();
    });

    _server.on("/api/sessions", HTTP_GET, [this]() {
        handleListSessions();
    });

    _server.on("/api/session/delete", HTTP_POST, [this]() {
        handleDeleteSession();
    });

    _server.on("/api/export", HTTP_GET, [this]() {
        handleExport();
    });

    _server.on("/api/raw", HTTP_GET, [this]() {
        handleRawDownload();
    });

    _server.on("/firmware", HTTP_GET, [this]() {
        handleFirmwareUploadPage();
    });

    _server.on(
        "/api/firmware",
        HTTP_POST,
        [this]() {
            handleFirmwareUploadFinished();
        },
        [this]() {
            handleFirmwareUpload();
        }
    );

    _server.on("/generate_204", HTTP_GET, [this]() {
        _server.sendHeader("Location", "http://192.168.4.1", true);
        _server.send(302, "text/plain", "");
    });

    _server.on("/hotspot-detect.html", HTTP_GET, [this]() {
        _server.sendHeader("Location", "http://192.168.4.1", true);
        _server.send(302, "text/plain", "");
    });

    _server.onNotFound([this]() {
        _server.sendHeader("Location", "http://192.168.4.1", true);
        _server.send(302, "text/plain", "");
    });
}

void WebUi::handleRoot() {
    _server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    _server.sendHeader("Pragma", "no-cache");
    _server.sendHeader("Expires", "0");
    _server.send_P(200, "text/html", INDEX_HTML);
}

void WebUi::handleStatus() {
    const esp_partition_t* runningPartition = esp_ota_get_running_partition();

    String otaSlot = "unknown";

    if (runningPartition != nullptr && runningPartition->label != nullptr) {
        otaSlot = String(runningPartition->label);
    }


    const GuidedStep& step = _profile->step();

    const size_t capacity = _logger->bufferCapacity();
    const size_t used = _logger->bufferUsed();

    const uint32_t bufferPercent =
        capacity == 0 ? 0 :
        static_cast<uint32_t>((used * 100ULL) / capacity);

    String json = "{";
    json += "\"version\":\"" + String(APP_VERSION) + "\",";
    json += "\"buildDate\":\"" + String(__DATE__) + "\",";
    json += "\"buildTime\":\"" + String(__TIME__) + "\",";
    json += "\"hardware\":\"" + String(APP_HARDWARE_NAME) + "\",";
    json += "\"otaSlot\":\"" + otaSlot + "\",";
    json += "\"busActive\":" + String(_can->busActive() ? "true" : "false") + ",";
    json += "\"fps\":" + String(_can->framesPerSecond()) + ",";
    json += "\"totalFrames\":" + String(static_cast<unsigned long long>(_can->totalFrames())) + ",";
    json += "\"loggedFrames\":" + String(static_cast<unsigned long long>(_logger->loggedFrames())) + ",";
    json += "\"droppedFrames\":" + String(static_cast<unsigned long long>(_logger->droppedFrames())) + ",";
    json += "\"logging\":" + String(_logger->isLogging() ? "true" : "false") + ",";
    json += "\"usingPsram\":" + String(_logger->usingPsram() ? "true" : "false") + ",";
    json += "\"psramDetected\":" + String(psramFound() ? "true" : "false") + ",";
    json += "\"flashBytes\":" + String(static_cast<unsigned long long>(ESP.getFlashChipSize())) + ",";
    json += "\"psramBytes\":" + String(static_cast<unsigned long long>(ESP.getPsramSize())) + ",";
    json += "\"freePsramBytes\":" + String(static_cast<unsigned long long>(ESP.getFreePsram())) + ",";
    json += "\"bufferPercent\":" + String(bufferPercent) + ",";
    json += "\"storageReady\":" + String(_logger->storageReady() ? "true" : "false") + ",";
    json += "\"storageTotalBytes\":" + String(static_cast<unsigned long long>(_logger->storageTotalBytes())) + ",";
    json += "\"storageUsedBytes\":" + String(static_cast<unsigned long long>(_logger->storageUsedBytes())) + ",";
    json += "\"storageFreePercent\":" + String(_logger->storageFreePercent()) + ",";
    json += "\"storageFreeBytes\":" + String(static_cast<unsigned long long>(_logger->storageFreeBytes())) + ",";
    json += "\"step\":" + String(_profile->currentStep()) + ",";
    json += "\"stepCount\":" + String(_profile->stepCount()) + ",";
    json += "\"stepTitle\":\"" + String(step.title) + "\",";
    json += "\"instruction\":\"" + String(step.instruction) + "\",";
    json += "\"waitSeconds\":" + String(step.waitSeconds);
    json += "}";

    _server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    _server.sendHeader("Pragma", "no-cache");
    _server.send(200, "application/json", json);
}

void WebUi::handleStartSession() {
    _profile->reset();

    if (!_logger->startSession()) {
        _server.send(
            507,
            "text/plain",
            "Could not start session. Check LittleFS status and available storage."
        );
        return;
    }

    _logger->addMarker(_profile->currentStep(), "SESSION");
    _logger->addMarker(_profile->currentStep(), "STEP");

    _server.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleStopSession() {
    _logger->addMarker(_profile->currentStep(), "STOP");
    _logger->stopSession();

    _server.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleNextStep() {
    _logger->addMarker(_profile->currentStep(), "DONE");
    _profile->next();
    _logger->addMarker(_profile->currentStep(), "STEP");

    _server.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handlePreviousStep() {
    _profile->previous();
    _logger->addMarker(_profile->currentStep(), "BACK");

    _server.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleAddMarker() {
    String marker = _server.arg("plain");

    if (marker.length() > 8) {
        marker = marker.substring(0, 8);
    }

    _logger->addMarker(_profile->currentStep(), marker);

    _server.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleListSessions() {
    _server.send(
        200,
        "application/json",
        _logger->listSessionsJson()
    );
}

void WebUi::handleDeleteSession() {
    const String name = _server.arg("name");

    if (!_logger->deleteSession(name)) {
        _server.send(400, "text/plain", "Could not delete session.");
        return;
    }

    _server.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleExport() {
    const String name = _server.arg("name");
    const String format = _server.arg("format");

    String extension;
    String mimeType = "text/plain";

    if (format == "csv") {
        extension = ".csv";
        mimeType = "text/csv";
    }
    else if (format == "asc") {
        extension = ".asc";
        mimeType = "text/plain";
    }
    else if (format == "candump") {
        extension = ".log";
        mimeType = "text/plain";
    }
    else {
        _server.send(400, "text/plain", "Unsupported export format.");
        return;
    }

    String downloadName = name;
    downloadName.replace(".mqblog", extension);

    _server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"" + downloadName + "\""
    );

    // CONTENT_LENGTH_UNKNOWN makes WebServer use HTTP chunked transfer encoding.
    // All generated payload MUST then go through sendContent(), not directly
    // to the underlying WiFiClient.
    _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server.send(200, mimeType, "");

    ChunkedHttpPrint writer(_server);

    bool ok = false;

    if (format == "csv") {
        ok = _logger->exportCsv(name, writer);
    }
    else if (format == "asc") {
        ok = _logger->exportAsc(name, writer);
    }
    else if (format == "candump") {
        ok = _logger->exportCandump(name, writer);
    }

    if (!ok) {
        const char* errorText = "Export failed.\n";
        _server.sendContent(errorText, strlen(errorText));
    }

    // Explicitly terminate chunked transfer so browsers consider the file complete.
    _server.sendContent("", 0);
}

void WebUi::handleRawDownload() {
    const String name = _server.arg("name");

    if (
        name.isEmpty() ||
        name.indexOf("..") >= 0 ||
        name.indexOf("/") >= 0 ||
        name.indexOf("\\") >= 0 ||
        !name.endsWith(".mqblog")
    ) {
        _server.send(400, "text/plain", "Invalid session name.");
        return;
    }

    const String path = String(SESSION_DIR) + "/" + name;

    if (!LittleFS.exists(path)) {
        _server.send(404, "text/plain", "Session not found.");
        return;
    }

    File file = LittleFS.open(path, FILE_READ);

    if (!file) {
        _server.send(500, "text/plain", "Could not open session.");
        return;
    }

    _server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"" + name + "\""
    );

    // streamFile sends a normal response with the exact Content-Length.
    _server.streamFile(file, "application/octet-stream");
    file.close();
}

void WebUi::handleFirmwareUploadPage() {
    _server.send_P(200, "text/html", FIRMWARE_HTML);
}

void WebUi::handleFirmwareUpload() {
    HTTPUpload& upload = _server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        _logger->stopSession();

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
        const size_t written = Update.write(
            upload.buf,
            upload.currentSize
        );

        if (written != upload.currentSize) {
            Update.printError(Serial);
        }
    }
    else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) {
            Update.printError(Serial);
        }
    }
    else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
    }
}

void WebUi::handleFirmwareUploadFinished() {
    if (Update.hasError()) {
        _server.send(500, "text/plain", "Firmware update failed.");
        return;
    }

    _server.send(
        200,
        "text/html",
        "<html><body><h2>Firmware update complete</h2><p>The device will restart.</p></body></html>"
    );

    delay(750);
    ESP.restart();
}
