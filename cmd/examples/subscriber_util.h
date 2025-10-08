//
// Created by schweitzer on 2025. 09. 29..
//

#pragma once

#ifndef QUICR_SUBSCRIBER_UTIL_H
#define QUICR_SUBSCRIBER_UTIL_H

#include "catalog.hpp"

struct SubTrack
{
    TrackEntry track_entry;
    std::string namespace_;

};

class SubscriberUtil
{

public:
    std::vector<std::shared_ptr<SubTrack>> sub_tracks;
    bool catalog_read = false;
    bool subscribed = false;

    SubscriberUtil(){};
    ~SubscriberUtil(){};

};

#endif // QUICR_SUBSCRIBER_UTIL_H
