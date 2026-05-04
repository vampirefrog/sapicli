#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <http.h>
#include <objbase.h>

#include "http.h"
#include "service.h"
#include "handlers.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "httpapi.lib")

namespace sapisrv {

namespace {

const HTTPAPI_VERSION kVersion = HTTPAPI_VERSION_2;

// StreamWriter that sends an HTTP API v2 chunked response.
// First write triggers HttpSendHttpResponse (headers + first chunk + MORE_DATA);
// subsequent writes use HttpSendResponseEntityBody with MORE_DATA;
// finish() sends a final empty body without MORE_DATA to close.
class HttpStreamWriter final : public StreamWriter {
public:
    HttpStreamWriter(HANDLE queue, HTTP_REQUEST_ID req_id) : queue_(queue), req_id_(req_id) {}

    void start(int status, const char* status_text, const char* content_type) override {
        status_ = status;
        status_text_ = status_text;
        content_type_ = content_type;
    }

    void write(const void* data, std::size_t len) override {
        if (!headers_sent_) {
            send_headers_with_chunk(data, len);
            headers_sent_ = true;
            if (data && len) entity_open_ = true;
        } else {
            send_chunk(data, len);
            if (len) entity_open_ = true;
        }
    }

    void finish() override {
        if (!headers_sent_) {
            send_headers_with_chunk(nullptr, 0);
            headers_sent_ = true;
        }
        if (entity_open_) {
            // Final call: no MORE_DATA flag → closes the response stream.
            HttpSendResponseEntityBody(queue_, req_id_, 0, 0, nullptr, nullptr,
                                       nullptr, 0, nullptr, nullptr);
        }
        finished_ = true;
    }

private:
    void send_headers_with_chunk(const void* data, std::size_t len) {
        HTTP_RESPONSE r{};
        r.StatusCode = static_cast<USHORT>(status_);
        r.pReason = status_text_.c_str();
        r.ReasonLength = static_cast<USHORT>(status_text_.size());
        r.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = content_type_.c_str();
        r.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength =
            static_cast<USHORT>(content_type_.size());

        HTTP_DATA_CHUNK chunk{};
        if (data && len) {
            chunk.DataChunkType = HttpDataChunkFromMemory;
            chunk.FromMemory.pBuffer = const_cast<void*>(data);
            chunk.FromMemory.BufferLength = static_cast<ULONG>(len);
            r.EntityChunkCount = 1;
            r.pEntityChunks = &chunk;
        }
        ULONG sent = 0;
        HttpSendHttpResponse(queue_, req_id_, HTTP_SEND_RESPONSE_FLAG_MORE_DATA,
                             &r, nullptr, &sent, nullptr, 0, nullptr, nullptr);
    }

    void send_chunk(const void* data, std::size_t len) {
        if (!data || !len) return;
        HTTP_DATA_CHUNK chunk{};
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = const_cast<void*>(data);
        chunk.FromMemory.BufferLength = static_cast<ULONG>(len);
        HttpSendResponseEntityBody(queue_, req_id_, HTTP_SEND_RESPONSE_FLAG_MORE_DATA,
                                   1, &chunk, nullptr, nullptr, 0, nullptr, nullptr);
    }

    HANDLE queue_;
    HTTP_REQUEST_ID req_id_;
    int status_ = 200;
    std::string status_text_ = "OK";
    std::string content_type_ = "application/octet-stream";
    bool headers_sent_ = false;
    bool entity_open_ = false;
    bool finished_ = false;
};

bool path_equals(const HTTP_COOKED_URL& url, const wchar_t* p) {
    if (!url.pAbsPath) return false;
    auto plen = wcslen(p);
    if (url.AbsPathLength / sizeof(wchar_t) != plen) return false;
    return wcsncmp(url.pAbsPath, p, plen) == 0;
}

void send_simple(HANDLE queue, HTTP_REQUEST_ID req_id, int status, const char* text) {
    HttpStreamWriter w(queue, req_id);
    w.start(status, text, "text/plain; charset=utf-8");
    w.write(text, strlen(text));
    w.finish();
}

void dispatch(HANDLE queue, const HTTP_REQUEST* req) {
    if (req->Verb == HttpVerbGET && path_equals(req->CookedUrl, L"/voices")) {
        HttpStreamWriter w(queue, req->RequestId);
        handle_voices(w);
        return;
    }
    if (req->Verb == HttpVerbGET && path_equals(req->CookedUrl, L"/synthesize")) {
        HttpStreamWriter w(queue, req->RequestId);
        std::wstring qs;
        if (req->CookedUrl.pQueryString && req->CookedUrl.QueryStringLength) {
            qs.assign(req->CookedUrl.pQueryString,
                      req->CookedUrl.QueryStringLength / sizeof(wchar_t));
        }
        handle_synthesize(qs, w);
        return;
    }
    send_simple(queue, req->RequestId, 404, "Not Found");
}

void worker_loop(HANDLE queue) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        fwprintf(stderr, L"worker CoInitialize failed: 0x%08x\n", (unsigned)hr);
        return;
    }
    std::vector<unsigned char> buf(8192);
    for (;;) {
        ULONG bytes = 0;
        DWORD r = HttpReceiveHttpRequest(queue, HTTP_NULL_ID, 0,
            reinterpret_cast<HTTP_REQUEST*>(buf.data()), static_cast<ULONG>(buf.size()),
            &bytes, nullptr);
        if (r == ERROR_MORE_DATA) {
            buf.resize(bytes);
            continue;
        }
        if (r == ERROR_OPERATION_ABORTED || r == ERROR_HANDLE_EOF) break;
        if (r != NO_ERROR) {
            fwprintf(stderr, L"HttpReceiveHttpRequest failed: %lu\n", r);
            break;
        }
        dispatch(queue, reinterpret_cast<HTTP_REQUEST*>(buf.data()));
    }
    CoUninitialize();
}

