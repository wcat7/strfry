#pragma once

#include <iostream>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <mutex>

#include <hoytech/time.h>
#include <hoytech/hex.h>
#include <hoytech/file_change_monitor.h>
#include <uWebSockets/src/uWS.h>
#include <tao/json.hpp>

#include "golpe.h"
#include "TenantManager.h"

#include "Subscription.h"
#include "ThreadPool.h"
#include "events.h"
#include "filters.h"
#include "jsonParseUtils.h"
#include "Decompressor.h"




struct MsgWebsocket : NonCopyable {
    struct Send {
        uint64_t connId;
        std::string payload;
    };

    struct SendBinary {
        uint64_t connId;
        std::string payload;
    };

    struct SendEventToBatch {
        RecipientList list;
        std::string evJson;
    };

    struct GracefulShutdown {
    };

    using Var = std::variant<Send, SendBinary, SendEventToBatch, GracefulShutdown>;
    Var msg;
    MsgWebsocket(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgIngester : NonCopyable {
    struct ClientMessage {
        uint64_t connId;
        std::string ipAddr;
        std::string subdomain;  // Add subdomain for multi-tenant support
        std::string payload;
    };

    struct CloseConn {
        uint64_t connId;
    };

    using Var = std::variant<ClientMessage, CloseConn>;
    Var msg;
    MsgIngester(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgWriter : NonCopyable {
    struct AddEvent {
        uint64_t connId;
        std::string ipAddr;
        std::string subdomain;  // Add subdomain for multi-tenant support
        std::string packedStr;
        std::string jsonStr;
    };

    struct CloseConn {
        uint64_t connId;
    };

    using Var = std::variant<AddEvent, CloseConn>;
    Var msg;
    MsgWriter(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgReqWorker : NonCopyable {
    struct NewSub {
        Subscription sub;
        std::string subdomain;  // Add subdomain for multi-tenant support
    };

    struct RemoveSub {
        uint64_t connId;
        SubId subId;
    };

    struct CloseConn {
        uint64_t connId;
    };

    using Var = std::variant<NewSub, RemoveSub, CloseConn>;
    Var msg;
    MsgReqWorker(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgReqMonitor : NonCopyable {
    struct NewSub {
        Subscription sub;
        std::string subdomain;  // Add subdomain for multi-tenant support
    };

    struct RemoveSub {
        uint64_t connId;
        SubId subId;
    };

    struct CloseConn {
        uint64_t connId;
    };

    struct DBChange {
        std::string subdomain;  // Add subdomain for multi-tenant support
    };

    using Var = std::variant<NewSub, RemoveSub, CloseConn, DBChange>;
    Var msg;
    MsgReqMonitor(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgNegentropy : NonCopyable {
    struct NegOpen {
        Subscription sub;
        std::string subdomain;  // Add subdomain for multi-tenant support
        std::string filterStr;
        std::string negPayload;
    };

    struct NegMsg {
        uint64_t connId;
        SubId subId;
        std::string negPayload;
    };

    struct NegClose {
        uint64_t connId;
        SubId subId;
    };

    struct CloseConn {
        uint64_t connId;
    };

    using Var = std::variant<NegOpen, NegMsg, NegClose, CloseConn>;
    Var msg;
    MsgNegentropy(Var &&msg_) : msg(std::move(msg_)) {}
};

// NIP-42 AUTH support
struct AuthStatus {
    std::string challenge;
    std::string authed;
};


struct RelayServer {
    uS::Async *hubTrigger = nullptr;

    // Multi-tenant database management
    std::unordered_map<std::string, std::unique_ptr<defaultDb::environment>> tenantEnvs;
    std::mutex tenantEnvsMutex;
    
    // Get or create database environment for a tenant
    defaultDb::environment& getTenantEnv(const std::string& subdomain);
    
    // Extract subdomain from host header
    std::string extractSubdomain(const std::string& host, const std::string& path = "");
    
    // Clean up unused tenant databases
    void cleanupUnusedTenants();

    // Thread Pools

    ThreadPool<MsgWebsocket> tpWebsocket;
    ThreadPool<MsgIngester> tpIngester;
    ThreadPool<MsgWriter> tpWriter;
    ThreadPool<MsgReqWorker> tpReqWorker;
    ThreadPool<MsgReqMonitor> tpReqMonitor;
    ThreadPool<MsgNegentropy> tpNegentropy;
    std::thread cronThread;
    std::thread signalHandlerThread;

    void run();

    void runWebsocket(ThreadPool<MsgWebsocket>::Thread &thr);

    void runIngester(ThreadPool<MsgIngester>::Thread &thr);
    void ingesterProcessEvent(lmdb::txn &txn, uint64_t connId, flat_hash_map<uint64_t, AuthStatus*> &connIdToAuthStatus, std::string ipAddr, std::string subdomain, secp256k1_context *secpCtx, const tao::json::value &origJson, std::vector<MsgWriter> &output);
    void ingesterProcessReq(lmdb::txn &txn, uint64_t connId, std::string subdomain, const tao::json::value &origJson);
    void ingesterProcessClose(lmdb::txn &txn, uint64_t connId, const tao::json::value &origJson);
    void ingesterProcessAuth(uint64_t connId, flat_hash_map<uint64_t, AuthStatus*> connIdToAuthStatus, secp256k1_context *secpCtx, const tao::json::value &eventJson);
    void ingesterProcessNegentropy(lmdb::txn &txn, Decompressor &decomp, uint64_t connId, std::string subdomain, const tao::json::value &origJson);

    void runWriter(ThreadPool<MsgWriter>::Thread &thr);

    void runReqWorker(ThreadPool<MsgReqWorker>::Thread &thr);

    void runReqMonitor(ThreadPool<MsgReqMonitor>::Thread &thr);

    void runNegentropy(ThreadPool<MsgNegentropy>::Thread &thr);

    void runCron();

    void runSignalHandler();

    // Utils (can be called by any thread)

    void sendToConn(uint64_t connId, std::string &&payload) {
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(payload)}});
        hubTrigger->send();
    }

    void sendToConnBinary(uint64_t connId, std::string &&payload) {
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::SendBinary{connId, std::move(payload)}});
        hubTrigger->send();
    }

    void sendEvent(uint64_t connId, const SubId &subId, std::string_view evJson) {
        auto subIdSv = subId.sv();

        std::string reply;
        reply.reserve(13 + subIdSv.size() + evJson.size());

        reply += "[\"EVENT\",\"";
        reply += subIdSv;
        reply += "\",";
        reply += evJson;
        reply += "]";

        sendToConn(connId, std::move(reply));
    }

    void sendEventToBatch(RecipientList &&list, std::string &&evJson) {
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::SendEventToBatch{std::move(list), std::move(evJson)}});
        hubTrigger->send();
    }

    void sendNoticeError(uint64_t connId, std::string &&payload) {
        LI << "sending error to [" << connId << "]: " << payload;
        auto reply = tao::json::value::array({ "NOTICE", std::string("ERROR: ") + payload });
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(tao::json::to_string(reply))}});
        hubTrigger->send();
    }

    void sendOKResponse(uint64_t connId, std::string_view eventIdHex, bool written, std::string_view message) {
        auto reply = tao::json::value::array({ "OK", eventIdHex, written, message });
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(tao::json::to_string(reply))}});
        hubTrigger->send();
    }

    void sendAuthChallenge(uint64_t connId, std::string_view challenge) {
        auto reply = tao::json::value::array({ "AUTH", challenge });
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(tao::json::to_string(reply))}});
        hubTrigger->send();
    }
};
