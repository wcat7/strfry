#include "RelayServer.h"
#include "TenantManager.h"
#include <filesystem>
#include <algorithm>

// Extract subdomain from host header or URL path
std::string RelayServer::extractSubdomain(const std::string& host, const std::string& path) {
    // First try to extract from path (e.g., /abc -> abc)
    if (!path.empty() && path != "/") {
        std::string pathSubdomain = path.substr(1); // Remove leading slash
        
        // Remove trailing slash if present
        if (!pathSubdomain.empty() && pathSubdomain.back() == '/') {
            pathSubdomain.pop_back();
        }
        
        // Validate path subdomain (alphanumeric and hyphens only)
        if (!pathSubdomain.empty() && pathSubdomain.length() <= 63) {
            bool valid = true;
            for (char c : pathSubdomain) {
                if (!std::isalnum(c) && c != '-') {
                    valid = false;
                    break;
                }
            }
            
            // Don't allow subdomains that start or end with hyphen
            if (valid && pathSubdomain.front() != '-' && pathSubdomain.back() != '-') {
                return pathSubdomain;
            }
        }
    } else if (path == "/") {
        // Root path should use default tenant
        return "default";
    }
    
    // Fallback to host-based subdomain extraction
    // Remove port if present
    size_t colonPos = host.find(':');
    std::string hostname = (colonPos != std::string::npos) ? host.substr(0, colonPos) : host;
    
    // Find the first dot to extract subdomain
    size_t dotPos = hostname.find('.');
    if (dotPos == std::string::npos) {
        // No subdomain found, use "default" as tenant
        return "default";
    }
    
    std::string subdomain = hostname.substr(0, dotPos);
    
    // Validate subdomain (alphanumeric and hyphens only)
    if (subdomain.empty() || subdomain.length() > 63) {
        return "default";
    }
    
    for (char c : subdomain) {
        if (!std::isalnum(c) && c != '-') {
            return "default";
        }
    }
    
    // Don't allow subdomains that start or end with hyphen
    if (subdomain.front() == '-' || subdomain.back() == '-') {
        return "default";
    }
    
    return subdomain;
}

// Get or create database environment for a tenant
defaultDb::environment& RelayServer::getTenantEnv(const std::string& subdomain) {
    std::lock_guard<std::mutex> lock(tenantEnvsMutex);
    
    auto it = tenantEnvs.find(subdomain);
    if (it != tenantEnvs.end()) {
        return *it->second;
    }
    
    // Check if tenant exists in tenant manager (for non-default tenants)
    if (subdomain != "default") {
        Tenant* tenant = g_tenantManager.getTenant(subdomain);
        if (!tenant) {
            // Tenant doesn't exist in manager, create it automatically
            // This allows backward compatibility with existing tenants
            tenant = g_tenantManager.createTenant(subdomain, "", subdomain, "Auto-created tenant", 0);
            LI << "Auto-created tenant: " << subdomain;
        }
    }
    
    // Create new tenant database
    std::string tenantDbDir = dbDir + "/tenants/" + subdomain;
    
    // Create directory if it doesn't exist
    std::filesystem::create_directories(tenantDbDir);
    
    // Create new environment
    auto newEnv = std::make_unique<defaultDb::environment>();
    
    unsigned int dbFlags = 0;
    if (cfg().dbParams__noReadAhead) dbFlags |= MDB_NORDAHEAD;
    
    // Use custom LMDB setup if configured
    if (cfg().dbParams__maxreaders > 0 || cfg().dbParams__mapsize > 0) {
        newEnv->lmdb_env.set_max_dbs(64);
        newEnv->lmdb_env.set_max_readers(cfg().dbParams__maxreaders);
        newEnv->lmdb_env.set_mapsize(cfg().dbParams__mapsize);
        newEnv->open(tenantDbDir, false, dbFlags);
    } else {
        newEnv->open(tenantDbDir, true, dbFlags);
    }
    
    // Initialize the database
    {
        auto txn = newEnv->txn_rw();
        
        // Check if database needs initialization
        auto s = newEnv->lookup_Meta(txn, 1);
        if (!s) {
            newEnv->insert_Meta(txn, CURR_DB_VERSION, 1, 1);
            newEnv->insert_NegentropyFilter(txn, "{}");
                        
            // Set up Negentropy database for this tenant
            negentropy::storage::BTreeLMDB::setupDB(txn, "negentropy");
        }
        
        txn.commit();
    }
    
    LI << "Created new tenant database for subdomain: " << subdomain << " at " << tenantDbDir;
    
    // Store the environment
    auto& envRef = *newEnv;
    tenantEnvs[subdomain] = std::move(newEnv);
    
    return envRef;
}

// Clean up unused tenant databases
void RelayServer::cleanupUnusedTenants() {
    // This function can be called periodically to clean up unused tenant databases
    // For now, we'll keep all databases in memory
    // In a production environment, you might want to implement LRU eviction
    // or other cleanup strategies based on usage patterns
} 