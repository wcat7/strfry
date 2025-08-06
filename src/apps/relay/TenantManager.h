#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <set>
#include "golpe.h"

// Tenant role definitions (similar to relay29 roles)
enum class TenantRole {
    OWNER,      // Tenant owner, can manage members and delete tenant
    ADMIN,      // Administrator, can add/remove members
    MODERATOR,  // Moderator, can manage content
    MEMBER      // Regular member, can read and write
};

// Tenant member structure
struct TenantMember {
    std::string pubkey;
    TenantRole role;
    uint64_t joined_at;
    
    TenantMember() : pubkey(""), role(TenantRole::MEMBER), joined_at(0) {}
    
    TenantMember(const std::string& pk, TenantRole r, uint64_t joined) 
        : pubkey(pk), role(r), joined_at(joined) {}
};

// Tenant structure (similar to relay29 Group)
struct Tenant {
    std::string id;                    // Tenant ID (e.g., "abc", "123")
    std::string name;                  // Tenant name
    std::string description;           // Tenant description
    std::string creator;               // Creator pubkey
    uint64_t created_at;               // Creation time
    bool is_private;                   // Whether tenant is private
    uint32_t max_members;              // Maximum allowed members (0 = unlimited)
    
    std::unordered_map<std::string, TenantMember> members;  // Member list
    
    Tenant(const std::string& tenant_id, const std::string& creator_pk, uint64_t created)
        : id(tenant_id), creator(creator_pk), created_at(created), 
          is_private(false), max_members(0) {}
    
    // Check if a pubkey is a member
    bool isMember(const std::string& pubkey) const {
        return members.find(pubkey) != members.end();
    }
    
    // Check if a pubkey has a specific role or higher
    bool hasRole(const std::string& pubkey, TenantRole role) const {
        auto it = members.find(pubkey);
        if (it == members.end()) return false;
        return static_cast<int>(it->second.role) <= static_cast<int>(role);  // Lower enum value = higher privilege
    }
    
    // Add member
    void addMember(const std::string& pubkey, TenantRole role) {
        members[pubkey] = TenantMember(pubkey, role, hoytech::curr_time_us() / 1000000);
    }
    
    // Remove member
    void removeMember(const std::string& pubkey) {
        members.erase(pubkey);
    }
    
    // Get member role
    TenantRole getMemberRole(const std::string& pubkey) const {
        auto it = members.find(pubkey);
        if (it == members.end()) return TenantRole::MEMBER;  // Default role
        return it->second.role;
    }
    
    // Get all members
    std::vector<std::string> getMemberPubkeys() const {
        std::vector<std::string> pubkeys;
        for (const auto& [pk, member] : members) {
            pubkeys.push_back(pk);
        }
        return pubkeys;
    }
    
    // Get members by role
    std::vector<std::string> getMembersByRole(TenantRole role) const {
        std::vector<std::string> pubkeys;
        for (const auto& [pk, member] : members) {
            if (member.role == role) {
                pubkeys.push_back(pk);
            }
        }
        return pubkeys;
    }
    
    // Check if can add more members
    bool canAddMember() const {
        if (max_members == 0) return true;  // No limit
        return members.size() < max_members;
    }
    
    // Get current member count
    size_t getMemberCount() const {
        return members.size();
    }
    
    // Get member count by role
    size_t getMemberCountByRole(TenantRole role) const {
        size_t count = 0;
        for (const auto& [pk, member] : members) {
            if (member.role == role) {
                count++;
            }
        }
        return count;
    }
    
    // Serialize tenant to JSON for database storage
    std::string toJson() const {
        tao::json::value tenantJson = tao::json::value::object({
            {"id", id},
            {"name", name},
            {"description", description},
            {"creator", creator},
            {"created_at", created_at},
            {"is_private", is_private},
            {"max_members", max_members}
        });
        
        // Add members
        tao::json::value membersArray = tao::json::value::array({});
        for (const auto& [pk, member] : members) {
            tao::json::value memberJson = tao::json::value::object({
                {"pubkey", member.pubkey},
                {"role", static_cast<int>(member.role)},
                {"joined_at", member.joined_at}
            });
            membersArray.push_back(memberJson);
        }
        tenantJson["members"] = membersArray;
        
        return tao::json::to_string(tenantJson);
    }
    
    // Deserialize tenant from JSON
    static std::unique_ptr<Tenant> fromJson(const std::string& jsonStr) {
        try {
            auto json = tao::json::from_string(jsonStr);
            
            auto tenant = std::make_unique<Tenant>(
                json.at("id").get_string(),
                json.at("creator").get_string(),
                json.at("created_at").get_unsigned()
            );
            
            tenant->name = json.at("name").get_string();
            tenant->description = json.at("description").get_string();
            tenant->is_private = json.at("is_private").get_boolean();
            tenant->max_members = static_cast<uint32_t>(json.at("max_members").get_unsigned());
            
            // Load members
            auto membersArray = json.at("members").get_array();
            for (const auto& memberJson : membersArray) {
                std::string pubkey = memberJson.at("pubkey").get_string();
                TenantRole role = static_cast<TenantRole>(memberJson.at("role").get_unsigned());
                uint64_t joined_at = memberJson.at("joined_at").get_unsigned();
                
                tenant->members[pubkey] = TenantMember(pubkey, role, joined_at);
            }
            
            return tenant;
        } catch (const std::exception& e) {
            LI << "Failed to deserialize tenant from JSON: " << e.what();
            return nullptr;
        }
    }
};

