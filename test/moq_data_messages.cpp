// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "quicr/detail/messages.h"

#include <any>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <sys/socket.h>

using namespace quicr;
using namespace quicr::messages;
using namespace std::string_literals;

static Bytes
FromASCII(const std::string& ascii)
{
    return std::vector<uint8_t>(ascii.begin(), ascii.end());
}

const TrackNamespace kTrackNamespaceConf{ FromASCII("conf.example.com"), FromASCII("conf"), FromASCII("1") };
const Bytes kTrackNameAliceVideo = FromASCII("alice/video");
const UintVar kTrackAliasAliceVideo{ 0xA11CE };
const Extensions kExampleExtensions = { { 0x1, { 0x1, 0x2 } }, { 0x2, { 0, 0, 0, 0, 0, 0x3, 0x2, 0x1 } } };
const std::optional<Extensions> kOptionalExtensions = kExampleExtensions;

template<typename T>
bool
Verify(std::vector<uint8_t>& buffer, uint64_t message_type, T& message, [[maybe_unused]] size_t slice_depth = 1)
{
    // TODO: support Size_depth > 1, if needed
    StreamBuffer<uint8_t> in_buffer;
    in_buffer.InitAny<T>(); // Set parsed data to be of this type using out param

    std::optional<uint64_t> msg_type;
    bool done = false;

    for (auto& v : buffer) {
        auto& msg = in_buffer.GetAny<T>();
        in_buffer.Push(v);

        if (!msg_type) {
            msg_type = in_buffer.DecodeUintV();
            if (!msg_type) {
                continue;
            }
            CHECK_EQ(*msg_type, message_type);
            continue;
        }

        done = in_buffer >> msg;
        if (done) {
            // copy the working parsed data to out param.
            message = msg;
            break;
        }
    }

    return done;
}

template<typename T>
bool
VerifyCtrl(BytesSpan buffer, uint64_t message_type, T& message)
{
    uint64_t msg_type = 0;
    uint64_t length = 0;
    buffer = buffer >> msg_type;
    buffer = buffer >> length;

    CHECK_EQ(msg_type, message_type);
    CHECK_EQ(length, buffer.size());

    buffer = buffer >> message;

    return true;
}

static void
ObjectDatagramEncodeDecode(bool extensions)
{
    Bytes buffer;
    auto object_datagram = messages::ObjectDatagram{};
    object_datagram.track_alias = uint64_t(kTrackAliasAliceVideo);
    object_datagram.group_id = 0x1000;
    object_datagram.object_id = 0xFF;
    object_datagram.priority = 0xA;
    object_datagram.extensions = extensions ? kOptionalExtensions : std::nullopt;
    object_datagram.payload = { 0x1, 0x2, 0x3, 0x5, 0x6 };

    buffer << object_datagram;

    messages::ObjectDatagram object_datagram_out;
    StreamBuffer<uint8_t> sbuf;
    sbuf.Push(buffer);

    auto msg_type = sbuf.DecodeUintV();

    CHECK_EQ(msg_type, static_cast<uint64_t>(messages::DataMessageType::kObjectDatagram));

    sbuf >> object_datagram_out;

    CHECK_EQ(object_datagram.track_alias, object_datagram_out.track_alias);
    CHECK_EQ(object_datagram.group_id, object_datagram_out.group_id);
    CHECK_EQ(object_datagram.object_id, object_datagram_out.object_id);
    CHECK_EQ(object_datagram.priority, object_datagram_out.priority);
    CHECK_EQ(object_datagram.extensions, object_datagram_out.extensions);
    CHECK(object_datagram.payload.size() > 0);
    CHECK_EQ(object_datagram.payload, object_datagram_out.payload);
}

TEST_CASE("ObjectDatagram  Message encode/decode")
{
    ObjectDatagramEncodeDecode(false);
    ObjectDatagramEncodeDecode(true);
}

TEST_CASE("ObjectDatagramStatus  Message encode/decode")
{
    Bytes buffer;
    auto object_datagram_status = messages::ObjectDatagramStatus{};
    object_datagram_status.track_alias = uint64_t(kTrackAliasAliceVideo);
    object_datagram_status.group_id = 0x1000;
    object_datagram_status.object_id = 0xFF;
    object_datagram_status.priority = 0xA;
    object_datagram_status.status = quicr::ObjectStatus::kAvailable;

    buffer << object_datagram_status;

    messages::ObjectDatagramStatus object_datagram_status_out;
    CHECK(Verify(
      buffer, static_cast<uint64_t>(messages::DataMessageType::kObjectDatagramStatus), object_datagram_status_out));
    CHECK_EQ(object_datagram_status.track_alias, object_datagram_status_out.track_alias);
    CHECK_EQ(object_datagram_status.group_id, object_datagram_status_out.group_id);
    CHECK_EQ(object_datagram_status.object_id, object_datagram_status_out.object_id);
    CHECK_EQ(object_datagram_status.priority, object_datagram_status_out.priority);
    CHECK_EQ(object_datagram_status.status, object_datagram_status_out.status);
}

