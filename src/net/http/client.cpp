#include "client.hpp"

#include <util/time.hpp>

GHTTPClient::GHTTPClient() {
    curl = curl_easy_init();
    GLOBED_REQUIRE(curl != nullptr, "cURL failed to initialize")

    threadHandle.setLoopFunction(&GHTTPClient::threadFunc);
    threadHandle.start(this);
}

GHTTPClient::~GHTTPClient() {
    threadHandle.stopAndWait();

    if (curl) {
        curl_easy_cleanup(curl);
        curl = nullptr;
    }

    geode::log::debug("HTTP client thread halted");
}

void GHTTPClient::send(GHTTPRequestHandle request) {
    requests.push(request);
}

void GHTTPClient::threadFunc() {
    if (!requests.waitForMessages(util::time::secs(1))) return;

    auto request = requests.pop();
    auto response = this->performRequest(request);

    geode::Loader::get()->queueInMainThread([response, request] {
        request.maybeCallback(response);
    });
}

static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::ostringstream* stream) {
    size_t totalSize = size * nmemb;
    *stream << std::string(static_cast<char*>(contents), totalSize);
    return totalSize;
}

static int progressCallback(GHTTPRequest* request, double dltotal, double dlnow, double ultotal, double ulnow) {
    // if cancelled, abort the request
    if (request->_cancelled) {
        return 1;
    }

    return 0;
}

GHTTPResponse GHTTPClient::performRequest(GHTTPRequestHandle handle) {
    GHTTPResponse response;

    GHTTPRequest& req = *handle.handle.get();

    // clear leftover data from previous request
    curl_easy_reset(curl);

    switch (req.reqData.reqType) {
        case GHTTPRequestType::Get:
            break;
        case GHTTPRequestType::Post:
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            break;
        case GHTTPRequestType::Put:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            break;
        case GHTTPRequestType::Delete:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
    }

    // post fields
    if (!req.reqData.payload.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.reqData.payload.c_str());
    } else if (req.reqData.reqType == GHTTPRequestType::Post) {
        // we dont talk about this
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "secret=Wmfd2893gb7");
    }

    // generic stuff
    curl_easy_setopt(curl, CURLOPT_USERAGENT, req.reqData.userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, req.reqData.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.reqData.followRedirects);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req.reqData.timeout);

    // security is for nerds
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    // http headers
    struct curl_slist* headerList = nullptr;
    if (!req.reqData.headers.empty()) {
        for (const auto& header: req.reqData.headers) {
            headerList = curl_slist_append(headerList, header.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    // data writing
    std::ostringstream ss;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ss);

    // cancellation
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, (void*)handle.handle.get());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

    response.resCode = curl_easy_perform(curl);
    response.failed = response.resCode != CURLE_OK;

    if (response.failed) {
        response.failMessage = curl_easy_strerror(response.resCode);
    } else {
        long httpCode;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        response.statusCode = httpCode;
        response.response = ss.str();
    }

    if (headerList != nullptr) {
        curl_slist_free_all(headerList);
    }

    return response;
}