void shutdown_watcher(HANDLE queue) {
    WaitForSingleObject(stop_event(), INFINITE);
    HttpShutdownRequestQueue(queue);
}

}  // namespace

int run_http_server(const HttpConfig& cfg) {
    DWORD r = HttpInitialize(kVersion, HTTP_INITIALIZE_SERVER, nullptr);
    if (r != NO_ERROR) {
        fwprintf(stderr, L"HttpInitialize failed: %lu\n", r);
        return 1;
    }

    HTTP_SERVER_SESSION_ID session = HTTP_NULL_ID;
    r = HttpCreateServerSession(kVersion, &session, 0);
    if (r != NO_ERROR) {
        fwprintf(stderr, L"HttpCreateServerSession failed: %lu\n", r);
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return 1;
    }

    HTTP_URL_GROUP_ID url_group = HTTP_NULL_ID;
    r = HttpCreateUrlGroup(session, &url_group, 0);
    if (r != NO_ERROR) {
        fwprintf(stderr, L"HttpCreateUrlGroup failed: %lu\n", r);
        HttpCloseServerSession(session);
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return 1;
    }

    HANDLE queue = nullptr;
    r = HttpCreateRequestQueue(kVersion, nullptr, nullptr, 0, &queue);
    if (r != NO_ERROR) {
        fwprintf(stderr, L"HttpCreateRequestQueue failed: %lu\n", r);
        HttpCloseUrlGroup(url_group);
        HttpCloseServerSession(session);
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return 1;
    }

    HTTP_BINDING_INFO binding{};
    binding.Flags.Present = 1;
    binding.RequestQueueHandle = queue;
    r = HttpSetUrlGroupProperty(url_group, HttpServerBindingProperty, &binding, sizeof(binding));
    if (r != NO_ERROR) {
        fwprintf(stderr, L"HttpSetUrlGroupProperty failed: %lu\n", r);
        HttpCloseRequestQueue(queue);
        HttpCloseUrlGroup(url_group);
        HttpCloseServerSession(session);
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return 1;
    }

    r = HttpAddUrlToUrlGroup(url_group, cfg.url_prefix.c_str(), 0, 0);
    if (r != NO_ERROR) {
        if (r == ERROR_ACCESS_DENIED) {
            fwprintf(stderr,
                L"Access denied binding %s\n"
                L"Reserve the URL with (admin shell):\n"
                L"  netsh http add urlacl url=%s user=\"%s\"\n",
                cfg.url_prefix.c_str(), cfg.url_prefix.c_str(),
                L"NT AUTHORITY\\NetworkService");
        } else {
            fwprintf(stderr, L"HttpAddUrlToUrlGroup(%s) failed: %lu\n", cfg.url_prefix.c_str(), r);
        }
        HttpCloseRequestQueue(queue);
        HttpCloseUrlGroup(url_group);
        HttpCloseServerSession(session);
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return 1;
    }

    fwprintf(stderr, L"sapisrv listening on %s\n", cfg.url_prefix.c_str());

    std::thread shutdown_thread(shutdown_watcher, queue);

    std::vector<std::thread> workers;
    workers.reserve(cfg.worker_threads);
    for (int i = 0; i < cfg.worker_threads; ++i) {
        workers.emplace_back(worker_loop, queue);
    }
    for (auto& t : workers) t.join();
    shutdown_thread.join();

    HttpRemoveUrlFromUrlGroup(url_group, cfg.url_prefix.c_str(), 0);
    HttpCloseRequestQueue(queue);
    HttpCloseUrlGroup(url_group);
    HttpCloseServerSession(session);
    HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
    return 0;
}

int run_server() {
    HttpConfig cfg;
    return run_http_server(cfg);
}

}  // namespace sapisrv