// Tenant Manager (similar to relay29 State)
class TenantManager {
private:
    std::unordered_map<std::string, std::unique_ptr<Tenant>> tenants;
    mutable std::mutex tenantsMutex;
    
    // Default tenant for management
    std::string defaultTenantId = "default";
    
    // Database reference for persistence
    defaultDb::environment* dbEnv = nullptr;
    
public:
    TenantManager() {
        // Initialize default tenant for management
        createTenant(defaultTenantId, "", "System Management Tenant", "System management tenant for managing other tenants");
    }
    
    // Set database environment for persistence
    void setDatabase(defaultDb::environment* env) {
        dbEnv = env;
    }
    
    // Load all tenants from database
    void loadFromDatabase() {
        if (!dbEnv) {
            LI << "No database environment set for tenant manager";
            return;
        }
        
        try {
            auto txn = dbEnv->txn_ro();
            
            // Query all tenant records
            dbEnv->foreach_Tenant(txn, [this](defaultDb::environment::View_Tenant& view) {
                auto tenant = Tenant::fromJson(std::string(view.tenant_data()));
                if (tenant) {
                    tenants[tenant->id] = std::move(tenant);
                    LI << "Loaded tenant: " << view.tenant_id();
                }
                return true;
            });
            
            txn.abort();
            LI << "Loaded " << tenants.size() << " tenants from database";
            
        } catch (const std::exception& e) {
            LI << "Failed to load tenants from database: " << e.what();
        }
    }
    
    // Save tenant to database
    bool saveTenantToDatabase(const Tenant* tenant) {
        if (!dbEnv || !tenant) return false;
        
        try {
            auto txn = dbEnv->txn_rw();
            
            // Insert or update tenant record
            dbEnv->insert_Tenant(txn, tenant->id, tenant->toJson());
            
            txn.commit();
            return true;
            
        } catch (const std::exception& e) {
            LI << "Failed to save tenant to database: " << e.what();
            return false;
        }
    }
    
    // Delete tenant from database
    bool deleteTenantFromDatabase(const std::string& tenantId) {
        if (!dbEnv) return false;
        
        try {
            auto txn = dbEnv->txn_rw();
            
            // Find tenant by ID and delete it
            dbEnv->foreach_Tenant(txn, [&](defaultDb::environment::View_Tenant& view) {
                if (std::string(view.tenant_id()) == tenantId) {
                    dbEnv->delete_Tenant(txn, view.primaryKeyId);
                    return false; // Stop iteration
                }
                return true;
            });
            
            txn.commit();
            return true;
            
        } catch (const std::exception& e) {
            LI << "Failed to delete tenant from database: " << e.what();
            return false;
        }
    }
    
    // Create a new tenant
    Tenant* createTenant(const std::string& id, const std::string& creator, 
                        const std::string& name = "", const std::string& description = "",
                        uint32_t maxMembers = 0) {
        std::lock_guard<std::mutex> lock(tenantsMutex);
        
        if (tenants.find(id) != tenants.end()) {
            return nullptr;  // Tenant already exists
        }
        
        auto tenant = std::make_unique<Tenant>(id, creator, hoytech::curr_time_us() / 1000000);
        tenant->name = name.empty() ? id : name;
        tenant->description = description;
        tenant->max_members = maxMembers;
        
        // Add creator as owner
        if (!creator.empty()) {
            tenant->addMember(creator, TenantRole::OWNER);
        }
        
        Tenant* tenantPtr = tenant.get();
        tenants[id] = std::move(tenant);
        
        // Save to database
        saveTenantToDatabase(tenantPtr);
        
        return tenantPtr;
    }
    
    // Get tenant by ID
    Tenant* getTenant(const std::string& id) const {
        std::lock_guard<std::mutex> lock(tenantsMutex);
        auto it = tenants.find(id);
        return it != tenants.end() ? it->second.get() : nullptr;
    }
    
    // Delete tenant
    bool deleteTenant(const std::string& id, const std::string& requester) {
        std::lock_guard<std::mutex> lock(tenantsMutex);
        
        auto it = tenants.find(id);
        if (it == tenants.end()) return false;
        
        Tenant* tenant = it->second.get();
        
        // Only owner can delete tenant
        if (!tenant->hasRole(requester, TenantRole::OWNER)) {
            return false;
        }
        
        // Delete from database first
        deleteTenantFromDatabase(id);
        
        tenants.erase(it);
        return true;
    }
    
