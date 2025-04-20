#pragma once

#include <list>
#include <WiFiServer.h>
#include <ESPmDNS.h>
#include "VideoFrameProvider.h"
#include <CRtspSession.h>
#include <arduino-timer.h>

// Custom streamer class that works with our VideoFrameProvider
class VideoStreamer : public CStreamer
{
private:
    VideoFrameProvider& videoProvider;
    
public:
    VideoStreamer(SOCKET aClient, VideoFrameProvider& provider) : CStreamer(aClient), videoProvider(provider)
    {
    }
    
    virtual void streamImage(uint32_t curMsec)
    {
        // Get a frame from the video provider
        auto fb = videoProvider.getFrame();
        if (fb)
        {
            // Stream the JPEG frame
            streamFrame(fb->buf, fb->len, curMsec);
            
            // Return the frame buffer
            videoProvider.returnFrame(fb);
        }
    }
};

class rtsp_server_video : public WiFiServer
{
public:
    rtsp_server_video(VideoFrameProvider& provider, unsigned long interval, int port = 554)
        : WiFiServer(port), videoProvider_(provider)
    {
        log_i("Starting RTSP server for video");
        WiFiServer::begin();
        timer_.every(interval, client_handler, this);
    }
    
    size_t num_connected()
    {
        return clients_.size();
    }
    
    void doLoop()
    {
        timer_.tick();
    }

private:
    struct rtsp_client
    {
    public:
        rtsp_client(const WiFiClient& client, VideoFrameProvider& provider)
        {
            wifi_client = client;
            streamer = std::shared_ptr<VideoStreamer>(new VideoStreamer(&wifi_client, provider));
            session = std::shared_ptr<CRtspSession>(new CRtspSession(&wifi_client, streamer.get()));
        }
        
        WiFiClient wifi_client;
        // Streamer for UDP/TCP based RTP transport
        std::shared_ptr<VideoStreamer> streamer;
        // RTSP session and state
        std::shared_ptr<CRtspSession> session;
    };
    
    VideoFrameProvider& videoProvider_;
    std::list<std::unique_ptr<rtsp_client>> clients_;
    uintptr_t task_;
    Timer<> timer_;
    
    static bool client_handler(void* arg)
    {
        auto self = static_cast<rtsp_server_video*>(arg);
        // Check if a client wants to connect
        auto new_client = self->accept();
        if (new_client)
            self->clients_.push_back(std::unique_ptr<rtsp_client>(new rtsp_client(new_client, self->videoProvider_)));
        
        auto now = millis();
        for (const auto& client : self->clients_)
        {
            // Handle requests
            client->session->handleRequests(0);
            // Send the frame. For now return the uptime as time marker, currMs
            client->session->broadcastCurrentFrame(now);
        }
        
        self->clients_.remove_if([](std::unique_ptr<rtsp_client> const& c)
                                 { return c->session->m_stopped; });
        
        return true;
    }
};