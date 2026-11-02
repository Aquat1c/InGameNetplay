#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace netplay::tls
{
bool IsAvailable();

bool HttpGet(
    const std::string& url,
    bool verifyPeer,
    uint32_t connectTimeoutMs,
    uint32_t receiveTimeoutMs,
    std::string* outBody,
    std::string* outError);

// Single-thread use.
class KeepAliveClient
{
public:
    KeepAliveClient();
    ~KeepAliveClient();
    KeepAliveClient(const KeepAliveClient&) = delete;
    KeepAliveClient& operator=(const KeepAliveClient&) = delete;

    bool Get(
        const std::string& url,
        bool verifyPeer,
        uint32_t connectTimeoutMs,
        uint32_t receiveTimeoutMs,
        std::string* outBody,
        std::string* outError);

private:
    struct Connection;
    std::unique_ptr<Connection> m_connection;
    friend bool HttpGet(
        const std::string&, bool, uint32_t, uint32_t, std::string*, std::string*);
};

// Without Content-Length or chunking the response ends only on close, so this stays false.
inline bool IsHttpResponseComplete(const std::string& raw, bool* outClose)
{
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        return false;
    }

    std::string headers = "\r\n" + raw.substr(0, headerEnd + 2);
    for (char& c : headers)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const bool close = headers.find("\r\nconnection: close\r\n") != std::string::npos;
    if (outClose != nullptr)
    {
        *outClose = close;
    }

    const size_t bodyStart = headerEnd + 4;
    if (headers.find("\r\ntransfer-encoding: chunked\r\n") != std::string::npos)
    {
        size_t cursor = bodyStart;
        for (;;)
        {
            const size_t lineEnd = raw.find("\r\n", cursor);
            if (lineEnd == std::string::npos)
            {
                return false;
            }
            char* parsedEnd = nullptr;
            const std::string sizeText = raw.substr(cursor, lineEnd - cursor);
            const unsigned long chunkSize = std::strtoul(sizeText.c_str(), &parsedEnd, 16);
            if (parsedEnd == sizeText.c_str())
            {
                if (outClose != nullptr)
                {
                    *outClose = true;
                }
                return false;
            }
            cursor = lineEnd + 2;
            if (chunkSize == 0)
            {
                if (raw.compare(cursor, 2, "\r\n") == 0)
                {
                    return true;
                }
                return raw.find("\r\n\r\n", cursor) != std::string::npos;
            }
            cursor += static_cast<size_t>(chunkSize) + 2;
            if (cursor > raw.size())
            {
                return false;
            }
        }
    }

    const size_t lengthPos = headers.find("\r\ncontent-length:");
    if (lengthPos == std::string::npos)
    {
        if (outClose != nullptr)
        {
            *outClose = true;
        }
        return false;
    }
    const unsigned long length = std::strtoul(
        headers.c_str() + lengthPos + std::char_traits<char>::length("\r\ncontent-length:"),
        nullptr,
        10);
    return raw.size() - bodyStart >= static_cast<size_t>(length);
}
}
