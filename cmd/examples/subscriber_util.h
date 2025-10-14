//
// Created by schweitzer on 2025. 09. 29..
//

#pragma once

#ifndef QUICR_SUBSCRIBER_UTIL_H
#define QUICR_SUBSCRIBER_UTIL_H

#include "catalog.hpp"

class MySubscribeTrackHandler;

struct SubTrack
{
    CatalogTrackEntry track_entry;
    std::string namespace_;
    std::vector<uint8_t> init;
};

class SubscriberUtil
{

public:
    Catalog catalog;
    std::map <std::shared_ptr<MySubscribeTrackHandler>, std::shared_ptr<SubTrack>> sub_tracks;
    bool catalog_read = false;
    bool subscribed = false;

    SubscriberUtil(){};
    ~SubscriberUtil(){};

};

#endif // QUICR_SUBSCRIBER_UTIL_H
