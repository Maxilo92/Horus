#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include "Common.hpp"

// Persisted face identity (kept separate from ReID Entities; different embedding type)
struct FaceRecord {
    int                id = -1;
    std::string        name;
    std::vector<float> embedding;
};

class DossierDatabase {
public:
    DossierDatabase(const std::string& dbPath);
    ~DossierDatabase();

    // Prevent copying
    DossierDatabase(const DossierDatabase&) = delete;
    DossierDatabase& operator=(const DossierDatabase&) = delete;

    bool init();

    // Entity Operations
    bool upsertEntity(const DossierEntry& entry);
    bool getEntityByUUID(const std::string& uuid, DossierEntry& outEntry);
    bool findNearestEntity(const std::vector<float>& embedding, const std::string& type, float threshold, DossierEntry& outEntry);
    bool updateDossierText(const std::string& uuid, const std::string& text);
    
    // Sightings
    bool addSighting(const std::string& entityUuid, const std::string& sessionId);

    // Face identity persistence (separate Faces table)
    bool upsertFace(int id, const std::string& name, const std::vector<float>& embedding);
    bool updateFaceName(int id, const std::string& name);
    std::vector<FaceRecord> getAllFaces() const;
    int getMaxFaceId() const;

    // Global stats
    int getEntityCount() const;
    std::vector<DossierEntry> getAllEntities() const;

private:
    std::string m_dbPath;
    sqlite3* m_db = nullptr;
    mutable std::mutex m_mutex;

    bool executeQuery(const std::string& query);
    static std::vector<float> blobToVector(const void* blob, int size);
    static std::string vectorToBlob(const std::vector<float>& vec);
};