    // Add member to tenant
    bool addMember(const std::string& tenantId, const std::string& memberPubkey, 
                  TenantRole role, const std::string& requester) {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return false;
        
        // Only admin or owner can add members
        if (!tenant->hasRole(requester, TenantRole::ADMIN)) {
            return false;
        }
        
        // Cannot add member with higher role than requester
        if (static_cast<int>(role) < static_cast<int>(tenant->getMemberRole(requester))) {
            return false;
        }
        
        // Check member limit
        if (!tenant->canAddMember()) {
            LI << "Cannot add member to tenant " << tenantId << ": member limit reached";
            return false;
        }
        
        tenant->addMember(memberPubkey, role);
        
        // Save to database
        saveTenantToDatabase(tenant);
        
        return true;
    }
    
    // Remove member from tenant
    bool removeMember(const std::string& tenantId, const std::string& memberPubkey, 
                     const std::string& requester) {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return false;
        
        // Only admin or owner can remove members
        if (!tenant->hasRole(requester, TenantRole::ADMIN)) {
            return false;
        }
        
        // Cannot remove owner unless it's the owner themselves
        if (tenant->getMemberRole(memberPubkey) == TenantRole::OWNER && 
            memberPubkey != requester) {
            return false;
        }
        
        tenant->removeMember(memberPubkey);
        
        // Save to database
        saveTenantToDatabase(tenant);
        
        return true;
    }
    
    // Check if user can access tenant
    bool canAccessTenant(const std::string& tenantId, const std::string& pubkey) const {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return false;
        
        // Default tenant is open to everyone for management
        if (tenantId == defaultTenantId) return true;
        
        // Check if user is a member
        return tenant->isMember(pubkey);
    }
    
    // Check if user can write to tenant
    bool canWriteToTenant(const std::string& tenantId, const std::string& pubkey) const {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return false;
        
        // Default tenant has restricted write access
        if (tenantId == defaultTenantId) {
            return tenant->hasRole(pubkey, TenantRole::ADMIN);
        }
        
        // Check if user is a member
        return tenant->isMember(pubkey);
    }
    
    // Get all tenants
    std::vector<std::string> getAllTenantIds() const {
        std::lock_guard<std::mutex> lock(tenantsMutex);
        std::vector<std::string> ids;
        for (const auto& [id, tenant] : tenants) {
            ids.push_back(id);
        }
        return ids;
    }
    
    // Get tenants for a user
    std::vector<std::string> getTenantsForUser(const std::string& pubkey) const {
        std::lock_guard<std::mutex> lock(tenantsMutex);
        std::vector<std::string> ids;
        for (const auto& [id, tenant] : tenants) {
            if (tenant->isMember(pubkey)) {
                ids.push_back(id);
            }
        }
        return ids;
    }
    
    // Get tenant members
    std::vector<std::string> getTenantMembers(const std::string& tenantId) const {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return {};
        return tenant->getMemberPubkeys();
    }
    
    // Get tenant members by role
    std::vector<std::string> getTenantMembersByRole(const std::string& tenantId, TenantRole role) const {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return {};
        return tenant->getMembersByRole(role);
    }
    
    // Update tenant settings
    bool updateTenantSettings(const std::string& tenantId, const std::string& name, 
                            const std::string& description, bool isPrivate,
                            uint32_t maxMembers,
                            const std::string& requester) {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return false;
        
        // Only owner can update settings
        if (!tenant->hasRole(requester, TenantRole::OWNER)) {
            return false;
        }
        
        tenant->name = name;
        tenant->description = description;
        tenant->is_private = isPrivate;
        tenant->max_members = maxMembers;
        
        // Save to database
        saveTenantToDatabase(tenant);
        
        return true;
    }
    
    // Get tenant statistics
    struct TenantStats {
        size_t total_tenants;
        size_t total_members;
        size_t tenants_with_limits;
        size_t private_tenants;
    };
    
    TenantStats getStats() const {
        std::lock_guard<std::mutex> lock(tenantsMutex);
        
        TenantStats stats = {0, 0, 0, 0};
        
        for (const auto& [id, tenant] : tenants) {
            stats.total_tenants++;
            stats.total_members += tenant->getMemberCount();
            
            if (tenant->max_members > 0) {
                stats.tenants_with_limits++;
            }
            
            if (tenant->is_private) {
                stats.private_tenants++;
            }
        }
        
        return stats;
    }
    
    // Get tenant info for API
    struct TenantInfo {
        std::string id;
        std::string name;
        std::string description;
        std::string creator;
        uint64_t created_at;
        bool is_private;
        uint32_t max_members;
        size_t current_members;
        size_t owners;
        size_t admins;
        size_t moderators;
        size_t members;
    };
    
    TenantInfo getTenantInfo(const std::string& tenantId) const {
        Tenant* tenant = getTenant(tenantId);
        if (!tenant) return TenantInfo{};
        
        return TenantInfo{
            tenant->id,
            tenant->name,
            tenant->description,
            tenant->creator,
            tenant->created_at,
            tenant->is_private,
            tenant->max_members,
            tenant->getMemberCount(),
            tenant->getMemberCountByRole(TenantRole::OWNER),
            tenant->getMemberCountByRole(TenantRole::ADMIN),
            tenant->getMemberCountByRole(TenantRole::MODERATOR),
            tenant->getMemberCountByRole(TenantRole::MEMBER)
        };
    }
};

// Global tenant manager instance
extern TenantManager g_tenantManager; 