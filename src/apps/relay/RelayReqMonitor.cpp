#include "RelayServer.h"

#include "ActiveMonitors.h"



void RelayServer::runReqMonitor(ThreadPool<MsgReqMonitor>::Thread &thr) {
    // Multi-tenant monitors - one per subdomain
    std::unordered_map<std::string, std::unique_ptr<ActiveMonitors>> monitorsBySubdomain;
    std::unordered_map<std::string, uint64_t> currEventIds;
    std::unordered_map<std::string, std::unique_ptr<hoytech::file_change_monitor>> dbChangeWatchers;
    
    Decompressor decomp;

    while (1) {
        auto newMsgs = thr.inbox.pop_all();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReqMonitor::NewSub>(&newMsg.msg)) {
                auto connId = msg->sub.connId;
                auto& subdomain = msg->subdomain;
                
                // Get or create monitor for this subdomain
                auto& monitors = monitorsBySubdomain[subdomain];
                if (!monitors) {
                    monitors = std::make_unique<ActiveMonitors>();
                    currEventIds[subdomain] = MAX_U64;
                    
                    // Create DB change watcher for this subdomain
                    auto& tenantEnv = getTenantEnv(subdomain);
                    std::string tenantDbPath = dbDir + "/tenants/" + subdomain + "/data.mdb";
                    dbChangeWatchers[subdomain] = std::make_unique<hoytech::file_change_monitor>(tenantDbPath);
                    dbChangeWatchers[subdomain]->setDebounce(100);
                    
                    // Start watching for DB changes
                    dbChangeWatchers[subdomain]->run([&, subdomain](){
                        tpReqMonitor.dispatchToAll([subdomain]{ return MsgReqMonitor{MsgReqMonitor::DBChange{subdomain}}; });
                    });
                }
                
                auto& tenantEnv = getTenantEnv(subdomain);
                auto txn = tenantEnv.txn_ro();
                
                uint64_t latestEventId = getMostRecentLevId(txn);
                if (currEventIds[subdomain] > latestEventId) currEventIds[subdomain] = latestEventId;

                tenantEnv.foreach_Event(txn, [&](auto &ev){
                    if (msg->sub.filterGroup.doesMatch(PackedEventView(ev.buf))) {
                        sendEvent(connId, msg->sub.subId, getEventJson(txn, decomp, ev.primaryKeyId));
                    }

                    return true;
                }, false, msg->sub.latestEventId + 1);

                msg->sub.latestEventId = latestEventId;

                if (!monitors->addSub(txn, std::move(msg->sub), latestEventId)) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }
            } else if (auto msg = std::get_if<MsgReqMonitor::RemoveSub>(&newMsg.msg)) {
                // Find which subdomain this subscription belongs to
                for (auto& [subdomain, monitors] : monitorsBySubdomain) {
                    monitors->removeSub(msg->connId, msg->subId);
                }
            } else if (auto msg = std::get_if<MsgReqMonitor::CloseConn>(&newMsg.msg)) {
                // Close connection in all subdomains
                for (auto& [subdomain, monitors] : monitorsBySubdomain) {
                    monitors->closeConn(msg->connId);
                }
            } else if (auto msg = std::get_if<MsgReqMonitor::DBChange>(&newMsg.msg)) {
                auto& subdomain = msg->subdomain;
                auto it = monitorsBySubdomain.find(subdomain);
                if (it != monitorsBySubdomain.end()) {
                    auto& monitors = it->second;
                    auto& tenantEnv = getTenantEnv(subdomain);
                    auto txn = tenantEnv.txn_ro();
                    
                    uint64_t latestEventId = getMostRecentLevId(txn);
                    
                    tenantEnv.foreach_Event(txn, [&](auto &ev){
                        monitors->process(txn, ev, [&](RecipientList &&recipients, uint64_t levId){
                            sendEventToBatch(std::move(recipients), std::string(getEventJson(txn, decomp, levId)));
                        });
                        return true;
                    }, false, currEventIds[subdomain] + 1);

                    currEventIds[subdomain] = latestEventId;
                }
            }
        }
    }
}