static void
StreamPerSubGroupObjectEncodeDecode(bool extensions, bool empty_payload)
{
    Bytes buffer;
    auto hdr_grp = messages::StreamHeaderSubGroup{};
    hdr_grp.track_alias = uint64_t(kTrackAliasAliceVideo);
    hdr_grp.group_id = 0x1000;
    hdr_grp.subgroup_id = 0x5000;
    hdr_grp.priority = 0xA;

    buffer << hdr_grp;

    messages::StreamHeaderSubGroup hdr_group_out;
    CHECK(Verify(buffer, static_cast<uint64_t>(messages::DataMessageType::kStreamHeaderSubgroup), hdr_group_out));
    CHECK_EQ(hdr_grp.track_alias, hdr_group_out.track_alias);
    CHECK_EQ(hdr_grp.group_id, hdr_group_out.group_id);
    CHECK_EQ(hdr_grp.subgroup_id, hdr_group_out.subgroup_id);

    // stream all the objects
    buffer.clear();
    auto objects = std::vector<messages::StreamSubGroupObject>{};
    // send 10 objects
    for (size_t i = 0; i < 10; i++) {
        auto obj = messages::StreamSubGroupObject{};
        obj.object_id = 0x1234;

        if (empty_payload) {
            obj.object_status = ObjectStatus::kDoesNotExist;
        } else {
            obj.payload = { 0x1, 0x2, 0x3, 0x4, 0x5 };
        }

        obj.extensions = extensions ? kOptionalExtensions : std::nullopt;
        objects.push_back(obj);
        buffer << obj;
    }

    auto obj_out = messages::StreamSubGroupObject{};
    size_t object_count = 0;
    StreamBuffer<uint8_t> in_buffer;
    for (size_t i = 0; i < buffer.size(); i++) {
        in_buffer.Push(buffer.at(i));
        bool done = in_buffer >> obj_out;
        if (!done) {
            continue;
        }
        CHECK_EQ(obj_out.object_id, objects[object_count].object_id);
        if (empty_payload) {
            CHECK_EQ(obj_out.object_status, objects[object_count].object_status);
        } else {
            CHECK(obj_out.payload.size() > 0);
            CHECK_EQ(obj_out.payload, objects[object_count].payload);
        }
        CHECK_EQ(obj_out.extensions, objects[object_count].extensions);
        // got one object
        object_count++;
        obj_out = {};
        in_buffer.Pop(in_buffer.Size());
    }

    CHECK_EQ(object_count, objects.size());
}

TEST_CASE("StreamPerSubGroup Object  Message encode/decode")
{
    StreamPerSubGroupObjectEncodeDecode(false, true);
    StreamPerSubGroupObjectEncodeDecode(false, false);
    StreamPerSubGroupObjectEncodeDecode(true, true);
    StreamPerSubGroupObjectEncodeDecode(true, false);
}

static void
FetchStreamEncodeDecode(bool extensions, bool empty_payload)
{
    Bytes buffer;
    auto fetch_header = messages::FetchHeader{};
    fetch_header.subscribe_id = 0x1234;

    buffer << fetch_header;

    messages::FetchHeader fetch_header_out;
    CHECK(Verify(buffer, static_cast<uint64_t>(messages::DataMessageType::kFetchHeader), fetch_header_out));
    CHECK_EQ(fetch_header.subscribe_id, fetch_header_out.subscribe_id);

    // stream all the objects
    buffer.clear();
    auto objects = std::vector<messages::FetchObject>{};
    // send 10 objects
    for (size_t i = 0; i < 10; i++) {
        auto obj = messages::FetchObject{};
        obj.group_id = 0x1234;
        obj.subgroup_id = 0x5678;
        obj.object_id = 0x9012;
        obj.publisher_priority = 127;

        if (empty_payload) {
            obj.object_status = ObjectStatus::kDoesNotExist;
        } else {
            obj.payload = { 0x1, 0x2, 0x3, 0x4, 0x5 };
        }

        obj.extensions = extensions ? kOptionalExtensions : std::nullopt;
        objects.push_back(obj);
        buffer << obj;
    }

    auto obj_out = messages::FetchObject{};
    size_t object_count = 0;
    StreamBuffer<uint8_t> in_buffer;
    for (size_t i = 0; i < buffer.size(); i++) {
        in_buffer.Push(buffer.at(i));
        bool done = in_buffer >> obj_out;
        if (!done) {
            continue;
        }
        CHECK_EQ(obj_out.group_id, objects[object_count].group_id);
        CHECK_EQ(obj_out.subgroup_id, objects[object_count].subgroup_id);
        CHECK_EQ(obj_out.object_id, objects[object_count].object_id);
        CHECK_EQ(obj_out.publisher_priority, objects[object_count].publisher_priority);
        if (empty_payload) {
            CHECK_EQ(obj_out.object_status, objects[object_count].object_status);
        } else {
            CHECK(obj_out.payload.size() > 0);
            CHECK_EQ(obj_out.payload, objects[object_count].payload);
        }
        CHECK_EQ(obj_out.extensions, objects[object_count].extensions);
        // got one object
        object_count++;
        obj_out = {};
        in_buffer.Pop(in_buffer.Size());
    }

    CHECK_EQ(object_count, objects.size());
}

TEST_CASE("Fetch Stream Message encode/decode")
{
    FetchStreamEncodeDecode(false, true);
    FetchStreamEncodeDecode(false, false);
    FetchStreamEncodeDecode(true, true);
    FetchStreamEncodeDecode(true, false);
}
